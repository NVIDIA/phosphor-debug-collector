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

#include "em_args.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace phosphor
{
namespace dump
{
namespace collectors
{

/** Outcome of a reachability probe: Reachable -> collect; Unreachable ->
 *  write .error.json and exit 2; Skipped -> no probe, may continue. */
enum class ProbeResult
{
    Reachable,
    Unreachable,
    Skipped,
};

/** @brief Parse a string like "0x40" or "64" into a 7-bit I²C address. */
inline std::optional<uint16_t> parseI2cAddress(const std::string& s)
{
    if (s.empty())
    {
        return std::nullopt;
    }
    try
    {
        return static_cast<uint16_t>(std::stoul(s, nullptr, 0));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

/** Best-effort, non-destructive I²C probe: open /dev/i2c-<bus>, I2C_SLAVE
 *  ioctl, single 1-byte read. Used by every I²C-attached collector. */
inline ProbeResult probeI2c(const EmCollectorArgs& args)
{
    if (!args.i2cBus || !args.i2cAddress)
    {
        return ProbeResult::Skipped;
    }
    auto addr = parseI2cAddress(*args.i2cAddress);
    if (!addr)
    {
        lg2::warning("I2C probe skipped: unparsable I2CAddress '{ADDR}'",
                     "ADDR", *args.i2cAddress);
        return ProbeResult::Skipped;
    }

    std::string devPath = "/dev/i2c-" + std::to_string(*args.i2cBus);
    int fd = ::open(devPath.c_str(), O_RDWR);
    if (fd < 0)
    {
        lg2::warning("I2C probe: cannot open '{DEV}'", "DEV", devPath);
        return ProbeResult::Unreachable;
    }

    if (::ioctl(fd, I2C_SLAVE, static_cast<unsigned long>(*addr)) < 0)
    {
        ::close(fd);
        return ProbeResult::Unreachable;
    }

    uint8_t scratch = 0;
    ssize_t n = ::read(fd, &scratch, 1);
    ::close(fd);
    return (n >= 0) ? ProbeResult::Reachable : ProbeResult::Unreachable;
}

/** Best-effort MCTP reachability check via the userspace `mctp` tool; its
 *  exit code is the truth source. Returns Skipped if the tool is absent. */
inline ProbeResult probeMctp(const EmCollectorArgs& args)
{
    if (!args.mctpEid)
    {
        return ProbeResult::Skipped;
    }
    std::string cmd = "/usr/bin/mctp echo " + std::to_string(*args.mctpEid) +
                      " >/dev/null 2>&1";
    // NOLINTNEXTLINE(cert-env33-c)
    int rc = std::system(cmd.c_str());
    if (rc == -1)
    {
        return ProbeResult::Skipped;
    }
    return (WEXITSTATUS(rc) == 0) ? ProbeResult::Reachable
                                  : ProbeResult::Unreachable;
}

} // namespace collectors
} // namespace dump
} // namespace phosphor
