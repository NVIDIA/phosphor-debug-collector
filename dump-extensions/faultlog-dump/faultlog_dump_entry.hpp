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
#pragma once

#include "dump_entry.hpp"
#include "xyz/openbmc_project/Common/FaultLogType/server.hpp"
#include "xyz/openbmc_project/Dump/Entry/CPERDecode/server.hpp"
#include "xyz/openbmc_project/Dump/Entry/FaultLog/server.hpp"
#include "xyz/openbmc_project/Dump/Entry/server.hpp"
#include "xyz/openbmc_project/Object/Delete/server.hpp"
#include "xyz/openbmc_project/Time/EpochTime/server.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <string>
using json = nlohmann::json;

namespace phosphor
{
namespace dump
{
namespace faultLog
{
template <typename T>
using ServerObject = typename sdbusplus::server::object::object<T>;

using EntryIfaces = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Dump::Entry::server::FaultLog,
    sdbusplus::xyz::openbmc_project::Dump::Entry::server::CPERDecode>;

using FaultDataType = sdbusplus::xyz::openbmc_project::Common::server::
    FaultLogType::FaultLogTypes;

namespace fs = std::filesystem;
class Manager;

/** @class Entry
 *  @brief OpenBMC Dump Entry implementation.
 *  @details A concrete implementation for the
 *  xyz.openbmc_project.Dump.Entry DBus API
 */
class Entry : virtual public phosphor::dump::Entry, virtual public EntryIfaces
{
  public:
    Entry() = delete;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;
    ~Entry() = default;

    /** @brief Constructor for the System Dump Entry Object
     *  @param[in] bus - Bus to attach to.
     *  @param[in] objPath - Object path to attach to
     *  @param[in] dumpId - Dump id.
     *  @param[in] timeStamp - Dump creation timestamp
     *             since the epoch.
     *  @param[in] faultLogType - Type of  fault data
     *  @param[in] additionalTypeName - Additional string to further the type
     *  @param[in] fileSize - Dump file size in bytes.
     *  @param[in] file - Name of dump file.
     *  @param[in] status - status  of the dump.
     *  @param[in] dumpSize - Dump size in bytes.
     *  @param[in] sourceId - DumpId provided by the source.
     *  @param[in] parent - The dump entry's parent.
     */
    Entry(sdbusplus::bus::bus& bus, const std::string& objPath, uint32_t dumpId,
          uint64_t timeStamp, FaultDataType typeIn,
          const std::string& additionalTypeNameIn,
          const std::string& primaryLogIdIn, uint64_t fileSize,
          const fs::path& file, phosphor::dump::OperationStatus status,
          const std::string& NotifTypeIn, const std::string& SectionTypeIn,
          const std::string& fruIDIn, const std::string& severityIn,
          const std::string& nvIPSigIn, const std::string& nvSevIn,
          const std::string& nvSockNumIn, const std::string& pcieVendorIDIn,
          const std::string& pcieDeviceIDIn, const std::string& pcieClassCodeIn,
          const std::string& pcieFuncNumberIn,
          const std::string& pcieDeviceNumberIn,
          const std::string& pcieSegmentNumberIn,
          const std::string& pcieDeviceBusNumberIn,
          const std::string& pcieSecondaryBusNumberIn,
          const std::string& pcieSlotNumberIn, std::string originatorId,
          originatorTypes originatorType, phosphor::dump::Manager& parent) :
        phosphor::dump::Entry(bus, objPath.c_str(), dumpId, timeStamp, fileSize,
                              std::string(), status, originatorId,
                              originatorType, parent),
        EntryIfaces(bus, objPath.c_str(), EntryIfaces::action::defer_emit),
        file(file)
    {
        type(typeIn);
        additionalTypeName(additionalTypeNameIn);
        primaryLogId(primaryLogIdIn);
        notificationType(NotifTypeIn);
        sectionType(SectionTypeIn);
        fruid(fruIDIn);
        severity(severityIn);
        nvipSignature(nvIPSigIn);
        nvSeverity(nvSevIn);
        nvSocketNumber(nvSockNumIn);
        pcieVendorID(pcieVendorIDIn);
        pcieDeviceID(pcieDeviceIDIn);
        pcieClassCode(pcieClassCodeIn);
        pcieFunctionNumber(pcieFuncNumberIn);
        pcieDeviceNumber(pcieDeviceNumberIn);
        pcieSegmentNumber(pcieSegmentNumberIn);
        pcieDeviceBusNumber(pcieDeviceBusNumberIn);
        pcieSecondaryBusNumber(pcieSecondaryBusNumberIn);
        pcieSlotNumber(pcieSlotNumberIn);

        //  Emit deferred signal.
        this->phosphor::dump::faultLog::EntryIfaces::emit_object_added();
    }

