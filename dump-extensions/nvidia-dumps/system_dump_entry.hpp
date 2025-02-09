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
#include "xyz/openbmc_project/Dump/Entry/System/server.hpp"
#include "xyz/openbmc_project/Dump/Entry/server.hpp"
#include "xyz/openbmc_project/Object/Delete/server.hpp"
#include "xyz/openbmc_project/Time/EpochTime/server.hpp"

#include <chrono>
#include <filesystem>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdbusplus/timer.hpp>

namespace phosphor
{
namespace dump
{
namespace system
{
using namespace phosphor::logging;

template <typename T>
using ServerObject = typename sdbusplus::server::object::object<T>;

using EntryIfaces = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Dump::Entry::server::System>;

// Timeout is kept similar to bmcweb dump creation task timeout
// Max time taken for the bmcweb task timeout is 45 min and dump
// creation is around 45 minutes but keeping the bmcweb task
// timeout as the timeout.
constexpr auto systemDumpMaxTimeLimitInSec = 2700;

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
     *  @param[in] fileSize - Dump file size in bytes.
     *  @param[in] file - Name of dump file.
     *  @param[in] status - status  of the dump.
     *  @param[in] dumpSize - Dump size in bytes.
     *  @param[in] sourceId - DumpId provided by the source.
     *  @param[in] parent - The dump entry's parent.
     */
    Entry(sdbusplus::bus_t& bus, const std::string& objPath, uint32_t dumpId,
          uint64_t timeStamp, uint64_t fileSize, const fs::path& file,
          phosphor::dump::OperationStatus status, std::string originatorId,
          originatorTypes originatorType, phosphor::dump::Manager& parent) :
        phosphor::dump::Entry(bus, objPath.c_str(), dumpId, timeStamp, fileSize,
                              file, status, originatorId, originatorType,
                              parent),
        EntryIfaces(bus, objPath.c_str(), EntryIfaces::action::defer_emit)
    {
        // Emit deferred signal.
        this->phosphor::dump::system::EntryIfaces::emit_object_added();
    }
    /** @brief Constructor for the System Dump Entry Object with dump type
     * included
     *  @param[in] bus - Bus to attach to.
     *  @param[in] objPath - Object path to attach to
     *  @param[in] dumpId - Dump id.
     *  @param[in] timeStamp - Dump creation timestamp
     *             since the epoch.
     *  @param[in] fileSize - Dump file size in bytes.
     *  @param[in] file - Name of dump file.
     *  @param[in] status - status  of the dump.
     *  @param[in] dumpSize - Dump size in bytes.
     *  @param[in] sourceId - DumpId provided by the source.
     *  @param[in] parent - The dump entry's parent.
     *  @param[in] diagnosticType - The dump entry's dump type.
     */
    Entry(sdbusplus::bus_t& bus, const std::string& objPath, uint32_t dumpId,
          uint64_t timeStamp, uint64_t fileSize, const fs::path& file,
          phosphor::dump::OperationStatus status, std::string originatorId,
          originatorTypes originatorType, phosphor::dump::Manager& parent,
          std::string diagnosticType) :
        phosphor::dump::Entry(bus, objPath.c_str(), dumpId, timeStamp, fileSize,
                              file, status, originatorId, originatorType,
                              parent),
        EntryIfaces(bus, objPath.c_str(), EntryIfaces::action::defer_emit)
    {
        // Emit deferred signal.
        this->phosphor::dump::system::EntryIfaces::emit_object_added();
        dumpType = diagnosticType;
        // Create timer for entries which are in progress
        if (phosphor::dump::Entry::status() == OperationStatus::InProgress)
        {
            progressTimer = std::make_unique<sdbusplus::Timer>([this]() {
                uint64_t now = std::time(nullptr);
                uint64_t limit = (phosphor::dump::Entry::startTime()) +
                                 systemDumpMaxTimeLimitInSec;
                float timeProgress =
                    now <= limit ? (((float)(limit - now) /
                                     (float)systemDumpMaxTimeLimitInSec) *
                                    100.0)
                                 : 100.0;
                progress(100 - timeProgress);
                std::string m =
                    "Dump is " + std::to_string(100 - timeProgress) + " " +
                    std::to_string(now) + " " + std::to_string(limit) + +" " +
                    std::to_string(timeProgress);
                log<level::ERR>(m.c_str());

                bool completed = phosphor::dump::Entry::status() ==
                                 OperationStatus::Completed;
                bool validProcesGroupId = entryProcessGroupID > 0;
                bool pastTimeout = now > limit;

                if (pastTimeout && validProcesGroupId && !completed)
                {
                    std::string msg = "Terminating " +
                                      std::to_string(entryProcessGroupID) +
                                      " PGID\r\n";
                    log<level::ERR>(msg.c_str());
                    /* use SIGTERM as dreport has TRAP on it to clean-up
                        leftovers in /tmp */
                    kill(-1 * (entryProcessGroupID), SIGTERM);
                    clearProcessGroupId();
                }

                if (completed || pastTimeout)
                {
                    progressTimer->stop();
                    if (pastTimeout && !completed)
                    {
                        std::string msg =
                            "Stopped progress timer due to timeout";
                        log<level::ERR>(msg.c_str());
                    }
                }
                return;
            });
            // Progress update is done every 45 second.
            progressTimer->start(std::chrono::seconds(45), true);
        }
    }

    /** @brief Delete this d-bus object.
     */
    void delete_() override;

    /** @brief Method to initiate the offload of dump
     *  @param[in] uri - URI to offload dump
     */
    void initiateOffload(std::string uri) override;

    /** @brief Method to update an existing dump entry, once the dump creation
     *  is completed this function will be used to update the entry which got
     *  created during the dump request.
     *  @param[in] timeStamp - Dump creation timestamp
     *  @param[in] fileSize - Dump file size in bytes.
     *  @param[in] file - Name of dump file.
     */
    void update(uint64_t timeStamp, uint64_t fileSize, const fs::path& filePath)
    {
        elapsed(timeStamp);
        size(fileSize);
        status(OperationStatus::Completed);
        file = filePath;
        completedTime(timeStamp);
    }

    /** @brief Minimal interface to allow setting status as failed
     */
    void setFailedStatus(void)
    {
        status(phosphor::dump::OperationStatus::Failed);
    }

    /** @brief Method to get entry's dump type
     *  @return A string of dump type.
     */
    std::string getDumpType()
    {
        return dumpType;
    }

    void clearProcessGroupId(void)
    {
        entryProcessGroupID = 0;
    }

  private:
    /** @brief A string implying the dump type of entry*/
    std::string dumpType;

    /**
     * @brief timer to update progress percent
     *
     */
    std::unique_ptr<sdbusplus::Timer> progressTimer;

    /** @brief Dump process group Id when currently running > 0 or 0 if not
     * valid */
    pid_t entryProcessGroupID;
};

} // namespace system
} // namespace dump
} // namespace phosphor
