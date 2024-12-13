/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"

#include "dump_manager_faultlog.hpp"

#include "dump_utils.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <regex>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>
#include <string>

#include "dump-extensions/faultlog-dump/faultlog_dump_config.h"

using json = nlohmann::json;

namespace phosphor
{
namespace dump
{
namespace faultLog
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

void Manager::limitDumpEntries()
{
    // Delete dumps only when fault log dump max limit is configured
#if FAULTLOG_DUMP_MAX_LIMIT == 0
    // Do nothing - fault log dump max limit is not configured
    return;
#else  // #if FAULTLOG_DUMP_MAX_LIMIT == 0
    // Delete dumps on reaching allowed entries
    auto totalDumps = entries.size();
    if (totalDumps < FAULTLOG_DUMP_MAX_LIMIT)
    {
        // Do nothing - Its within allowed entries
        return;
    }
    // Get the oldest dumps
    int excessDumps = totalDumps - (FAULTLOG_DUMP_MAX_LIMIT - 1);
    // Delete the oldest dumps
    auto d = entries.begin();
    while (d != entries.end() && excessDumps)
    {
        auto& entry = d->second;
        d++;
        entry->delete_();
        --excessDumps;
    }

    return;
#endif // #if FAULTLOG_DUMP_MAX_LIMIT == 0
}

void Manager::limitTotalDumpSize()
{
    auto size = getAllowedSize();
    if (size >= FAULTLOG_DUMP_MIN_SPACE_REQD)
    {
        return;
    }

    // Reached to maximum limit
#ifdef FAULTLOG_DUMP_ROTATION
    log<level::ERR>("Not enough space: Deleting oldest dumps");

    // Delete the oldest dumps till free size is enough
    auto d = entries.begin();
    while (d != entries.end())
    {
        auto& entry = d->second;
        d++;
        entry->delete_();
        size = getAllowedSize();
        if (size >= FAULTLOG_DUMP_MIN_SPACE_REQD)
        {
            break;
        }
    }

    if (!entries.size() && !getAllowedSize())
    {
        using namespace sdbusplus::xyz::openbmc_project::Dump::Create::Error;
        using Reason =
            xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }
#else
    using namespace sdbusplus::xyz::openbmc_project::Dump::Create::Error;
    using Reason = xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
    elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
#endif
}

sdbusplus::message::object_path
    Manager::createDump(phosphor::dump::DumpCreateParams params)
{
    // Limit dumps to max allowed entries
    limitDumpEntries();
    // Limit dumps to max allowed size
    limitTotalDumpSize();

    const auto& [id, type, additionalTypeName,
                 primayLogId] = captureDump(params);

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);
    std::string notifType = "NA";
    std::string sectionType = "NA";
    std::string fruID = "NA";
    std::string severity = "NA";
    std::string nvipSignature = "NA";
    std::string nvSeverity = "NA";
    std::string nvSocketNumber = "NA";
    std::string pcieVendorID = "NA";
    std::string pcieDeviceID = "NA";
    std::string pcieClassCode = "NA";
    std::string pcieFunctionNumber = "NA";
    std::string pcieDeviceNumber = "NA";
    std::string pcieSegmentNumber = "NA";
    std::string pcieDeviceBusNumber = "NA";
    std::string pcieSecondaryBusNumber = "NA";
    std::string pcieSlotNumber = "NA";

