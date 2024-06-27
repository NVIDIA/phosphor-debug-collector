/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <regex>

#include "dump-extensions/faultlog-dump/faultlog_dump_config.h"

#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

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
    for (auto d = entries.begin(); d != entries.end() && excessDumps; d++)
    {
        auto& entry = d->second;
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
    for (auto d = entries.begin(); d != entries.end(); d++)
    {
        auto& entry = d->second;
        entry->delete_();
        size = getAllowedSize();
        if (size < FAULTLOG_DUMP_MIN_SPACE_REQD)
        {
            break;
        }
    }
#else
    using namespace sdbusplus::xyz::openbmc_project::Dump::Create::Error;
    using Reason = xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
    elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
#endif
}

sdbusplus::message::object_path Manager::createDump(phosphor::dump::DumpCreateParams params)
{
    // Limit dumps to max allowed entries
    limitDumpEntries();
    // Limit dumps to max allowed size
    limitTotalDumpSize();

    const auto& [id, type, additionalTypeName, primayLogId] = captureDump(params);

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);
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
            id,
            std::make_unique<faultLog::Entry>(
                bus, objPath.c_str(), id, timeStamp, type, additionalTypeName,
                primayLogId, 0, std::string(),
                phosphor::dump::OperationStatus::InProgress, sectionType, fruID,
                severity, nvipSignature, nvSeverity, nvSocketNumber, pcieVendorID,
                pcieDeviceID, pcieClassCode, pcieFunctionNumber, pcieDeviceNumber,
                pcieSegmentNumber, pcieDeviceBusNumber, pcieSecondaryBusNumber,
                pcieSlotNumber, originatorId, originatorType, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>(
            "Error in creating system dump entry", entry("OBJECTPATH=%s", objPath.c_str()), entry("ID=%d", id));
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

FaultLogEntryInfo
    Manager::captureDump(phosphor::dump::DumpCreateParams params)
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
        auto rc = sd_event_add_child(eventLoop.get(), nullptr, pid,
                                     WEXITED | WSTOPPED, callback, nullptr);
        if (0 > rc)
        {
            // Failed to add to event loop
            log<level::ERR>("FaultLog dump: Error occurred during the "
                            "sd_event_add_child call",
                            entry("RC=%d", rc));
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
    std::regex file_regex("obmcdump_([0-9]+)_([0-9]+).([a-zA-Z0-9]+)");

    std::smatch match;
    std::string name = file.filename();
 
    if (!((std::regex_search(name, match, file_regex)) && (match.size() > 0)))
    {
        log<level::ERR>("System dump: Invalid Dump file name",
                        entry("FILENAME=%s", file.filename().c_str()));
        return;
    }

    std::string sectionType = "NA";
    std::string fruid = "NA";
    std::string severity = "NA";
    std::string nvipSignature = "NA";
    std::string nvSeverity = "NA";
    int nvSockNumber;
    std::string nvSocketNumber = "NA";
    std::string pcieVendorID = "NA";
    std::string pcieDeviceID = "NA";
    std::string pcieClassCode = "NA";
    std::string pcieFunctionNumber = "NA";
    std::string pcieDeviceNumber = "NA";
    std::string pcieSegmentNumber = "NA";
    std::string pcieDeviceBusNumber = "NA";
    std::string pcieSecondaryBusNumber = "NA";
    int pcieSlotNum;
    std::string pcieSlotNumber = "NA";

    auto idString = match[ID_POS];
    auto msString = match[EPOCHTIME_POS];

    auto id = stoul(idString);

    // If there is an existing entry update it and return.
    auto dumpEntry = entries.find(id);
    if (dumpEntry != entries.end())
    {
          dynamic_cast<phosphor::dump::faultLog::Entry*>(dumpEntry->second.get())
            ->update(stoull(msString), fs::file_size(file), file, std::to_string(id));
        
        return;
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    std::string cperDecodePath = "/var/lib/logging/dumps/faultlog/" + std::to_string(id) + "/Decoded/decoded.json";
    std::ifstream cperFile(cperDecodePath.c_str());

    if (cperFile.is_open())
    {
        json jsonData;
        jsonData = json::parse(cperFile, nullptr, false);
        if (!jsonData.is_discarded())
        {
            if (jsonData.contains("Header") && jsonData["Header"].contains("Section Count"))
            {
                int secCount = jsonData["Header"]["Section Count"];

                for (int i = 0; i < secCount; i++)
                {
                    if (jsonData.contains("Sections") && jsonData["Sections"].is_array() &&
                        !jsonData["Sections"].empty())
                    {
                        if (jsonData["Sections"][i].contains("Section Descriptor"))
                        {
                            // Extracting existing fields
                            if (jsonData["Sections"][i]["Section Descriptor"].contains("Section Type")){
                                sectionType = jsonData["Sections"][i]["Section Descriptor"]["Section Type"];}

                            if (jsonData["Sections"][i]["Section Descriptor"].contains("FRU Id")){
                                fruid = jsonData["Sections"][i]["Section Descriptor"]["FRU Id"];}

                            if (jsonData["Sections"][i]["Section Descriptor"].contains("Section Severity")){
                                severity = jsonData["Sections"][i]["Section Descriptor"]["Section Severity"];}

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("IPSignature")){
                                nvipSignature = jsonData["Sections"][i]["Section"]["IPSignature"];}

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("Severity")){
                                nvSeverity = jsonData["Sections"][i]["Section"]["Severity"];}

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("Socket Number"))
                            {
                                nvSockNumber = jsonData["Sections"][i]["Section"]["Socket Number"];
                                nvSocketNumber = std::to_string(nvSockNumber);
                            }

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("Device ID"))
                            {
                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Vendor ID")){
                                    pcieVendorID = jsonData["Sections"][i]["Section"]["Device ID"]["Vendor ID"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Device ID")){
                                    pcieDeviceID = jsonData["Sections"][i]["Section"]["Device ID"]["Device ID"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Class Code")){
                                    pcieClassCode = jsonData["Sections"][i]["Section"]["Device ID"]["Class Code"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Function Number")){
                                    pcieFunctionNumber = jsonData["Sections"][i]["Section"]["Device ID"]["Function Number"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Device Number")){
                                    pcieDeviceNumber = jsonData["Sections"][i]["Section"]["Device ID"]["Device Number"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Segment Number")){
                                    pcieSegmentNumber = jsonData["Sections"][i]["Section"]["Device ID"]["Segment Number"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Device Bus Number")){
                                    pcieDeviceBusNumber = jsonData["Sections"][i]["Section"]["Device ID"]["Device Bus Number"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Secondary Bus Number")){
                                    pcieSecondaryBusNumber = jsonData["Sections"][i]["Section"]["Device ID"]["Secondary Bus Number"];}

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Slot Number"))
                                {
                                    pcieSlotNum = jsonData["Sections"][i]["Section"]["Device ID"]["Slot Number"];
                                    pcieSlotNumber = std::to_string(pcieSlotNum);
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
                    bus, objPath.c_str(), id, stoull(msString),
                    FaultDataType::CPER, "CPER", "0", fs::file_size(file), file,
                    phosphor::dump::OperationStatus::Completed,
                    sectionType, fruid, severity, nvipSignature, nvSeverity,
                    nvSocketNumber, pcieVendorID, pcieDeviceID, pcieClassCode,
                    pcieFunctionNumber, pcieDeviceNumber, pcieSegmentNumber,
                    pcieDeviceBusNumber, pcieSecondaryBusNumber, pcieSlotNumber,originatorId, originatorType, *this)));
    }

    catch (const std::invalid_argument &e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating FaultLog dump entry",
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

    size =
        (size > FAULTLOG_DUMP_TOTAL_SIZE ? 0 : FAULTLOG_DUMP_TOTAL_SIZE - size);

    if (size > FAULTLOG_DUMP_MAX_SIZE)
    {
        size = FAULTLOG_DUMP_MAX_SIZE;
    }

    return size;
}

} // namespace faultLog
} // namespace dump
} // namespace phosphor
