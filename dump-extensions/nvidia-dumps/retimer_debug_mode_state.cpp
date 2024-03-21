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


bool DebugMode::debugMode() const
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

bool DebugMode::debugMode(bool value)
{
    /* FPGA aggregate command for setting retimer debug mode from HMC:
    i2ctransfer -y 2 w3@0x60 0xe3 0xff 0x01 */
    const char i2cBus[] = "/dev/i2c-2";
    unsigned char saddr = 0x60;
    unsigned char *outbuf;
    if (value)
    {
        static unsigned char command[3] = {0xe3, 0xff, 0x01};
        outbuf = &command[0];
    }
    else
    {
        static unsigned char command[3] = {0xe3, 0x00, 0x00};
        outbuf = &command[0];
    }
    struct i2c_rdwr_ioctl_data packets;
    struct i2c_msg messages[1];

    int file = open(i2cBus, O_RDONLY);
    
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

} // namespace retimer
} // namespace dump
} // namespace phosphor
