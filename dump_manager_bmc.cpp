#include "config.h"

#include "dump_manager_bmc.hpp"

#include "bmc_dump_entry.hpp"
#include "dump_internal.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <ctime>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>

#include <cmath>
#include <ctime>
#include <regex>

namespace phosphor
{
namespace dump
{
namespace bmc
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

namespace internal
{

void Manager::create(Type type, std::vector<std::string> fullPaths)
{
    // Limit dumps to max allowed entries
    dumpMgr.phosphor::dump::bmc::Manager::limitDumpEntries(type);
    auto dumpPGID = 0;
    dumpMgr.phosphor::dump::bmc::Manager::captureDump(type, fullPaths,
                                                      dumpPGID);
}

} // namespace internal

void Manager::limitDumpEntries(Type type)
{
    // Delete dumps only when bmc dump max limit is configured
    if (BMC_DUMP_MAX_LIMIT == 0 && BMC_CORE_DUMP_MAX_LIMIT == 0)
    {
        // Do nothing - bmc dump max limit is not configured
        return;
    }
    // Delete dumps on reaching allowed entries
    int totalBmcDumps = 0;
    int totalCoreDumps = 0;
    std::string coreDir = TypeMap.find(Type::ApplicationCored)->second;

    for (auto& entry : entries)
    {
        auto path =
            dynamic_cast<phosphor::dump::bmc::Entry*>(entry.second.get())
                ->getFileName()
                .string();
        if (path.find(coreDir) != std::string::npos)
        {
            ++totalCoreDumps;
        }
        else
        {
            ++totalBmcDumps;
        }
    }

    if (totalBmcDumps < BMC_DUMP_MAX_LIMIT &&
        totalCoreDumps < BMC_CORE_DUMP_MAX_LIMIT)
    {
        // Do nothing - Its within allowed entries
        return;
    }

    // Get the oldest dumps
    int excessBmcDumps = 0;
    int excessCoreDumps = 0;

    if (totalBmcDumps >= BMC_DUMP_MAX_LIMIT && BMC_DUMP_MAX_LIMIT &&
        type != Type::ApplicationCored)
    {
        excessBmcDumps = totalBmcDumps - (BMC_DUMP_MAX_LIMIT - 1);
    }

    if (totalCoreDumps >= BMC_CORE_DUMP_MAX_LIMIT && BMC_CORE_DUMP_MAX_LIMIT &&
        type == Type::ApplicationCored)
    {
        excessCoreDumps = totalCoreDumps - (BMC_CORE_DUMP_MAX_LIMIT - 1);
    }

    log<level::WARNING>(fmt::format("Excess dumps to be deleted, "
                                    "excessBmcDumps({}), excessCoreDumps({})",
                                    excessBmcDumps, excessCoreDumps)
                            .c_str());

    // Delete the oldest dumps
    for (auto d = entries.begin();
         d != entries.end() && (excessCoreDumps || excessBmcDumps); d++)
    {
        auto& entry = d->second;
        auto path = dynamic_cast<phosphor::dump::bmc::Entry*>(entry.get())
                        ->getFileName()
                        .string();

        if (path.find(coreDir) != std::string::npos && excessCoreDumps)
        {
            entry->delete_();
            --excessCoreDumps;
        }
        else if (path.find(coreDir) == std::string::npos && excessBmcDumps)
        {
            entry->delete_();
            --excessBmcDumps;
        }
    }

    return;
}

bool Manager::checkDumpCreationInProgress()
{
    for (auto d = entries.begin(); d != entries.end(); d++)
    {
        auto& entry = d->second;
        uint64_t elapsed = static_cast<uint64_t>(
            difftime(std::time(nullptr), entry->startTime()));
        // Time check is required because if dump creation fails due to external
        // plug-in erros then status would remain as InProgress with the current
        // desing
        if (entry->status() == phosphor::dump::OperationStatus::InProgress &&
            elapsed < bmcDumpMaxTimeLimitInSec)
        {
            return true;
        }
    }
    return false;
}

sdbusplus::message::object_path
    Manager::createDump(std::map<std::string, std::string> params)
{
    if (!params.empty())
    {
        log<level::WARNING>("BMC dump accepts no additional parameters");
    }
    // don't allows simultaneously dump creation
    if (checkDumpCreationInProgress())
    {
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
        return std::string();
    }

    // Limit dumps to max allowed entries
    limitDumpEntries(Type::UserRequested);
    std::vector<std::string> paths;
    pid_t dumpProcessGroupId = 0;
    auto id = captureDump(Type::UserRequested, paths, dumpProcessGroupId);

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        std::time_t timeStamp = std::time(nullptr);
        entries.insert(std::make_pair(
            id, std::make_unique<bmc::Entry>(
                    bus, objPath.c_str(), id, timeStamp, 0, std::string(),
                    phosphor::dump::OperationStatus::InProgress, *this,
                    dumpProcessGroupId)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(fmt::format("Error in creating dump entry, "
                                    "errormsg({}), OBJECTPATH({}), ID({})",
                                    e.what(), objPath.c_str(), id)
                            .c_str());
        elog<InternalFailure>();
    }

    return objPath.string();
}

uint32_t Manager::captureDump(Type type,
                              const std::vector<std::string>& fullPaths,
                              pid_t& dumpPGID)
{
    // Get Dump size.
    auto size = getAllowedSize();

    log<level::INFO>(
        fmt::format("Capturing BMC dump of type ({})",
            TypeMap.find(type)->second)
                .c_str());

    pid_t pid = fork();

    if (pid == 0)
    {
        // used to detach from previous process group to properly kill dreport
        // process group and leaving original dump manager running
        setpgid(0, 0);

        // get dreport type map entry
        auto tempType = TypeMap.find(type);
        fs::path dumpPath(dumpDir);

        // Core dumps are stored in core directory
        if (type == Type::ApplicationCored)
        {
            dumpPath /= tempType->second;
        }

        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        execl("/usr/bin/dreport", "dreport", "-d", dumpPath.c_str(), "-i",
              id.c_str(), "-s", std::to_string(size).c_str(), "-q", "-v", "-p",
              fullPaths.empty() ? "" : fullPaths.front().c_str(), "-t",
              tempType->second.c_str(), nullptr);

        // dreport script execution is failed.
        auto error = errno;
        log<level::ERR>(
            fmt::format(
                "Error occurred during dreport function execution, errno({})",
                error)
                .c_str());
        elog<InternalFailure>();
    }
    else if (pid > 0)
    {
        auto entryId = lastEntryId + 1;
        Child::Callback callback = [this, type, pid, entryId](Child&, const siginfo_t*) {
            this->childPtrMap.erase(pid);
            this->clearEntryGroupProcessId(entryId);
        };

        dumpPGID = pid;

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
        log<level::ERR>(
            fmt::format("Error occurred during fork, errno({})", error)
                .c_str());
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
        log<level::ERR>(fmt::format("Invalid Dump file name, FILENAME({})",
                                    file.filename().c_str())
                            .c_str());
        return;
    }

    auto idString = match[ID_POS];
    auto msString = match[EPOCHTIME_POS];

    auto id = stoul(idString);

    // If there is an existing entry update it and return.
    auto dumpEntry = entries.find(id);
    if (dumpEntry != entries.end())
    {          
        dynamic_cast<phosphor::dump::bmc::Entry*>(dumpEntry->second.get())
            ->update(stoull(msString), fs::file_size(file), file);
        return;
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        entries.insert(std::make_pair(
            id,
            std::make_unique<bmc::Entry>(
                bus, objPath.c_str(), id, stoull(msString), fs::file_size(file),
                file, phosphor::dump::OperationStatus::Completed, *this, 0)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(
            fmt::format(
                "Error in creating dump entry, errormsg({}), OBJECTPATH({}), "
                "ID({}), TIMESTAMP({}), SIZE({}), FILENAME({})",
                e.what(), objPath.c_str(), id, stoull(msString),
                std::filesystem::file_size(file), file.filename().c_str())
                .c_str());
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
                    std::mem_fn(&phosphor::dump::bmc::Manager::watchCallback),
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
    fs::path dumpPath(dumpDir);
    restoreDir(dumpPath);

    auto tempType = TypeMap.find(Type::ApplicationCored);
    dumpPath /= tempType->second;

    restoreDir(dumpPath);
}

void Manager::restoreDir(fs::path dir)
{
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
            auto dirEntryId = static_cast<uint32_t>(std::stoul(idStr));
            if (dirEntryId != lastEntryId)
            {
                lastEntryId = std::max(lastEntryId, dirEntryId);
                auto fileIt = fs::directory_iterator(p.path());
                // Create dump entry d-bus object.
                if (fileIt != fs::end(fileIt))
                {               
                    createEntry(fileIt->path());
                }
            }
            else
            {
                log<level::ERR>(
                    fmt::format("Bmc duplicate dump entry found, EntryId({})",
                                dirEntryId)
                        .c_str());
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
            size += std::ceil(std::filesystem::file_size(p) / 1024.0);
        }
    }

    // Set the Dump size to Maximum  if the free space is greater than
    // Dump max size otherwise return the available size.

    size = (size > BMC_DUMP_TOTAL_SIZE ? 0 : BMC_DUMP_TOTAL_SIZE - size);

    if (size < BMC_DUMP_MIN_SPACE_REQD)
    {
        // Reached to maximum limit
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }
    if (size > BMC_DUMP_MAX_SIZE)
    {
        size = BMC_DUMP_MAX_SIZE;
    }

    return size;
}

} // namespace bmc
} // namespace dump
} // namespace phosphor
