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

#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdbusplus/timer.hpp>

#include <chrono>
#include <filesystem>

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
// Max time taken for the bmcweb task timeout is 60 min and dump
// creation is around 60 minutes but keeping the bmcweb task
// timeout as the timeout.
constexpr auto systemDumpMaxTimeLimitInSec = 3600;

namespace fs = std::filesystem;

// NOLINTNEXTLINE
class Manager;

/** @brief OpenBMC Dump Entry implementation for the
 *  xyz.openbmc_project.Dump.Entry DBus API */
class Entry : virtual public phosphor::dump::Entry, virtual public EntryIfaces
{
  public:
    Entry() = delete;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;
    ~Entry() = default;

    /** @brief Constructor for the System Dump Entry Object */
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
     *  @param[in] inProgressKey - Key for Manager::dumpInProgress (e.g.
     * NetIR:GPU_0).
     */
    Entry(sdbusplus::bus_t& bus, const std::string& objPath, uint32_t dumpId,
          uint64_t timeStamp, uint64_t fileSize, const fs::path& file,
          phosphor::dump::OperationStatus status, std::string originatorId,
          originatorTypes originatorType, phosphor::dump::Manager& parent,
          std::string diagnosticType, std::string inProgressKey) :
        phosphor::dump::Entry(bus, objPath.c_str(), dumpId, timeStamp, fileSize,
                              file, status, originatorId, originatorType,
                              parent),
        EntryIfaces(bus, objPath.c_str(), EntryIfaces::action::defer_emit),
        dumpType(std::move(diagnosticType)),
        dumpInProgressKey(std::move(inProgressKey))
    {
        // Emit deferred signal.
        this->phosphor::dump::system::EntryIfaces::emit_object_added();
        // Create timer for in-progress entries
        if (phosphor::dump::Entry::status() == OperationStatus::InProgress)
        {
            progressTimer = std::make_unique<sdbusplus::Timer>([this]() {
                uint64_t now = std::time(nullptr);
                uint64_t limit = (phosphor::dump::Entry::startTime()) +
                                 systemDumpMaxTimeLimitInSec;
                float timeProgress =
                    now <= limit ? (((float)(limit - now) /
                                     (float)systemDumpMaxTimeLimitInSec) *
                                    100.0F)
                                 : 100.0F;
                progress(static_cast<uint8_t>(100 - timeProgress));

                bool completed = phosphor::dump::Entry::status() ==
                                 OperationStatus::Completed;
                bool validProcesGroupId = entryProcessGroupID > 0;
                bool pastTimeout = now > limit;

                if (pastTimeout && validProcesGroupId && !completed)
                {
                    std::string msg =
                        "Terminating " + std::to_string(entryProcessGroupID) +
                        " PGID\r\n";
                    log<level::ERR>(msg.c_str());
                    // SIGTERM: dreport traps it to clean up /tmp leftovers
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
            // Progress update every 45 seconds
            progressTimer->start(std::chrono::seconds(45), true);
        }
    }

    /** @brief Delete this d-bus object. */
    void delete_() override;

    /** @brief Initiate the offload of dump to the given URI */
    void initiateOffload(std::string uri) override;

    /** @brief Update an existing dump entry once dump creation is completed */
    void update(uint64_t timeStamp, uint64_t fileSize, const fs::path& filePath)
    {
        elapsed(timeStamp);
        size(fileSize);
        status(OperationStatus::Completed);
        file = filePath;
        completedTime(timeStamp);
    }

    /** @brief Set status as failed */
    void setFailedStatus()
    {
        status(phosphor::dump::OperationStatus::Failed);
        // Stop the timer so a failed dump resolves immediately (its stop
        // condition only checks Completed or past-timeout, not Failed).
        if (progressTimer)
        {
            progressTimer->stop();
        }
    }

    /** @brief Get entry's dump type */
    std::string getDumpType()
    {
        return dumpType;
    }

    /** @brief Key used in Manager::dumpInProgress (may include device scope).
     */
    const std::string& getDumpInProgressKey() const
    {
        return dumpInProgressKey.empty() ? dumpType : dumpInProgressKey;
    }

    void clearProcessGroupId()
    {
        entryProcessGroupID = 0;
    }

  private:
    /** @brief The dump type of entry */
    std::string dumpType;

    /** @brief Scoped key matching Manager::dumpInProgress for this request */
    std::string dumpInProgressKey;

    /**
     * @brief timer to update progress percent
     *
     */
    std::unique_ptr<sdbusplus::Timer> progressTimer;

    /** @brief Dump process group Id when currently running > 0 or 0 if not
     * valid */
    pid_t entryProcessGroupID{0};
};

} // namespace system
} // namespace dump
} // namespace phosphor