    /** @brief Delete this d-bus object.
     */
    void delete_() override;

    /** @brief Method to initiate the offload of dump
     *  @param[in] uri - URI to offload dump
     */
    void initiateOffload(std::string uri) override;

    /**
     * @brief Method to update an existing dump entry, once the dump creation
     *        is completed this function will be used to update the entry which
     * got created during the dump request.
     * @param[in] timeStamp - Dump creation timestamp
     * @param[in] fileSize - Dump file size in bytes.
     * @param[in] file - Name of dump file.
     */
    void update(uint64_t timeStamp, uint64_t fileSize, const fs::path& filePath,
                std::string id)
    {
        elapsed(timeStamp);
        size(fileSize);
        status(OperationStatus::Completed);
        file = filePath;
        completedTime(timeStamp);

        std::string cperDecodePath = "/var/lib/logging/dumps/faultlog/" + id +
                                     "/Decoded/decoded.json";
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
                        notificationType(hdr["NotificationType"]);

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
                                const json& descToLog =
                                    toLog["SectionDescriptor"];

                                // Extracting existing fields
                                if (descToLog.contains("SectionType"))
                                    sectionType(descToLog["SectionType"]);

                                if (descToLog.contains("FRUId"))
                                    fruid(descToLog["FRUId"]);

                                if (descToLog.contains("SectionSeverity"))
                                    severity(descToLog["SectionSeverity"]);

                                if (toLog.contains("Section"))
                                {
                                    const json& sectionToLog = toLog["Section"];

                                    if (sectionToLog.contains("IPSignature"))
                                        nvipSignature(
                                            sectionToLog["IPSignature"]);

                                    if (sectionToLog.contains("Severity"))
                                        nvSeverity(sectionToLog["Severity"]);

                                    if (sectionToLog.contains("SocketNumber"))
                                        nvSocketNumber(
                                            sectionToLog["SocketNumber"]
                                                .dump());

                                    if (sectionToLog.contains("DeviceID"))
                                    {
                                        const json& devID =
                                            sectionToLog["DeviceID"];

                                        if (devID.contains("VendorID"))
                                            pcieVendorID(devID["VendorID"]);

                                        if (devID.contains("DeviceID"))
                                            pcieDeviceID(devID["DeviceID"]);

                                        if (devID.contains("ClassCode"))
                                            pcieClassCode(devID["ClassCode"]);

                                        if (devID.contains("FunctionNumber"))
                                            pcieFunctionNumber(
                                                devID["FunctionNumber"]);

                                        if (devID.contains("DeviceNumber"))
                                            pcieDeviceNumber(
                                                devID["DeviceNumber"]);

                                        if (devID.contains("SegmentNumber"))
                                            pcieSegmentNumber(
                                                devID["SegmentNumber"]);

                                        if (devID.contains("DeviceBusNumber"))
                                            pcieDeviceBusNumber(
                                                devID["DeviceBusNumber"]);

                                        if (devID.contains(
                                                "SecondaryBusNumber"))
                                            pcieSecondaryBusNumber(
                                                devID["SecondaryBusNumber"]);

                                        if (devID.contains("SlotNumber"))
                                            pcieSlotNumber(
                                                devID["SlotNumber"].dump());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /** @brief Minimal interface to allow setting status as failed
     */
    void setFailedStatus(void)
    {
        status(phosphor::dump::OperationStatus::Failed);
    }

  private:
    /** @Dump file name */
    fs::path file;
};

} // namespace faultLog
} // namespace dump
} // namespace phosphor
