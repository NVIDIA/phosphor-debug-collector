// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// What hmc-mailbox needs to know about its board: detected at runtime where
// it can be, else from the optional key=value file configFile. A missing file
// or key leaves the vr-nvl-hmc (AST2600) value.

#pragma once

#include "conf_file.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace phosphor::dump::hmc_mailbox
{

constexpr auto configFile = "/usr/share/hmc-mailbox/hmc-mailbox.conf";

struct Config
{
    // Unset: the bus that has the EEPROM at the address.
    std::optional<unsigned> telemetryBus;
    std::optional<unsigned> controlBus;
    uint8_t telemetryAddress = 0x4f;
    uint8_t controlAddress = 0x45;
    // MtdErase, Initramfs or None; unset: MtdErase when rwfs is on MTD, else
    // None.
    std::string rwfsRecovery;
    bool fixedMctpStatus = false;
    bool forceReboot = false;
    bool signatureCheck = true;
    std::string softRebootReason =
        "Reboot triggered by external command over I2C";
    unsigned startupDelay = 0;
    std::string bmcSyslogAddress = "172.31.13.241";
};

using conf::parseUnsigned;

inline std::optional<bool> parseBool(std::string_view s)
{
    if (s == "true" || s == "1")
    {
        return true;
    }
    if (s == "false" || s == "0")
    {
        return false;
    }
    return std::nullopt;
}

// Sets one key; false when the key or its value is not understood.
inline bool setKey(Config& c, std::string_view key, std::string_view value)
{
    if (key == "TelemetryBus" || key == "ControlBus")
    {
        auto v = parseUnsigned(value);
        (key == "TelemetryBus" ? c.telemetryBus : c.controlBus) = v;
        return v.has_value();
    }
    if (key == "TelemetryAddress" || key == "ControlAddress")
    {
        auto v = parseUnsigned(value);
        if (!v || *v > 0x7f)
        {
            return false;
        }
        (key == "TelemetryAddress" ? c.telemetryAddress : c.controlAddress) =
            static_cast<uint8_t>(*v);
        return true;
    }
    if (key == "RwfsRecovery")
    {
        if (value != "MtdErase" && value != "Initramfs" && value != "None")
        {
            return false;
        }
        c.rwfsRecovery = value;
        return true;
    }
    if (key == "FixedMctpStatus" || key == "ForceReboot" ||
        key == "SignatureCheck")
    {
        auto v = parseBool(value);
        if (!v)
        {
            return false;
        }
        (key == "FixedMctpStatus" ? c.fixedMctpStatus
         : key == "ForceReboot"   ? c.forceReboot
                                  : c.signatureCheck) = *v;
        return true;
    }
    if (key == "StartupDelay")
    {
        auto v = parseUnsigned(value);
        c.startupDelay = v.value_or(c.startupDelay);
        return v.has_value();
    }
    if (key == "SoftRebootReason" || key == "BmcSyslogAddress")
    {
        if (value.empty())
        {
            return false;
        }
        (key == "SoftRebootReason" ? c.softRebootReason : c.bmcSyslogAddress) =
            value;
        return true;
    }
    return false;
}

// Lines not understood are logged and skipped.
inline Config parseConfig(std::istream& in)
{
    Config c;
    for (const auto& e : conf::read(in))
    {
        if (!e.ok || !setKey(c, e.key, e.value))
        {
            lg2::error("{FILE} line {LINE} ignored: {TEXT}", "FILE", configFile,
                       "LINE", e.line, "TEXT",
                       e.ok ? e.key + "=" + e.value : e.key);
        }
    }
    return c;
}

// The bus of the only I2C target EEPROM at addr (<bus>-10<addr>/slave-eeprom
// under devices), or nothing when there is none or more than one.
inline std::optional<unsigned> findEepromBus(
    const std::filesystem::path& devices, uint8_t addr)
{
    const auto suffix = std::format("-10{:02x}", addr);
    std::vector<unsigned> buses;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(devices, ec))
    {
        const auto name = e.path().filename().string();
        if (!name.ends_with(suffix) ||
            !std::filesystem::exists(e.path() / "slave-eeprom", ec))
        {
            continue;
        }
        if (auto bus = parseUnsigned(
                std::string_view(name).substr(0, name.size() - suffix.size())))
        {
            buses.push_back(*bus);
        }
    }
    if (buses.size() != 1)
    {
        return std::nullopt;
    }
    return buses.front();
}

// MtdErase when rwfs is mounted from MTD, or not mounted while an MTD
// partition is named rwfs; None otherwise (eMMC).
inline std::string detectRwfsRecovery(const std::filesystem::path& mounts,
                                      const std::filesystem::path& mtdClass,
                                      std::string_view rwfsMount)
{
    std::ifstream f(mounts);
    for (std::string line; std::getline(f, line);)
    {
        std::istringstream fields(line);
        std::string device;
        std::string mount;
        if (fields >> device >> mount && mount == rwfsMount)
        {
            return device.starts_with("/dev/mtd") ? "MtdErase" : "None";
        }
    }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(mtdClass, ec))
    {
        std::string name;
        std::getline(std::ifstream(e.path() / "name"), name);
        if (name == "rwfs")
        {
            return "MtdErase";
        }
    }
    return "None";
}

// The first IPv4 address in networkctl's order: global before link-local,
// then lowest. Zero when there is none.
inline std::array<uint8_t, 4> firstIpv4(const std::vector<std::string>& addrs)
{
    std::vector<std::pair<bool, std::array<uint8_t, 4>>> parsed;
    for (const auto& s : addrs)
    {
        std::array<uint8_t, 4> a{};
        std::string_view rest(s);
        bool ok = true;
        for (std::size_t i = 0; i < a.size() && ok; ++i)
        {
            const auto dot = i + 1 < a.size() ? rest.find('.') : rest.size();
            auto v = dot == std::string_view::npos
                         ? std::nullopt
                         : parseUnsigned(rest.substr(0, dot));
            ok = v && *v <= 255 && !rest.substr(0, dot).starts_with("0x");
            a[i] = static_cast<uint8_t>(v.value_or(0));
            rest.remove_prefix(std::min(rest.size(), dot + 1));
        }
        if (ok)
        {
            parsed.emplace_back(a[0] == 169 && a[1] == 254, a);
        }
    }
    if (parsed.empty())
    {
        return {};
    }
    return std::ranges::min(parsed).second;
}

} // namespace phosphor::dump::hmc_mailbox
