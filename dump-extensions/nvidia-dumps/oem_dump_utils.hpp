/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION &
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
#include "config.h"

#include "com/nvidia/Dump/AllowableValues/server.hpp"

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

constexpr auto DUMP_OEM_ALLOWABLE_VALUES_PATH =
    "/xyz/openbmc_project/dump/oem_allowable_values";

namespace phosphor
{
namespace dump
{

using AllowableValuesIface = sdbusplus::server::object::object<
    sdbusplus::com::nvidia::Dump::server::AllowableValues>;
using DumpType = AllowableValuesIface::DumpType;

using OEMDataTypeAllowableValuesObject =
    sdbusplus::server::object::object<AllowableValuesIface>;

class AsioConnection
{
  public:
    AsioConnection() = delete;
    AsioConnection(const AsioConnection&) = delete;
    AsioConnection& operator=(const AsioConnection&) = delete;
    AsioConnection(AsioConnection&&) = delete;
    AsioConnection& operator=(AsioConnection&&) = delete;
    ~AsioConnection() = delete;

    /** @brief Get the asio connection. */
    static auto& getAsioConnection()
    {
        static boost::asio::io_context io;
        static auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        return conn;
    }
};

/** @brief One entry in the EID-keyed dump-target map; a live snapshot of
 *  currently-present dumpable endpoints (added/erased on MCTPReactor signals).
 */
struct DumpTarget
{
    std::string redfishDeviceName;           // e.g. "HGX_GPU_0"
    std::optional<std::string> deviceType;   // default DeviceType (fan-out)
    std::vector<std::string> supportedDumps; // e.g. {"RoT","GPUDiag","NetIR"}
    // Per-kind DeviceType override (kind -> DeviceType) parsed from
    // "<kind>=<DeviceType>" tokens; lets one endpoint expose kinds under
    // distinct DeviceTypes.
    std::map<std::string, std::string> deviceTypeByKind;
};

/** @brief MCTPReactor endpoint subtree root; PDC subscribes to
 *  InterfacesAdded/Removed under this path.
 */
constexpr auto MCTP_REACTOR_ROOT = "/au/com/codeconstruct/mctp1";

/** @brief Interface mctpd publishes on each endpoint (watched for teardown). */
constexpr auto MCTP_ENDPOINT_INTERFACE = "xyz.openbmc_project.MCTP.Endpoint";

/** @brief Association interface carrying the `configured_by` link to the device
 *  inventory object; PDC triggers on this so the target is always resolvable.
 */
constexpr auto ASSOCIATION_DEFINITIONS_INTERFACE =
    "xyz.openbmc_project.Association.Definitions";

class OEMTypeAllowableValuesIf : public OEMDataTypeAllowableValuesObject
{
  public:
    OEMTypeAllowableValuesIf() = delete;
    OEMTypeAllowableValuesIf(const OEMTypeAllowableValuesIf&) = delete;
    OEMTypeAllowableValuesIf& operator=(const OEMTypeAllowableValuesIf&) =
        delete;
    OEMTypeAllowableValuesIf(OEMTypeAllowableValuesIf&&) = delete;
    OEMTypeAllowableValuesIf& operator=(OEMTypeAllowableValuesIf&&) = delete;
    virtual ~OEMTypeAllowableValuesIf() = default;

    /** @brief Subscribes to MCTPReactor signals and runs a cold-start
     *  enumeration; `bus` must be the sd_event-attached bus so matches fire.
     */
    OEMTypeAllowableValuesIf(sdbusplus::bus_t& bus, const char* path);

    /** @brief Populate OEM allowable values for System dump; runs cold-start
     *  catch-up then rebuilds. Later MCTPReactor signals re-publish.
     */
    void populateSystemOEMDataTypeAllowableValues(
        sdbusplus::server::com::nvidia::dump::AllowableValues& iface);

#ifdef FDR_DUMP_EXTENSION
    /** @brief Populate OEM allowable values for FDR dump */
    void populateFDROEMDataTypeAllowableValues(
        sdbusplus::server::com::nvidia::dump::AllowableValues& iface);
#endif

    /** @brief Snapshot the EID-keyed target map by value under a shared lock.
     */
    std::map<uint8_t, DumpTarget> snapshotTargets() const;

  private:
    /** @brief Resolve an endpoint via its configured_by association and insert
     *  into targetsByEid_. Caller holds unique_lock on mapMutex_. Returns true
     *  if a target was resolved and inserted.
     */
    bool populateTargetFromEm(uint8_t eid, const std::string& endpointPath);

    /** @brief Handler for InterfacesAdded (and cold-start synthetic-add);
     *  resolves and inserts the endpoint's target, idempotent.
     */
    void onInterfacesAdded(uint8_t eid, const std::string& endpointPath);

    /** @brief Handler for the MCTPReactor InterfacesRemoved signal. */
    void onInterfacesRemoved(uint8_t eid);

    /** @brief Build the AllowableValues set from targetsByEid_ and publish.
     *  Caller must NOT hold mapMutex_ (takes a shared_lock).
     */
    void rebuildAllowableValues();

    /** @brief One-shot cold-start enumeration of present endpoints, calling
     *  onInterfacesAdded per live EID. Runs once from the constructor.
     */
    void coldStartEnumerate();

    /** @brief Extract the EID from an endpoint object path; nullopt if the path
     *  doesn't match the expected pattern.
     */
    static std::optional<uint8_t> parseEidFromEndpointPath(
        const std::string& path);

    /** @brief EID-keyed target map, the SoT for AllowableValues; mutated under
     *  mapMutex_.
     */
    std::map<uint8_t, DumpTarget> targetsByEid_;
    mutable std::shared_mutex mapMutex_;

    /** @brief The sd_event-attached bus; matches and D-Bus calls go through it
     *  so signals fire.
     */
    sdbusplus::bus_t& bus_;

    /** @brief D-Bus match for InterfacesAdded under MCTPReactor's root. */
    std::unique_ptr<sdbusplus::bus::match_t> ifacesAddedMatch_;

    /** @brief D-Bus match for InterfacesRemoved under MCTPReactor's root. */
    std::unique_ptr<sdbusplus::bus::match_t> ifacesRemovedMatch_;
};

} // namespace dump
} // namespace phosphor
