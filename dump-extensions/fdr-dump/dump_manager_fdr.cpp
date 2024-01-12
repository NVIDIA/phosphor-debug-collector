#include "config.h"

#include "dump_manager_fdr.hpp"

#include "dump_internal.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <regex>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>
#include <string>

#include "dump-extensions/fdr-dump/fdr_dump_config.h"

namespace phosphor
{
namespace dump
{
namespace FDR
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

void Manager::limitDumpEntries()
{
    // Delete dumps only when FDR dump max limit is configured
#if FDR_DUMP_MAX_LIMIT == 0
    // Do nothing - FDR dump max limit is not configured
    return;
#else  // #if FDR_DUMP_MAX_LIMIT == 0
    // Delete dumps on reaching allowed entries
    auto totalDumps = entries.size();
    if (totalDumps < FDR_DUMP_MAX_LIMIT)
    {
        // Do nothing - Its within allowed entries
        return;
    }
    // Get the oldest dumps
    int excessDumps = totalDumps - (FDR_DUMP_MAX_LIMIT - 1);
    // Delete the oldest dumps
    for (auto d = entries.begin(); d != entries.end() && excessDumps; d++)
    {
        auto& entry = d->second;
        entry->delete_();
        --excessDumps;
    }

    return;
#endif // #if FDR_DUMP_MAX_LIMIT == 0
}

sdbusplus::message::object_path
    Manager::createDump(std::map<std::string, std::string> params)
{
    // Limit dumps to max allowed entries
    limitDumpEntries();
    auto id = captureDump(params);

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        std::time_t timeStamp = std::time(nullptr);
        entries.insert(std::make_pair(
            id, std::make_unique<FDR::Entry>(
                    bus, objPath.c_str(), id, timeStamp, 0, std::string(),
                    phosphor::dump::OperationStatus::InProgress, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating FDR dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id));
        elog<InternalFailure>();
    }

    return objPath.string();
}

static void fdrDumpGetActionArgument(std::map<std::string, std::string>& params,
                                     std::string& action)
{
    const std::string key = "Action";
    const std::string cleanAction = "Clean";
    const std::string collectAction = "Collect";

    if (auto search = params.find(key); search != params.end())
    {
        if (search->second == cleanAction)
        {
            action = "clean";
        }
        else if (search->second == collectAction)
        {
            action = "collect";
        }
        else
        {
            log<level::ERR>("System dump: Unsupport argument for action");
        }
    }
}

uint32_t fdrDump(const std::string& dumpId, const std::string& dumpPath,
                 const std::string& action)
{
    // Construct FDR dump arguments
    std::vector<char*> arg_v;
    std::string fPath = FDR_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string aOption = "-a";
    arg_v.push_back(&aOption[0]);
    arg_v.push_back(const_cast<char*>(action.c_str()));

    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // FDR Dump execution is failed.
    auto error = errno;
    log<level::ERR>(
        "FDR dump: Error occurred during fdr dump function execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t Manager::captureDump(std::map<std::string, std::string> params)
{
    // check if minimum required space is available on destination partition
    std::error_code ec{};
    fs::path partitionPath(dumpDir);

#if (JFFS_SPACE_CALC_INACCURACY_OFFSET_WORKAROUND_PERCENT > 0)
    /* jffs2 space available problem is worked around by substracting 2%
       of capacity from currently available space, eg. 200M - 4M = 196M
       it solves problem of failed dump when user request it close to space
       limit so instead if silently failing the task user receives appropriate
       message. Test it yourself - fill up the partition until 'no space left'
       message appears, check `df -T` for available space, if there seems to be
       at least 1% space available then you just reproduced the issue*/
    uintmax_t offset = (fs::space(partitionPath, ec).capacity *
                        JFFS_SPACE_CALC_INACCURACY_OFFSET_WORKAROUND_PERCENT) /
                       100;
    uintmax_t spaceAvailable = fs::space(partitionPath, ec).available;
    uintmax_t sizeLeftKb = 0;
    if (spaceAvailable >= offset)
    {
        sizeLeftKb = (spaceAvailable - offset) / 1024;
    }
#else
    uintmax_t sizeLeftKb = fs::space(partitionPath, ec).available / 1024;
#endif
    uintmax_t reqSizeKb = FDR_DUMP_MIN_SPACE_REQD;

    if (ec.value() != 0)
    {
        log<level::ERR>("Failed to check available space");
        elog<InternalFailure>();
    }

    if (sizeLeftKb < reqSizeKb)
    {
        log<level::ERR>(
            "Not enough space available to create FDR dump",
            entry("REQ_KB=%d", static_cast<unsigned int>(reqSizeKb)),
            entry("LEFT_KB=%d", static_cast<unsigned int>(sizeLeftKb)));
        using QuotaExceeded =
            sdbusplus::xyz::openbmc_project::Dump::Create::Error::QuotaExceeded;
        using Reason =
            xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }

    // Validate request argument
    const std::string typeFDR = "FDR";
    auto diagnosticType = params["DiagnosticType"];
    params.erase("DiagnosticType");
    if (!diagnosticType.empty())
    {
        if (diagnosticType != typeFDR)
        {
            log<level::ERR>("Unrecognized DiagnosticType option",
                            entry("DIAG_TYPE=%s", diagnosticType.c_str()));
            using INV_ARG =
                xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
            using INV_VAL =
                xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
            elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                                  INV_VAL(diagnosticType.c_str()));
        }
    }
    else
    {
        log<level::ERR>("Empty DiagnosticType option",
                        entry("DIAG_TYPE=%s", diagnosticType.c_str()));
        using INV_ARG =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
        using INV_VAL =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
        elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                              INV_VAL(diagnosticType.c_str()));
    }

    pid_t pid = fork();

    if (pid == 0)
    {
        fs::path dumpPath(dumpDir);
        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        if (diagnosticType == typeFDR)
        {
            std::string action = "collect";

            fdrDumpGetActionArgument(params, action);
            fdrDump(id, dumpPath, action);
        }
        else
        {
            log<level::ERR>("FDR dump: Invalid DiagnosticType");
            elog<InternalFailure>();
        }
    }
    else if (pid > 0)
    {
        auto entryId = lastEntryId + 1;
        Child::Callback callback = [this, pid, entryId](Child&,
                                                        const siginfo_t* si) {
            if (si->si_status != 0)
            {
                std::string msg = "Dump process failed: (signo)" +
                                  std::to_string(si->si_signo) + "; (code)" +
                                  std::to_string(si->si_code) + "; (errno)" +
                                  std::to_string(si->si_errno) + "; (pid)" +
                                  std::to_string(si->si_pid) + "; (status)" +
                                  std::to_string(si->si_status);
                log<level::ERR>(msg.c_str());
                this->createDumpFailed(entryId);
            }

            this->childPtrMap.erase(pid);
        };

        try
        {
            childPtrMap.emplace(pid,
                                std::make_unique<Child>(eventLoop.get(), pid,
                                                        WEXITED | WSTOPPED,
                                                        std::move(callback)));
        }
        catch (const sdeventplus::SdEventError& ex)
        {
            // Failed to add to event loop
            log<level::ERR>(
                fmt::format(
                    "Error occurred during the sdeventplus::source::Child "
                    "creation ex({})",
                    ex.what())
                    .c_str());
            elog<InternalFailure>();
        }
    }
    else
    {
        auto error = errno;
        log<level::ERR>("FDR dump: Error occurred during fork",
                        entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }

    return ++lastEntryId;
}

void Manager::createEntry(const fs::path& file)
{
    // Dump File Name format obmcdump_ID_EPOCHTIME.EXT
    static constexpr auto ID_POS = 1;
    static constexpr auto EPOCHTIME_POS = 2;
    std::regex file_regex("obmcdump_([0-9]+)_([0-9]+).([a-zA-Z0-9]+)");

    std::smatch match;
    std::string name = file.filename();

    if (!((std::regex_search(name, match, file_regex)) && (match.size() > 0)))
    {
        log<level::ERR>("FDR dump: Invalid Dump file name",
                        entry("FILENAME=%s", file.filename().c_str()));
        return;
    }

    auto idString = match[ID_POS];
    auto msString = match[EPOCHTIME_POS];

    auto id = stoul(idString);

    // If there is an existing entry update it and return.
    auto dumpEntry = entries.find(id);
    if (dumpEntry != entries.end())
    {
        dynamic_cast<phosphor::dump::FDR::Entry*>(dumpEntry->second.get())
            ->update(stoull(msString), fs::file_size(file), file);
        return;
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        entries.insert(std::make_pair(
            id,
            std::make_unique<FDR::Entry>(
                bus, objPath.c_str(), id, stoull(msString), fs::file_size(file),
                file, phosphor::dump::OperationStatus::Completed, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating FDR dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id),
                        entry("TIMESTAMP=%ull", stoull(msString)),
                        entry("SIZE=%d", fs::file_size(file)),
                        entry("FILENAME=%s", file.c_str()));
        return;
    }
}

void Manager::watchCallback(const UserMap& fileInfo)
{
    for (const auto& i : fileInfo)
    {
        // For any new dump file create dump entry object
        // and associated inotify watch.
        if (IN_CLOSE_WRITE == i.second)
        {
            if (!std::filesystem::is_directory(i.first))
            {
                // Don't require filename to be passed, as the path
                // of dump directory is stored in the childWatchMap
                removeWatch(i.first.parent_path());
                // dump file is written now create D-Bus entry
                createEntry(i.first);
            }
            else
            {
                removeWatch(i.first);
            }
        }
        // Start inotify watch on newly created directory.
        else if ((IN_CREATE == i.second) && fs::is_directory(i.first))
        {
            auto watchObj = std::make_unique<Watch>(
                eventLoop, IN_NONBLOCK, IN_CLOSE_WRITE, EPOLLIN, i.first,
                std::bind(
                    std::mem_fn(&phosphor::dump::FDR::Manager::watchCallback),
                    this, std::placeholders::_1));

            childWatchMap.emplace(i.first, std::move(watchObj));
        }
    }
}

void Manager::removeWatch(const fs::path& path)
{
    // Delete Watch entry from map.
    childWatchMap.erase(path);
}

void Manager::restore()
{
    fs::path dir(dumpDir);
    if (!fs::exists(dir) || fs::is_empty(dir))
    {
        return;
    }

    // Dump file path: <DUMP_PATH>/<id>/<filename>
    for (const auto& p : fs::directory_iterator(dir))
    {
        auto idStr = p.path().filename().string();

        // Consider only directory's with dump id as name.
        // Note: As per design one file per directory.
        if ((fs::is_directory(p.path())) &&
            std::all_of(idStr.begin(), idStr.end(), ::isdigit))
        {
            lastEntryId =
                std::max(lastEntryId, static_cast<uint32_t>(std::stoul(idStr)));
            auto fileIt = fs::directory_iterator(p.path());
            // Create dump entry d-bus object.
            if (fileIt != fs::end(fileIt))
            {
                createEntry(fileIt->path());
            }
        }
    }
}

size_t Manager::getAllowedSize()
{
    using namespace sdbusplus::xyz::openbmc_project::Dump::Create::Error;
    using Reason = xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;

    auto size = 0;

    // Get current size of the dump directory.
    for (const auto& p : fs::recursive_directory_iterator(dumpDir))
    {
        if (!fs::is_directory(p))
        {
            size += fs::file_size(p);
        }
    }

    // Convert size into KB
    size = size / 1024;

    // Set the Dump size to Maximum  if the free space is greater than
    // Dump max size otherwise return the available size.

    size = (size > FDR_DUMP_TOTAL_SIZE ? 0 : FDR_DUMP_TOTAL_SIZE - size);

    if (size < FDR_DUMP_MIN_SPACE_REQD)
    {
        // Reached to maximum limit
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }
    if (size > FDR_DUMP_MAX_SIZE)
    {
        size = FDR_DUMP_MAX_SIZE;
    }

    return size;
}

} // namespace FDR
} // namespace dump
} // namespace phosphor
