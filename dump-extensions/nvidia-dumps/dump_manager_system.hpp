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
#pragma once

#include "dump_manager.hpp"
#include "dump_utils.hpp"
#include "nvidia_dumps_config.hpp"
#include "system_dump_entry.hpp"
#include "retimer_debug_mode_state.hpp"
#include "watch.hpp"
#include "xyz/openbmc_project/Dump/NewDump/server.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdeventplus/source/child.hpp>
#include <xyz/openbmc_project/Dump/Create/server.hpp>

namespace phosphor
{
namespace dump
{
namespace system
{

using CreateIface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Dump::server::Create>;

using UserMap = phosphor::dump::inotify::UserMap;

namespace fs = std::filesystem;

using Watch = phosphor::dump::inotify::Watch;
using ::sdeventplus::source::Child;

/** @class Manager
 *  @brief OpenBMC Dump manager implementation.
 *  @details A concrete implementation for the
 *  xyz.openbmc_project.Dump.Create DBus API
 */
class Manager : virtual public CreateIface,
                virtual public phosphor::dump::Manager
{
  public:
    Manager() = delete;
    Manager(const Manager&) = default;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;
    virtual ~Manager() = default;

    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] event - Dump manager sd_event loop.
     *  @param[in] path - Path to attach at.
     *  @param[in] baseEntryPath - Base path for dump entry.
     *  @param[in] filePath - Path where the dumps are stored.
     */
    Manager(sdbusplus::bus_t& bus, const EventPtr& event, const char* path,
            const std::string& baseEntryPath, const char* filePath) :
        CreateIface(bus, path),
        phosphor::dump::Manager(bus, path, baseEntryPath),
        eventLoop(event.get()),
        dumpWatch(
            eventLoop, IN_NONBLOCK, IN_CLOSE_WRITE | IN_CREATE, EPOLLIN,
            filePath,
            std::bind(
                std::mem_fn(&phosphor::dump::system::Manager::watchCallback),
                this, std::placeholders::_1)),
        dumpDir(filePath),
        retimerDebugModeState(bus, RETIMER_DEBUG_MODE_OBJPATH)
    {
    }

    /** @brief Implementation of dump watch call back
     *  @param [in] fileInfo - map of file info  path:event
     */
    void watchCallback(const UserMap& fileInfo);

    /** @brief Construct dump d-bus objects from their persisted
     *        representations.
     */
    void restore() override;

    /** @brief Implementation for CreateDump
     *  Method to create system dump.
     *
     *  @return object_path - The object path of the new dump entry.
     */
    sdbusplus::message::object_path
        createDump(phosphor::dump::DumpCreateParams params) override;

    /** @brief Used to serve case where create dump failed
     *  @param [in] id - entry id which failed
     */
    void createDumpFailed(int id)
    {
        auto entry = entries[id].get();
        if (entry != nullptr)
        {
            dynamic_cast<phosphor::dump::system::Entry*>(entries[id].get())
                ->setFailedStatus();
        }
    }

  private:
    /** @brief Create Dump entry d-bus object
     *  @param[in] fullPath - Full path of the Dump file name
     */
    void createEntry(const fs::path& fullPath);

    /** @brief Capture System Dump.
     *  @param[in] parama - Additional arguments for system dump.
     *  @return id - The Dump entry id number.
     */
    uint32_t captureDump(phosphor::dump::DumpCreateParams params);

    /** @brief Remove specified watch object pointer from the
     *        watch map and associated entry from the map.
     *        @param[in] path - unique identifier of the map
     */
    void removeWatch(const fs::path& path);

    /** @brief Calculate per dump allowed size based on the available
     *        size in the dump location.
     *  @returns dump size in kilobytes.
     */
    size_t getAllowedSize();

    /** @brief sdbusplus Dump event loop */
    EventPtr eventLoop;

    /** @brief Dump main watch object */
    Watch dumpWatch;

    /** @brief Path to the dump file*/
    std::string dumpDir;

    /** @brief Child directory path and its associated watch object map
     *        [path:watch object]
     */
    std::map<fs::path, std::unique_ptr<Watch>> childWatchMap;

    /** @brief map of SDEventPlus child pointer added to event loop */
    std::map<pid_t, std::unique_ptr<Child>> childPtrMap;

    /** @brief D-bus object for indicating retimer debug mode state*/
    phosphor::dump::retimer::DebugMode retimerDebugModeState;

    /** @brief a set containing string of dump types that are in progress */
    std::set<std::string> dumpInProgress;

    /** @brief Erase BMC dump entry and delete respective dump file
     *         from permanent location on reaching maximum allowed
     *         entries.
     */
    void limitDumpEntries();
};

} // namespace system
} // namespace dump
} // namespace phosphor
