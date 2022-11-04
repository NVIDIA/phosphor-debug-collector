#include "config.h"

#include "dump_manager_system.hpp"

#include "system_dump_entry.hpp"
#include "dump_internal.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <regex>
#include <chrono>

namespace phosphor
{
namespace dump
{
namespace system
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

// TODO: Merge system dump with bmc dump to avoid code duplication.

void Manager::limitDumpEntries()
{
    // Delete dumps only when system dump max limit is configured
#if SYSTEM_DUMP_MAX_LIMIT == 0
    // Do nothing - system dump max limit is not configured
    return;
#else // #if SYSTEM_DUMP_MAX_LIMIT == 0
    // Delete dumps on reaching allowed entries
    auto totalDumps = entries.size();
    if (totalDumps < SYSTEM_DUMP_MAX_LIMIT)
    {
        // Do nothing - Its within allowed entries
        return;
    }
    // Get the oldest dumps
    int excessDumps = totalDumps - (SYSTEM_DUMP_MAX_LIMIT-1);
    // Delete the oldest dumps
    for (auto d = entries.begin(); d != entries.end() && excessDumps;
            d++) {
        auto& entry = d->second;
        entry->delete_();
        --excessDumps;
    }

    return;
#endif // #if SYSTEM_DUMP_MAX_LIMIT == 0
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
            id, std::make_unique<system::Entry>(
                    bus, objPath.c_str(), id, timeStamp, 0, std::string(),
                    phosphor::dump::OperationStatus::InProgress, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating system dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id));
        elog<InternalFailure>();
    }

    return objPath.string();
}

// captureDump helper functions
uint32_t executeDreport
(
    const std::string& dumpType,
    const std::string& dumpId,
    const std::string& dumpPath,
    const size_t size,
    const std::vector<std::string>& addArgs
)
{
    // Construct dreport arguments
    std::vector<char*> arg_v;
    std::string fPath = "/usr/bin/dreport";
    arg_v.push_back(&fPath[0]);
    std::string dOption = "-d";
    arg_v.push_back(&dOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string sOption = "-s";
    arg_v.push_back(&sOption[0]);
    arg_v.push_back(const_cast<char*>(std::to_string(size).c_str()));
    std::string qOption = "-q";
    arg_v.push_back(&qOption[0]);
    std::string vOption = "-v";
    arg_v.push_back(&vOption[0]);
    std::string tOption = "-t";
    arg_v.push_back(&tOption[0]);
    arg_v.push_back(const_cast<char*>(dumpType.c_str()));
    // Add additional arguments
    std::string aOption = "-a";
    for(int i=0; i < (int) addArgs.size(); i++)
    {
        arg_v.push_back(&aOption[0]);
        arg_v.push_back(const_cast<char*>((addArgs.at(i)).c_str()));
    }
    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // dreport script execution is failed.
    auto error = errno;
    log<level::ERR>("System dump: Error occurred during dreport function execution",
                    entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t selfTest
(
    const std::string& dumpId,
    const std::string& dumpPath
)
{
    // Construct selftest dump arguments
    std::vector<char*> arg_v;
    std::string fPath = SELFTEST_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string dOption = "-v";
    arg_v.push_back(&dOption[0]);

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // self test execution is failed.
    auto error = errno;
    log<level::ERR>("System dump: Error occurred during self test execution",
                    entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t fpgaRegDump
(
    const std::string& dumpId,
    const std::string& dumpPath
)
{
    // Construct fpga dump arguments
    std::vector<char*> arg_v;
    std::string fPath = FPGA_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // FPGA register dump execution is failed.
    auto error = errno;
    log<level::ERR>("System dump: Error occurred during FPGA register dump execution",
                    entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t Manager::captureDump(std::map<std::string, std::string> params)
{
    // check if minimum required space is available on destination partition
    std::error_code ec{};
    fs::path partitionPath(dumpDir);
    uintmax_t sizeLeftKb = fs::space(partitionPath, ec).available / 1024;
    uintmax_t reqSizeKb = SYSTEM_DUMP_MIN_SPACE_REQD;

    if (ec.value() != 0)
    {
        log<level::ERR>("Failed to check available space");
        elog<InternalFailure>();
    }

    if (sizeLeftKb < reqSizeKb)
    {
        log<level::ERR>(
            "Not enough space available to create system dump",
            entry("REQ_KB=%d", static_cast<unsigned int>(reqSizeKb)),
            entry("LEFT_KB=%d", static_cast<unsigned int>(sizeLeftKb)));
        using QuotaExceeded =
            sdbusplus::xyz::openbmc_project::Dump::Create::Error::QuotaExceeded;
        using Reason =
            xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }

    // Get Dump size.
    auto size = getAllowedSize();

    pid_t pid = fork();

    if (pid == 0)
    {
        fs::path dumpPath(dumpDir);
        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        std::string dumpType = "system";

        // Extract dump configs
        auto diagnosticType = params["DiagnosticType"];
        params.erase("DiagnosticType");

        // Construct additional arguments from params
        std::vector<std::string> addArgs;
        std::map<std::string, std::string>::iterator itr;
        for (itr = params.begin(); itr != params.end(); ++itr) {
            addArgs.push_back(itr->first + "=" + itr->second);
        }

        if (diagnosticType.empty())
        {
            executeDreport(dumpType, id, dumpPath, size, addArgs);
        }
        else if (diagnosticType == "SelfTest")
        {
            selfTest(id, dumpPath);
        }
        else if (diagnosticType == "FPGA")
        {
            fpgaRegDump(id, dumpPath);
        }
        else
        {
            log<level::ERR>("System dump: Invalid DiagnosticType");
            elog<InternalFailure>();
        }
    }
    else if (pid > 0)
    {
        auto rc = sd_event_add_child(eventLoop.get(), nullptr, pid,
                                     WEXITED | WSTOPPED, callback, nullptr);
        if (0 > rc)
        {
            // Failed to add to event loop
            log<level::ERR>("System dump: Error occurred during the sd_event_add_child call",
                            entry("RC=%d", rc));
            elog<InternalFailure>();
        }
    }
    else
    {
        auto error = errno;
        log<level::ERR>("System dump: Error occurred during fork", entry("ERRNO=%d", error));
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
        log<level::ERR>("System dump: Invalid Dump file name",
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
        dynamic_cast<phosphor::dump::system::Entry*>(dumpEntry->second.get())
            ->update(stoull(msString), fs::file_size(file), file);
        return;
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        entries.insert(
            std::make_pair(id, std::make_unique<system::Entry>(
                bus, objPath.c_str(), id, stoull(msString), fs::file_size(file),
                file, phosphor::dump::OperationStatus::Completed, *this)));

    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating system dump entry",
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
            removeWatch(i.first);

            createEntry(i.first);
        }
        // Start inotify watch on newly created directory.
        else if ((IN_CREATE == i.second) && fs::is_directory(i.first))
        {
            auto watchObj = std::make_unique<Watch>(
                eventLoop, IN_NONBLOCK, IN_CLOSE_WRITE, EPOLLIN, i.first,
                std::bind(
                    std::mem_fn(&phosphor::dump::system::Manager::watchCallback),
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

    size = (size > SYSTEM_DUMP_TOTAL_SIZE ? 0 : SYSTEM_DUMP_TOTAL_SIZE - size);

    if (size < SYSTEM_DUMP_MIN_SPACE_REQD)
    {
        // Reached to maximum limit
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }
    if (size > SYSTEM_DUMP_MAX_SIZE)
    {
        size = SYSTEM_DUMP_MAX_SIZE;
    }

    return size;
}

} // namespace system
} // namespace dump
} // namespace phosphor
