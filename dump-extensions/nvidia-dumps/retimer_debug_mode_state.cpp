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
#include "config.h"

#include "retimer_debug_mode_state.hpp"

#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <regex>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>
#include <chrono>
#include <iostream>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

namespace phosphor
{
namespace dump
{
namespace retimer
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;
using DebugModeIface =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::Dump::server::DebugMode>;


bool State::debugMode() const
{
    /* FPGA aggregate command for reading retimer debug mode from HMC:
    i2ctransfer -y 2 w1@0x60 0xe3 r2
    the return value contains 2 bytes, the first byte varies from 0x00 to 0xff.
    Each bit represent a single retimer, 0 means normal state, 1 means debug mode.
    The second byte implies who has the arbitary, 0x01 means HMC, 0x02 means HostBMC, 0x00 means none. */
    const char i2cBus[] = "/dev/i2c-2";
    unsigned char outbuf = 0xe3, saddr = 0x60;
    unsigned char inbuf[2];
    struct i2c_rdwr_ioctl_data packets;
    struct i2c_msg messages[2];

    int file = open(i2cBus, O_RDONLY);

    if (file < 0)
    {
        auto error = errno;
        log<level::ERR>("System dump: Failed to open the I2C bus",
                        entry("ERRNO=%d", error));
        return DebugModeIface::debugMode();
    }

    messages[0].addr  = saddr;
    messages[0].flags = 0x00;
    messages[0].len   = 1;
    messages[0].buf   = &outbuf;

    messages[1].addr  = saddr;
    messages[1].flags = I2C_M_RD;
    messages[1].len   = 2;
    messages[1].buf   = inbuf;

    packets.msgs      = messages;
    packets.nmsgs     = 2;

    if (ioctl(file, I2C_RDWR, &packets) < 0)
    {
        auto error = errno;
        log<level::ERR>(
            "System dump: Failed to read retimerDebugMode from FPGA",
            entry("ERRNO=%d", error));
        close(file);
        return DebugModeIface::debugMode();
    }

    close(file);
    if (inbuf[0] > 0)
    {
        return true;
    }
    
    return false;
}

bool State::debugMode(bool value)
{
    /* FPGA aggregate command for setting retimer debug mode from HMC:
    i2ctransfer -y 2 w3@0x60 0xe3 0xff 0x01 */
    const char i2cBus[] = "/dev/i2c-2";
    unsigned char saddr = 0x60;
    unsigned char *outbuf;
    if (value)
    {
        ServiceReadyIface::state(States::Enabled);
        static unsigned char command[3] = {0xe3, 0xff, 0x01};
        outbuf = &command[0];
    }
    else
    {
        ServiceReadyIface::state(States::Disabled);
        static unsigned char command[3] = {0xe3, 0x00, 0x00};
        outbuf = &command[0];
    }
    struct i2c_rdwr_ioctl_data packets;
    struct i2c_msg messages[1];

    int file = open(i2cBus, O_RDONLY);
    if (file < 0)
    {
        auto error = errno;
        log<level::ERR>("System dump: Failed to open the I2C bus",
                        entry("ERRNO=%d", error));
        return debugMode();
    }

    messages[0].addr  = saddr;
    messages[0].flags = 0x00;
    messages[0].len   = 3;
    messages[0].buf   = outbuf;

    packets.msgs      = messages;
    packets.nmsgs     = 1;

    if (ioctl(file, I2C_RDWR, &packets) < 0)
    {
        auto error = errno;
        log<level::ERR>(
            "System dump: Failed to write retimerDebugMode to FPGA",
            entry("ERRNO=%d", error));
        close(file);
        return debugMode();
    }
    close(file);

    return DebugModeIface::debugMode(value);
}

std::string State::getVendorId() const
{
    return retimerVendorId;
}

std::string State::getDBusObject(sdbusplus::bus::bus& bus, const std::string& rootPath)
{
    std::vector<std::string> paths;

    auto mapper = bus.new_method_call("xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
                                      "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
    mapper.append(rootPath.c_str());
    mapper.append(0); // Depth 0 to search all
    mapper.append(std::vector<std::string>({SWITCH_INTERFACE}));
    auto reply = bus.call(mapper);

    reply.read(paths);
    for (auto& path : paths)
    {
        if (path.find("PCIeRetimer") != std::string::npos)
        {
            return path;
        }
    }

    return {};
}

std::string State::getService(sdbusplus::bus::bus& bus, const char* path, const char* interface) const
{
    using DbusInterfaceList = std::vector<std::string>;
    std::map<std::string, std::vector<std::string>> mapperResponse;

    auto mapper = bus.new_method_call("xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
                                      "xyz.openbmc_project.ObjectMapper", "GetObject");
    mapper.append(path, DbusInterfaceList({interface}));

    auto mapperResponseMsg = bus.call(mapper);
    mapperResponseMsg.read(mapperResponse);
    return mapperResponse.begin()->first;
}

void State::listenRetimerVendorIdEvents(sdbusplus::bus::bus& bus)
{
    try
    {
        switchObjectAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            ("interface='org.freedesktop.DBus.Properties',type='signal',"
             "member='PropertiesChanged',arg0='xyz.openbmc_project.Inventory.Item.Switch',"),
            std::bind(std::mem_fn(&State::switchObjectCallback), this,
                    std::placeholders::_1));
    }
    catch (const std::exception &e)
    {
        lg2::error("Failed to set up event listening for retimer VendorId: {ERROR}", "ERROR", e);
    }
}

void State::switchObjectCallback(sdbusplus::message::message& m)
{
    std::string path = m.get_path();
    std::string interface;
    std::map<std::string, std::variant<std::string>> changedProperties;
    std::vector<std::string> invalidatedProperties;
    m.read(interface, changedProperties, invalidatedProperties);
    if (retimerVendorId.empty() && path.find("PCIeRetimer") != std::string::npos)
    {
        for (auto& propertyEntry : changedProperties)
        {
            if (propertyEntry.first == "VendorId")
            {
                retimerVendorId = std::get<0>(propertyEntry.second);
            }
        }
    }
}

} // namespace retimer
} // namespace dump
} // namespace phosphor