    try
    {
        // Get the originator id and type from params
        std::string originatorId;
        originatorTypes originatorType;

        phosphor::dump::extractOriginatorProperties(params, originatorId,
                                                    originatorType);
        std::time_t timeStamp = std::time(nullptr);
        entries.insert(std::make_pair(
            id, std::make_unique<faultLog::Entry>(
                    bus, objPath.c_str(), id, timeStamp, type,
                    additionalTypeName, primayLogId, 0, std::string(),
                    phosphor::dump::OperationStatus::InProgress, notifType,
                    sectionType, fruID, severity, nvipSignature, nvSeverity,
                    nvSocketNumber, pcieVendorID, pcieDeviceID, pcieClassCode,
                    pcieFunctionNumber, pcieDeviceNumber, pcieSegmentNumber,
                    pcieDeviceBusNumber, pcieSecondaryBusNumber, pcieSlotNumber,
                    originatorId, originatorType, *this)));
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

uint32_t cperDump(const std::string& dumpId, const std::string& dumpPath,
                  const std::string& cperPath)
{
    // Construct fpga dump arguments
    std::vector<char*> arg_v;
    std::string cPath = CPER_DUMP_BIN_PATH;
    arg_v.push_back(&cPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string sOption = "-s";
    arg_v.push_back(&sOption[0]);
    arg_v.push_back(const_cast<char*>(cperPath.c_str()));

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // CPER dump execution is failed.
    auto error = errno;
    log<level::ERR>("CPER dump: Error occurred during CPER dump execution",
                    entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

FaultLogEntryInfo Manager::captureDump(phosphor::dump::DumpCreateParams params)
{
    FaultDataType type{};
    std::string additionalTypeName{};
    std::string primaryLogId{};
    std::string cperPath{};

    auto findCperType = params.find("CPER_TYPE");
    auto findCperPath = params.find("CPER_PATH");
    if (findCperType != params.end() && findCperPath != params.end())
    {
        type = FaultDataType::CPER;
        additionalTypeName = std::get<std::string>(findCperType->second);
        primaryLogId = std::to_string(++lastCperId);
        cperPath = std::get<std::string>(findCperPath->second);
        params.erase("CPER_TYPE");
        params.erase("CPER_PATH");
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        if (type == FaultDataType::CPER)
        {
            fs::path dumpPath(dumpDir);
            auto id = std::to_string(lastEntryId + 1);
            dumpPath /= id;
            cperDump(id, dumpPath, cperPath);
        }
        else
        {
            log<level::ERR>("FaultLog dump: Invalid FaultDataType");
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
        log<level::ERR>("FaultLog dump: Error occurred during fork",
                        entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }

    return std::make_tuple(++lastEntryId, type, additionalTypeName,
                           primaryLogId);
}

void Manager::createEntry(const fs::path& file)
{
    // Dump File Name format obmcdump_ID_EPOCHTIME.EXT
    static constexpr auto ID_POS = 1;
    static constexpr auto EPOCHTIME_POS = 2;
    std::regex file_regex("obmcdump_([0-9]+)_([0-9]+)\\.([a-zA-Z0-9]+)");

    std::smatch match;
    std::string name = file.filename();

    if (!((std::regex_search(name, match, file_regex)) && (match.size() > 0)))
    {
        log<level::ERR>("System dump: Invalid Dump file name",
                        entry("FILENAME=%s", file.filename().c_str()));
        return;
    }

    std::string notifType = "NA";
    std::string sectionType = "NA";
    std::string fruid = "NA";
    std::string severity = "NA";
    std::string nvipSignature = "NA";
    std::string nvSeverity = "NA";
    std::string nvSocketNumber = "NA";
    std::string pcieVendorID = "NA";
    std::string pcieDeviceID = "NA";
    std::string pcieClassCode = "NA";
    std::string pcieFunctionNumber = "NA";
    std::string pcieDeviceNumber = "NA";
    std::string pcieSegmentNumber = "NA";
    std::string pcieDeviceBusNumber = "NA";
    std::string pcieSecondaryBusNumber = "NA";
    std::string pcieSlotNumber = "NA";

    auto idString = match[ID_POS];
    uint64_t timestamp = stoull(match[EPOCHTIME_POS]) * 1000 * 1000;

    auto id = stoul(idString);

    // If there is an existing entry update it and return.
    auto dumpEntry = entries.find(id);
    if (dumpEntry != entries.end())
    {
        dynamic_cast<phosphor::dump::faultLog::Entry*>(dumpEntry->second.get())
            ->update(timestamp, fs::file_size(file), file, std::to_string(id));

        return;
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    std::string cperDecodePath = "/var/lib/logging/dumps/faultlog/" +
                                 std::to_string(id) + "/Decoded/decoded.json";
    std::ifstream cperFile(cperDecodePath.c_str());

    if (cperFile.is_open())
    {
        json jsonData = json::parse(cperFile, nullptr, false);
        if (!jsonData.is_discarded())
        {
            if (jsonData.contains("Header"))
            {
                const json& hdr = jsonData["Header"];

                if (hdr.contains("NotificationType"))
                    notifType = hdr["NotificationType"];

                if (hdr.contains("SectionCount"))
                {
                    // int secCount = hdr["SectionCount"];  // unused
                    if (jsonData.contains("Sections") &&
                        jsonData["Sections"].is_array() &&
                        jsonData["Sections"].size() > 0)
                    {
                        // TODO: Log multiple-sections

                        // Log only the 1st section (most CPERs have only 1)
                        const json& toLog = jsonData["Sections"][0];

                        if (toLog.contains("SectionDescriptor"))
                        {
                            const json& descToLog = toLog["SectionDescriptor"];

                            // Extracting existing fields
                            if (descToLog.contains("SectionType"))
                                sectionType = descToLog["SectionType"];

                            if (descToLog.contains("FRUId"))
                                fruid = descToLog["FRUId"];

                            if (descToLog.contains("SectionSeverity"))
                                severity = descToLog["SectionSeverity"];

                            if (toLog.contains("Section"))
                            {
                                const json& sectionToLog = toLog["Section"];

                                if (sectionToLog.contains("IPSignature"))
                                    nvipSignature = sectionToLog["IPSignature"];

                                if (sectionToLog.contains("Severity"))
                                    nvSeverity = sectionToLog["Severity"];

                                if (sectionToLog.contains("SocketNumber"))
                                    nvSocketNumber =
                                        sectionToLog["SocketNumber"].dump();

                                if (sectionToLog.contains("DeviceID"))
                                {
                                    const json& devID =
                                        sectionToLog["DeviceID"];

                                    if (devID.contains("VendorID"))
                                        pcieVendorID = devID["VendorID"];

                                    if (devID.contains("DeviceID"))
                                        pcieDeviceID = devID["DeviceID"];

                                    if (devID.contains("ClassCode"))
                                        pcieClassCode = devID["ClassCode"];

                                    if (devID.contains("FunctionNumber"))
                                        pcieFunctionNumber =
                                            devID["FunctionNumber"];

                                    if (devID.contains("DeviceNumber"))
                                        pcieDeviceNumber =
                                            devID["DeviceNumber"];

                                    if (devID.contains("SegmentNumber"))
                                        pcieSegmentNumber =
                                            devID["SegmentNumber"];

                                    if (devID.contains("DeviceBusNumber"))
                                        pcieDeviceBusNumber =
                                            devID["DeviceBusNumber"];

                                    if (devID.contains("SecondaryBusNumber"))
                                        pcieSecondaryBusNumber =
                                            devID["SecondaryBusNumber"];

                                    if (devID.contains("SlotNumber"))
                                        pcieSlotNumber =
                                            devID["SlotNumber"].dump();
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        log<level::ERR>("Unable to open file");
    }

    try
    {
        // Get the originator id and type from params
        std::string originatorId;
        originatorTypes originatorType;

        entries.insert(std::make_pair(
            id, std::make_unique<faultLog::Entry>(
                    bus, objPath.c_str(), id, timestamp, FaultDataType::CPER,
                    "CPER", "0", fs::file_size(file), file,
                    phosphor::dump::OperationStatus::Completed, notifType,
                    sectionType, fruid, severity, nvipSignature, nvSeverity,
                    nvSocketNumber, pcieVendorID, pcieDeviceID, pcieClassCode,
                    pcieFunctionNumber, pcieDeviceNumber, pcieSegmentNumber,
                    pcieDeviceBusNumber, pcieSecondaryBusNumber, pcieSlotNumber,
                    originatorId, originatorType, *this)));
    }

    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating FaultLog dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id), entry("TIMESTAMP=%ull", timestamp),
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
                    std::mem_fn(
                        &phosphor::dump::faultLog::Manager::watchCallback),
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
            lastEntryId = std::max(lastEntryId,
                                   static_cast<uint32_t>(std::stoul(idStr)));
            for (const auto& fileIt : fs::directory_iterator(p.path()))
            {
                // Create dump entry d-bus object.
                if (fs::is_regular_file(fileIt))
                {
                    createEntry(fileIt.path());
                }
            }
        }
    }
}

size_t calculate_directory_size(const fs::path& directory)
{
    size_t size = 0;

    for (const auto& entry : fs::directory_iterator(directory))
    {
        if (fs::is_directory(entry))
        {
            size += calculate_directory_size(entry.path());
        }
        else if (fs::is_regular_file(entry))
        {
            size += fs::file_size(entry);
        }
    }
    return size;
}

size_t Manager::getAllowedSize()
{
    size_t size = 0;

    try
    {
        size = calculate_directory_size(dumpDir);
    }
    catch (const fs::filesystem_error& e)
    {
        log<level::ERR>(
            fmt::format(
                "An error occurred during the calculation e({}), and the size 0 is returned.",
                e.what())
                .c_str());
        return 0;
    }

    // Convert size into KB
    size = size / 1024;

    // Set the Dump size to Maximum  if the free space is greater than
    // Dump max size otherwise return the available size.

    size = (size > FAULTLOG_DUMP_TOTAL_SIZE ? 0
                                            : FAULTLOG_DUMP_TOTAL_SIZE - size);

    if (size > FAULTLOG_DUMP_MAX_SIZE)
    {
        size = FAULTLOG_DUMP_MAX_SIZE;
    }

    return size;
}

} // namespace faultLog
} // namespace dump
} // namespace phosphor
