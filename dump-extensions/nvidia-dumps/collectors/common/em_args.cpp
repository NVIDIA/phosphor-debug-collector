/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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

#include "em_args.hpp"

#include <string_view>

namespace phosphor
{
namespace dump
{
namespace collectors
{
namespace em_args
{

namespace
{

/** Resolve the `<key>` array from either a CollectorArgs object or a value
 *  already unwrapped to the array itself. `key` is string_view to avoid a
 *  temporary that trips GCC 13+'s [-Wdangling-reference] on the return. */
const nlohmann::json& resolveArray(const nlohmann::json& collectorArgs,
                                   std::string_view key)
{
    if (collectorArgs.is_array())
    {
        return collectorArgs;
    }
    if (!collectorArgs.is_object())
    {
        throw EmArgsParseError(
            "collectorArgs must be a JSON object or array (got " +
            std::string(collectorArgs.type_name()) + ")");
    }
    // find() takes string_view directly (nlohmann v3.11+).
    auto it = collectorArgs.find(key);
    if (it == collectorArgs.end())
    {
        throw EmArgsParseError(
            "collectorArgs missing key '" + std::string(key) + "'");
    }
    if (!it->is_array())
    {
        throw EmArgsParseError(
            "collectorArgs['" + std::string(key) + "'] must be an array");
    }
    return *it;
}

} // namespace

std::vector<EmDevice> parseDevices(const nlohmann::json& collectorArgs)
{
    const auto& arr = resolveArray(collectorArgs, "Devices");
    std::vector<EmDevice> out;
    out.reserve(arr.size());
    for (const auto& entry : arr)
    {
        if (!entry.is_object())
        {
            throw EmArgsParseError("Devices entry must be an object (got " +
                                   std::string(entry.type_name()) + ")");
        }
        EmDevice d;
        auto nameIt = entry.find("Name");
        if (nameIt == entry.end() || !nameIt->is_string())
        {
            throw EmArgsParseError("Devices entry missing string 'Name'");
        }
        d.name = nameIt->get<std::string>();

        auto eidIt = entry.find("MctpEid");
        if (eidIt == entry.end() || !eidIt->is_number_integer())
        {
            throw EmArgsParseError(
                "Devices entry '" + d.name + "' missing integer 'MctpEid'");
        }
        auto eid = eidIt->get<int64_t>();
        if (eid < 0 || eid > 0xFF)
        {
            throw EmArgsParseError(
                "Devices entry '" + d.name + "' MctpEid out of range");
        }
        d.mctpEid = static_cast<uint8_t>(eid);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<EmCpldPage> parsePages(const nlohmann::json& collectorArgs)
{
    const auto& arr = resolveArray(collectorArgs, "Pages");
    std::vector<EmCpldPage> out;
    out.reserve(arr.size());
    for (const auto& entry : arr)
    {
        if (!entry.is_object())
        {
            throw EmArgsParseError("Pages entry must be an object (got " +
                                   std::string(entry.type_name()) + ")");
        }
        EmCpldPage p;

        auto idxIt = entry.find("Idx");
        if (idxIt == entry.end())
        {
            throw EmArgsParseError("Pages entry missing 'Idx'");
        }
        if (idxIt->is_string())
        {
            p.idx = idxIt->get<std::string>();
        }
        else if (idxIt->is_number_integer())
        {
            p.idx = std::to_string(idxIt->get<int64_t>());
        }
        else
        {
            throw EmArgsParseError("Pages entry 'Idx' must be string or int");
        }

        auto busIt = entry.find("Bus");
        if (busIt == entry.end() || !busIt->is_number_integer())
        {
            throw EmArgsParseError("Pages entry missing integer 'Bus'");
        }
        auto bus = busIt->get<int64_t>();
        if (bus < 0 || bus > 0xFF)
        {
            throw EmArgsParseError("Pages entry Bus out of range");
        }
        p.bus = static_cast<uint8_t>(bus);

        auto addrIt = entry.find("SlaveAddr");
        if (addrIt == entry.end() || !addrIt->is_string())
        {
            throw EmArgsParseError("Pages entry missing string 'SlaveAddr'");
        }
        p.slaveAddr = addrIt->get<std::string>();

        if (auto it = entry.find("RegAddr");
            it != entry.end() && it->is_string())
        {
            p.regAddr = it->get<std::string>();
        }
        if (auto it = entry.find("Page"); it != entry.end() && it->is_string())
        {
            p.page = it->get<std::string>();
        }
        if (auto it = entry.find("Size");
            it != entry.end() && it->is_number_integer())
        {
            auto sz = it->get<int64_t>();
            if (sz < 0 || sz > static_cast<int64_t>(UINT32_MAX))
            {
                throw EmArgsParseError("Pages entry Size out of range");
            }
            p.size = static_cast<uint32_t>(sz);
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::string parseDeviceType(const nlohmann::json& collectorArgs)
{
    if (collectorArgs.is_string())
    {
        return collectorArgs.get<std::string>();
    }
    if (!collectorArgs.is_object())
    {
        throw EmArgsParseError(
            "collectorArgs must be a JSON object or string for DeviceType");
    }
    auto it = collectorArgs.find("DeviceType");
    if (it == collectorArgs.end() || !it->is_string())
    {
        throw EmArgsParseError("collectorArgs missing string key 'DeviceType'");
    }
    return it->get<std::string>();
}

} // namespace em_args
} // namespace collectors
} // namespace dump
} // namespace phosphor
