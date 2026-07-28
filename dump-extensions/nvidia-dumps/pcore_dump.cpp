/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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

#include "pcore_dump.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

namespace phosphor::dump::pcore
{

namespace
{

constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";

/** @brief Read a uint64 property from the effecter object. */
std::optional<uint64_t> readBound(sdbusplus::bus_t& bus,
                                  const std::string& path, const char* property)
{
    try
    {
        auto method = bus.new_method_call(pldmService, path.c_str(),
                                          propertiesInterface, "Get");
        method.append(pcoreDumpInterface, property);

        std::variant<uint64_t> value;
        bus.call(method).read(value);
        return std::get<uint64_t>(value);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "PCoreDump: cannot read {PROPERTY} from {PATH}, error: {ERROR}",
            "PROPERTY", property, "PATH", path, "ERROR", e);
        return std::nullopt;
    }
}

} // namespace

std::optional<TerminusTarget> terminusForDevice(const std::string& configPath,
                                                const std::string& deviceType)
{
    std::ifstream configFile(configPath);
    if (!configFile.is_open())
    {
        lg2::error("PCoreDump: cannot open pldm static configuration {PATH}",
                   "PATH", configPath);
        return std::nullopt;
    }

    auto config = nlohmann::json::parse(configFile, nullptr, false);
    if (config.is_discarded() || !config.contains("PLDMTermini") ||
        !config["PLDMTermini"].is_array())
    {
        lg2::error(
            "PCoreDump: pldm static configuration {PATH} has no PLDMTermini array",
            "PATH", configPath);
        return std::nullopt;
    }

    for (const auto& terminus : config["PLDMTermini"])
    {
        if (!terminus.is_object() || !terminus.contains("Instance") ||
            !terminus["Instance"].is_number_integer() ||
            !terminus.contains("TerminusName") ||
            !terminus["TerminusName"].is_string())
        {
            continue;
        }

        // CpuIndex marks the CPU-package termini; other termini share the
        // array but never own a PCore dump effecter.
        if (!terminus.contains("CpuIndex"))
        {
            continue;
        }

        if (deviceType !=
            "CPU_" + std::to_string(terminus["Instance"].get<int>()))
        {
            continue;
        }

        auto name = terminus["TerminusName"].get<std::string>();
        if (name.empty())
        {
            continue;
        }

        // The EID is what separates two packages that share a terminus name.
        // Absent or malformed, resolution falls back to the name alone, which
        // is unambiguous only where the name is unique.
        int eid = -1;
        if (terminus.contains("EID") && terminus["EID"].is_number_integer() &&
            terminus["EID"].get<int>() >= 0)
        {
            eid = terminus["EID"].get<int>();
        }
        else
        {
            lg2::warning(
                "PCoreDump: {DEVICE} has no usable EID in the pldm static "
                "configuration; resolving by terminus name alone",
                "DEVICE", deviceType);
        }

        return TerminusTarget{.name = std::move(name), .eid = eid};
    }

    return std::nullopt;
}

std::optional<EffecterTarget> resolveEffecter(sdbusplus::bus_t& bus,
                                              const std::string& deviceType)
{
    auto terminus = terminusForDevice(pldmStaticConfigPath, deviceType);
    if (!terminus)
    {
        lg2::error("PCoreDump: {DEVICE} is not a configured CPU terminus",
                   "DEVICE", deviceType);
        return std::nullopt;
    }

    std::vector<std::string> paths;
    try
    {
        auto method = bus.new_method_call(mapperService, mapperPath,
                                          mapperInterface, "GetSubTreePaths");
        method.append(controlRoot, 0,
                      std::vector<std::string>{pcoreDumpInterface});
        bus.call(method).read(paths);
    }
    catch (const std::exception& e)
    {
        lg2::error("PCoreDump: ObjectMapper lookup failed, error: {ERROR}",
                   "ERROR", e);
        return std::nullopt;
    }

    // The object name is the terminus-prefixed PDR auxiliary name, which is
    // what ties an object to one CPU package.
    //
    // Matching stops at that name deliberately. pldmd builds these paths flat
    // as /xyz/openbmc_project/control/<effecterName>, so there is no parent
    // component to carry an EID, and the fallback name for an effecter with no
    // PDR auxiliary name embeds the TID rather than the EID. A platform that
    // ever fronted both packages behind a single terminus name could therefore
    // not be separated here at all; the mechanism for that is the per-CPU
    // cpu/pcore_dump_control association pldmd already publishes on each
    // effecter, not the object path.
    const std::string prefix = terminus->name + "_";
    for (const auto& path : paths)
    {
        if (!std::filesystem::path(path).filename().string().starts_with(
                prefix))
        {
            continue;
        }

        auto minId = readBound(bus, path, "MinPCoreId");
        auto maxId = readBound(bus, path, "MaxPCoreId");
        if (!minId || !maxId)
        {
            return std::nullopt;
        }

        if (*minId > *maxId)
        {
            lg2::error(
                "PCoreDump: {PATH} advertises an empty selector range {MIN}..{MAX}",
                "PATH", path, "MIN", *minId, "MAX", *maxId);
            return std::nullopt;
        }

        lg2::info(
            "PCoreDump: {DEVICE} resolved to {PATH}, selectors {MIN}..{MAX}",
            "DEVICE", deviceType, "PATH", path, "MIN", *minId, "MAX", *maxId);
        return EffecterTarget{.terminus = terminus->name,
                              .eid = terminus->eid,
                              .path = path,
                              .minId = *minId,
                              .maxId = *maxId};
    }

    lg2::error(
        "PCoreDump: no {INTERFACE} object found for terminus {TERMINUS} (EID {EID})",
        "INTERFACE", pcoreDumpInterface, "TERMINUS", terminus->name, "EID",
        terminus->eid);
    return std::nullopt;
}

} // namespace phosphor::dump::pcore
