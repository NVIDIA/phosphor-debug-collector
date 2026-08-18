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

#include "dump_manager.hpp"
#include "dump_utils.hpp"
#include "fdr_dump_entry.hpp"
#include "watch.hpp"
#include "xyz/openbmc_project/Dump/NewDump/server.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdeventplus/source/child.hpp>
#include <xyz/openbmc_project/Dump/Create/server.hpp>

#include <experimental/filesystem>
#include <map>
#include <set>
#include <string>

namespace phosphor
{
namespace dump
{
namespace FDR
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
class Manager :
    virtual public CreateIface,
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
    Manager(sdbusplus::bus::bus& bus, const EventPtr& event, const char* path,
            const std::string& baseEntryPath, const char* filePath) :
        CreateIface(bus, path),
        phosphor::dump::Manager(bus, path, baseEntryPath),
        eventLoop(event.get()),
        dumpWatch(
            eventLoop, IN_NONBLOCK, IN_CLOSE_WRITE | IN_CREATE, EPOLLIN,
            filePath,
            std::bind(std::mem_fn(&phosphor::dump::FDR::Manager::watchCallback),
                      this, std::placeholders::_1)),
        dumpDir(filePath)
    {}

    /** @brief Implementation of dump watch call back
     *  @param [in] fileInfo - map of file info  path:event
     */
    void watchCallback(const UserMap& fileInfo);

    /** @brief Construct dump d-bus objects from their persisted
     *        representations.
     */
    void restore() override;

    /** @brief Implementation for CreateDump
     *  Method to create FDR dump.
     *
     *  @return object_path - The object path of the new dump entry.
     */
    sdbusplus::message::object_path createDump(
        phosphor::dump::DumpCreateParams params) override;

    /** @brief Used to serve case where create dump failed
     *  @param [in] id - entry id which failed
     */
    void createDumpFailed(int id)
    {
        // Use find() — entries[id] would insert a null entry if id is missing,
        // which limitDumpEntries() would later dereference without a null check.
        auto it = entries.find(id);
        if (it != entries.end() && it->second != nullptr)
        {
            dynamic_cast<phosphor::dump::FDR::Entry*>(it->second.get())
                ->setFailedStatus();
        }
    }

  private:
    /** @brief Create Dump entry d-bus object
     *  @param[in] fullPath - Full path of the Dump file name
     */
    void createEntry(const fs::path& fullPath);

    /** @brief Capture FDR Dump.
     *  @param[in] parama - Additional arguments for FDR dump.
     *  @return id - The Dump entry id number.
     */
    uint32_t triggerFDRDumpScript(phosphor::dump::DumpCreateParams params);

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

    /** @brief Erase FDR dump entry and delete respective dump file
     *         from permanent location on reaching maximum allowed
     *         entries.
     */
    void limitDumpEntries();

    /** @brief Single-flight protection set — key "FDR" present while a dump
     *         is running or terminating. Checked in createDump() BEFORE
     *         limitDumpEntries() to prevent eviction of the active entry.
     *         Same pattern as System/NetIR dump (dump_manager_system.hpp).
     */
    std::set<std::string> dumpInProgress;

    /** @brief Per-entry archive/child-result coordination state.
     *
     *  Completion follows one of two paths depending on signal order:
     *    - inotify-first: createEntry() sets archiveReady=true and returns;
     *      Child::Callback finds archiveReady=true and marks Completed.
     *    - exit-first: Child::Callback finds archiveReady=false and performs
     *      a filesystem scan to locate the archive, then marks Completed.
     */
    struct EntryCompletionState
    {
        bool archiveReady{false};
        bool archiveError{false};
        fs::path archivePath{};
        uint64_t archiveSize{0};
        /** @brief Timestamp parsed from archive filename (microseconds since
         *         epoch, matching the format used by Entry::update()). Used by
         *         both completion paths so callback order does not affect the
         *         stored timestamp. */
        uint64_t archiveTimestamp{0};
        /** @brief True only for Collect action — expects an archive. False for
         *         Clean/GenBirthCert which clear the key on terminal exit
         *         without waiting for IN_CLOSE_WRITE. */
        bool expectsArchive{false};
    };

    /** @brief Per-entry completion state map keyed by entry ID. */
    std::map<uint32_t, EntryCompletionState> entryCompletionMap;

    /** @brief Kill the process group and register an async cleanup Child
     *         source to reap the child without blocking the event loop.
     *         Erases entryCompletionMap and dumpInProgress on all paths.
     */
    void killAndCleanup(pid_t pid, pid_t pgid, uint32_t entryId,
                        const std::string& progressKey);
};

} // namespace FDR
} // namespace dump
} // namespace phosphor
