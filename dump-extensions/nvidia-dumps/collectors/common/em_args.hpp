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
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace collectors
{
namespace em_args
{

/** Thrown by the typed parsers below when the JSON does not match the EM
 *  schema; main() catches it and exits non-zero. */
class EmArgsParseError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/** Per-device descriptor for fan-out collectors (erot, sma).
 *  EM JSON: `{ "Name": "ERoT_GPU_0", "MctpEid": 24 }`. */
struct EmDevice
{
    std::string name;
    uint8_t mctpEid = 0;
};

/** Per-page descriptor for CPLD page dumps (cpld_dump.sh table format). */
struct EmCpldPage
{
    std::string idx;
    uint8_t bus = 0;
    std::string slaveAddr;
    std::optional<std::string> regAddr;
    std::optional<std::string> page;
    std::optional<uint32_t> size;
};

/** Parse `CollectorArgs.Devices` into a typed vector; throws on malformed
 *  input. */
std::vector<EmDevice> parseDevices(const nlohmann::json& collectorArgs);

/** Parse `CollectorArgs.Pages` into a typed vector; throws on malformed
 *  input. */
std::vector<EmCpldPage> parsePages(const nlohmann::json& collectorArgs);

/** Parse `CollectorArgs.DeviceType` (string); throws when absent or
 *  non-string. */
std::string parseDeviceType(const nlohmann::json& collectorArgs);

} // namespace em_args

/** Per-target arguments parsed from the EM
 * SystemDumpAllowableType.CollectorArgs property (`a{sv}`). Each field is
 * optional; only present keys are populated. Typed extraction only, no schema
 * validation. */
struct EmCollectorArgs
{
    /** Raw map kept for forwarding unknown keys as -K=V flags. */
    std::map<std::string, std::variant<std::string, int64_t, uint8_t, bool>>
        raw;

    std::optional<int64_t> i2cBus;
    std::optional<std::string> i2cAddress; // string — EM JSON uses "0x40"
    std::optional<uint8_t> mctpEid;
    std::optional<std::string> deviceType;
    std::optional<std::string> usbPort;

    /** Lookup a string-valued key; nullopt if absent or wrong type. */
    std::optional<std::string> getString(const std::string& key) const
    {
        auto it = raw.find(key);
        if (it == raw.end())
        {
            return std::nullopt;
        }
        if (auto p = std::get_if<std::string>(&it->second))
        {
            return *p;
        }
        return std::nullopt;
    }
};

using EmVariantMap =
    std::map<std::string, std::variant<std::string, int64_t, uint8_t, bool>>;

/** Parse a `CollectorArgs` variant map into the typed struct above; missing
 *  keys become std::nullopt and the raw map is always preserved. */
inline EmCollectorArgs parseCollectorArgs(const EmVariantMap& variantMap)
{
    EmCollectorArgs out;
    out.raw = variantMap;

    auto lookup = [&](const std::string& key) -> const auto* {
        auto it = variantMap.find(key);
        return it == variantMap.end() ? nullptr : &it->second;
    };

    if (const auto* v = lookup("I2CBus"))
    {
        if (auto p = std::get_if<int64_t>(v))
        {
            out.i2cBus = *p;
        }
    }
    if (const auto* v = lookup("I2CAddress"))
    {
        if (auto p = std::get_if<std::string>(v))
        {
            out.i2cAddress = *p;
        }
    }
    if (const auto* v = lookup("MctpEid"))
    {
        if (auto p = std::get_if<uint8_t>(v))
        {
            out.mctpEid = *p;
        }
        else if (auto p = std::get_if<int64_t>(v))
        {
            // EM JSON integers come through as int64_t; narrow to uint8_t.
            if (*p < 0 || *p > UINT8_MAX)
            {
                throw em_args::EmArgsParseError("MctpEid out of uint8_t range");
            }
            out.mctpEid = static_cast<uint8_t>(*p);
        }
    }
    if (const auto* v = lookup("DeviceType"))
    {
        if (auto p = std::get_if<std::string>(v))
        {
            out.deviceType = *p;
        }
    }
    if (const auto* v = lookup("UsbPort"))
    {
        if (auto p = std::get_if<std::string>(v))
        {
            out.usbPort = *p;
        }
    }

    return out;
}

} // namespace collectors
} // namespace dump
} // namespace phosphor
