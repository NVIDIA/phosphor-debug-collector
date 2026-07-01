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

#include "oem_dump_utils.hpp"

#include "dump_kinds.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace phosphor
{
namespace dump
{

using MapperGetSubTreeResponse =
    std::map<std::string, std::map<std::string, std::vector<std::string>>>;

namespace
{

// Bounded reply timeout for ObjectMapper/EM lookups so a slow or unresponsive
// mapper can't wedge the single-threaded sd_event loop (vs the 25s default).
constexpr uint64_t mapperCallTimeoutUs = 10'000'000; // 10s

/** @brief Property variant emitted by EM Configuration records; covers the
 *  integer/string/vector forms entity-manager may publish.
 */
using EmPropVariant =
    std::variant<std::string, int64_t, int32_t, uint64_t, uint32_t, uint16_t,
                 uint8_t, double, bool, std::vector<std::string>,
                 std::vector<uint8_t>>;

/** @brief Read a string property out of a GetAll property map. */
std::string getString(const std::map<std::string, EmPropVariant>& props,
                      const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return {};
    }
    if (auto p = std::get_if<std::string>(&it->second))
    {
        return *p;
    }
    return {};
}

/** @brief Read a vector<string> property out of a GetAll property map. */
std::vector<std::string> getStringVec(
    const std::map<std::string, EmPropVariant>& props, const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return {};
    }
    if (auto p = std::get_if<std::vector<std::string>>(&it->second))
    {
        return *p;
    }
    return {};
}

/** @brief Dump capability resolved for one MCTP endpoint EID. */
struct TargetDump
{
    std::string name;
    std::vector<std::string> supportedDumps;
    std::optional<std::string> deviceType;
    // Per-kind DeviceType override parsed from "<kind>=<DeviceType>" tokens.
    std::map<std::string, std::string> deviceTypeByKind;
};

/** @brief Resolve an MCTP endpoint to its dump capability: read the endpoint's
 *  configured_by target, then the matching Configuration.Dump record's
 *  SupportedDumps/DeviceType. Nullopt if unresolvable.
 */
std::optional<TargetDump> resolveTargetDump(sdbusplus::bus_t& bus,
                                            const std::string& endpointPath)
{
    namespace fs = std::filesystem;
    using Assoc = std::tuple<std::string, std::string, std::string>;

    // 1) GetObject the endpoint (filtered to Association.Definitions) to name
    //    its owning service.
    std::map<std::string, std::vector<std::string>> endpointObj;
    try
    {
        auto call = bus.new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetObject");
        call.append(endpointPath);
        call.append(std::vector<std::string>{
            "xyz.openbmc_project.Association.Definitions"});
        auto reply = bus.call(call, mapperCallTimeoutUs);
        reply.read(endpointObj);
    }
    catch (const std::exception&)
    {
        // No Association.Definitions on the endpoint yet: nothing to resolve.
        return std::nullopt;
    }
    if (endpointObj.empty())
    {
        return std::nullopt;
    }

    std::string targetPath;
    try
    {
        std::variant<std::vector<Assoc>> v;
        auto get = bus.new_method_call(
            endpointObj.begin()->first.c_str(), endpointPath.c_str(),
            "org.freedesktop.DBus.Properties", "Get");
        get.append("xyz.openbmc_project.Association.Definitions",
                   "Associations");
        auto reply = bus.call(get, mapperCallTimeoutUs);
        reply.read(v);
        for (const auto& [forward, reverse, tgt] :
             std::get<std::vector<Assoc>>(v))
        {
            (void)reverse;
            if (forward == "configured_by" && !tgt.empty())
            {
                targetPath = tgt;
                break;
            }
        }
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (targetPath.empty())
    {
        return std::nullopt;
    }

    // 2) The device name is the basename of the configured_by target.
    const std::string deviceName = fs::path(targetPath).filename().string();

    // 3) Find the Configuration.Dump record whose Name matches the device.
    MapperGetSubTreeResponse dumps;
    try
    {
        auto call = bus.new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        call.append(std::string("/xyz/openbmc_project/inventory"));
        call.append(int32_t{0});
        call.append(
            std::vector<std::string>{"xyz.openbmc_project.Configuration.Dump"});
        auto reply = bus.call(call, mapperCallTimeoutUs);
        reply.read(dumps);
    }
    catch (const std::exception& e)
    {
        lg2::info(
            "resolveTargetDump: GetSubTree(Configuration.Dump) failed: {ERR}",
            "ERR", e.what());
        return std::nullopt;
    }

    TargetDump out;
    out.name = deviceName;
    for (const auto& [path, services] : dumps)
    {
        if (services.empty())
        {
            continue;
        }
        std::map<std::string, EmPropVariant> props;
        try
        {
            auto getAll = bus.new_method_call(
                services.begin()->first.c_str(), path.c_str(),
                "org.freedesktop.DBus.Properties", "GetAll");
            getAll.append("xyz.openbmc_project.Configuration.Dump");
            auto reply = bus.call(getAll, mapperCallTimeoutUs);
            reply.read(props);
        }
        catch (const std::exception&)
        {
            continue;
        }
        if (getString(props, "Name") != deviceName)
        {
            continue;
        }
        // SupportedDumps tokens are bare kinds or "<kind>=<DeviceType>"
        // per-kind DeviceType overrides.
        for (auto& tok : getStringVec(props, "SupportedDumps"))
        {
            auto eq = tok.find('=');
            if (eq == std::string::npos)
            {
                out.supportedDumps.push_back(tok);
            }
            else
            {
                std::string kind = tok.substr(0, eq);
                out.deviceTypeByKind[kind] = tok.substr(eq + 1);
                out.supportedDumps.push_back(std::move(kind));
            }
        }
        if (auto dt = getString(props, "DeviceType"); !dt.empty())
        {
            out.deviceType = std::move(dt);
        }
        break;
    }
    if (out.supportedDumps.empty())
    {
        return std::nullopt;
    }
    return out;
}

} // namespace

OEMTypeAllowableValuesIf::OEMTypeAllowableValuesIf(sdbusplus::bus_t& bus,
                                                   const char* path) :
    OEMDataTypeAllowableValuesObject(bus, path), bus_(bus)
{
    // AllowableValues' single source of truth is the EM Configuration.Dump
    // JSON; no runtime self-gates here.

    // Subscribe to MCTPReactor signals BEFORE cold-start enumeration so an
    // endpoint added during the window is still captured (idempotent insert).
    // Permissive variant for the properties mctpd publishes; unknown types
    // throw and the signal is dropped (cold-start/later signals cover it).
    using DbusVariant =
        std::variant<std::string, int64_t, int32_t, uint64_t, uint32_t,
                     uint16_t, uint8_t, double, bool, std::vector<std::string>,
                     std::vector<uint8_t>>;
    using InterfacesAddedT =
        std::map<std::string, std::map<std::string, DbusVariant>>;

    ifacesAddedMatch_ = std::make_unique<sdbusplus::bus::match_t>(
        bus_,
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::path_namespace(MCTP_REACTOR_ROOT),
        [this](sdbusplus::message::message& msg) {
            sdbusplus::object_path objPath;
            InterfacesAddedT interfaces;
            try
            {
                msg.read(objPath, interfaces);
            }
            catch (const std::exception& e)
            {
                lg2::warning(
                    "OEMAllowableValues: InterfacesAdded parse failed: {ERR}",
                    "ERR", e.what());
                return;
            }
            // Trigger only once the configured_by association is published,
            // so the endpoint resolves without a retry.
            if (!interfaces.contains(ASSOCIATION_DEFINITIONS_INTERFACE))
            {
                return;
            }
            auto eid = parseEidFromEndpointPath(objPath.str);
            if (!eid)
            {
                return;
            }
            this->onInterfacesAdded(*eid, objPath.str);
        });

    ifacesRemovedMatch_ = std::make_unique<sdbusplus::bus::match_t>(
        bus_,
        sdbusplus::bus::match::rules::interfacesRemoved() +
            sdbusplus::bus::match::rules::path_namespace(MCTP_REACTOR_ROOT),
        [this](sdbusplus::message::message& msg) {
            sdbusplus::object_path objPath;
            std::vector<std::string> interfaces;
            try
            {
                msg.read(objPath, interfaces);
            }
            catch (const std::exception& e)
            {
                lg2::warning(
                    "OEMAllowableValues: InterfacesRemoved parse failed: {ERR}",
                    "ERR", e.what());
                return;
            }
            if (std::find(interfaces.begin(), interfaces.end(),
                          MCTP_ENDPOINT_INTERFACE) == interfaces.end())
            {
                return;
            }
            auto eid = parseEidFromEndpointPath(objPath.str);
            if (!eid)
            {
                return;
            }
            this->onInterfacesRemoved(*eid);
        });

    populateSystemOEMDataTypeAllowableValues(*this);

#ifdef FDR_DUMP_EXTENSION
    populateFDROEMDataTypeAllowableValues(*this);
#endif
}

std::optional<uint8_t> OEMTypeAllowableValuesIf::parseEidFromEndpointPath(
    const std::string& path)
{
    // Expected: .../networks/<N>/endpoints/<EID>
    constexpr std::string_view marker = "/endpoints/";
    auto pos = path.find(marker);
    if (pos == std::string::npos)
    {
        return std::nullopt;
    }
    auto tail = path.substr(pos + marker.size());
    // Strip any trailing slash or extra path components.
    auto slash = tail.find('/');
    if (slash != std::string::npos)
    {
        tail = tail.substr(0, slash);
    }
    if (tail.empty())
    {
        return std::nullopt;
    }
    try
    {
        auto eid = std::stoul(tail, nullptr, 0);
        if (eid <= 0xFF)
        {
            return static_cast<uint8_t>(eid);
        }
    }
    catch (...)
    {
        return std::nullopt;
    }
    return std::nullopt;
}

std::map<uint8_t, DumpTarget> OEMTypeAllowableValuesIf::snapshotTargets() const
{
    std::shared_lock lock(mapMutex_);
    return targetsByEid_;
}

bool OEMTypeAllowableValuesIf::populateTargetFromEm(
    uint8_t eid, const std::string& endpointPath)
{
    // Caller holds a unique_lock on mapMutex_. Resolve endpoint -> capability.
    auto info = resolveTargetDump(bus_, endpointPath);
    if (!info)
    {
        // Unresolvable yet; this EID contributes nothing to AllowableValues.
        return false;
    }

    DumpTarget t;
    t.redfishDeviceName = std::move(info->name);
    t.deviceType = std::move(info->deviceType);
    t.supportedDumps = std::move(info->supportedDumps);
    t.deviceTypeByKind = std::move(info->deviceTypeByKind);
    targetsByEid_[eid] = std::move(t);
    return true;
}

void OEMTypeAllowableValuesIf::onInterfacesAdded(
    uint8_t eid, const std::string& endpointPath)
{
    // Insert/overwrite the EID's target from EM, then re-publish.
    bool resolved = false;
    {
        std::unique_lock lock(mapMutex_);
        resolved = populateTargetFromEm(eid, endpointPath);
    }
    // Rebuild only if a target was actually inserted.
    if (resolved)
    {
        rebuildAllowableValues();
    }
}

void OEMTypeAllowableValuesIf::onInterfacesRemoved(uint8_t eid)
{
    bool changed = false;
    {
        std::unique_lock lock(mapMutex_);
        changed = targetsByEid_.erase(eid) > 0;
    }
    if (changed)
    {
        rebuildAllowableValues();
    }
}

void OEMTypeAllowableValuesIf::rebuildAllowableValues()
{
    std::set<std::string> allow;
    {
        std::shared_lock lock(mapMutex_);
        for (const auto& [eid, t] : targetsByEid_)
        {
            for (const auto& kind : t.supportedDumps)
            {
                auto ki = dumpKinds().find(kind);
                if (ki == dumpKinds().end())
                {
                    continue; // unknown kind — not advertised
                }
                const auto& info = ki->second;
                if (info.perDevice)
                {
                    // Per-kind DeviceType override wins over the default.
                    auto ovr = t.deviceTypeByKind.find(kind);
                    const std::string deviceType =
                        (ovr != t.deviceTypeByKind.end())
                            ? ovr->second
                            : t.deviceType.value_or("");
                    allow.insert(std::format("DiagnosticType={};DeviceType={}",
                                             info.diagnosticType, deviceType));
                }
                else
                {
                    allow.insert(
                        std::format("DiagnosticType={}", info.diagnosticType));
                }
            }
        }
    }
    std::vector<std::string> systemValues(allow.begin(), allow.end());

    try
    {
        std::map<DumpType, std::vector<std::string>> oemAllowableValuesMap =
            this->oemDataTypeAllowableValues();
        oemAllowableValuesMap[DumpType::System] = std::move(systemValues);
        this->oemDataTypeAllowableValues(oemAllowableValuesMap);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error(
            "rebuildAllowableValues: failed to publish System AllowableValues: "
            "{ERR}",
            "ERR", e.what());
    }
}

void OEMTypeAllowableValuesIf::coldStartEnumerate()
{
    // GetSubTree for endpoints already carrying a configured_by association;
    // the rest arrive later via the signal.
    MapperGetSubTreeResponse subtree;
    try
    {
        auto call = bus_.new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        call.append(std::string(MCTP_REACTOR_ROOT));
        call.append(int32_t{0});
        call.append(
            std::vector<std::string>{ASSOCIATION_DEFINITIONS_INTERFACE});
        auto reply = bus_.call(call, mapperCallTimeoutUs);
        reply.read(subtree);
    }
    catch (const std::exception& e)
    {
        lg2::info("coldStartEnumerate: MCTPReactor not yet present "
                  "(GetSubTree failed: {ERR}); deferring to signals",
                  "ERR", e.what());
        return;
    }

    for (const auto& [path, services] : subtree)
    {
        auto eid = parseEidFromEndpointPath(path);
        if (eid)
        {
            onInterfacesAdded(*eid, path);
        }
    }
}

void OEMTypeAllowableValuesIf::populateSystemOEMDataTypeAllowableValues(
    sdbusplus::server::com::nvidia::dump::AllowableValues& iface)
{
    // Run cold-start catch-up, then publish the initial AllowableValues.
    (void)iface; // setting goes via this->oemDataTypeAllowableValues()
    coldStartEnumerate();
    rebuildAllowableValues();
}

#ifdef FDR_DUMP_EXTENSION
void OEMTypeAllowableValuesIf::populateFDROEMDataTypeAllowableValues(
    sdbusplus::server::com::nvidia::dump::AllowableValues& iface)
{
    std::vector<std::string> oemAllowableValues;

    // FDR dump format is fixed
    oemAllowableValues.emplace_back(
        "DiagnosticType=FDR;TimeRangeStart=<yyyy>-<MM>-<dd> <HH>:<mm>:<ss>;"
        "TimeRangeEnd=<yyyy>-<MM>-<dd> <HH>:<mm>:<ss>;ExtendedSource=<source-info>");

    try
    {
        std::map<DumpType, std::vector<std::string>> oemAllowableValuesMap =
            iface.oemDataTypeAllowableValues();
        oemAllowableValuesMap[DumpType::FDR] = oemAllowableValues;
        iface.oemDataTypeAllowableValues(oemAllowableValuesMap);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to set OEM allowable values for FDR Dump: "
                   "ERROR={ERROR}",
                   "ERROR", e.what());
    }
}
#endif

} // namespace dump
} // namespace phosphor
