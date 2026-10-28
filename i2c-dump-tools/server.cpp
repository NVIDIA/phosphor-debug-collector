// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// hmc-dump-server - runs on the HMC: creates dumps for the BMC and stages
// them block by block in the I2C mailbox for hmc-dump-client to copy. Polls
// the control EEPROM every second for a request.
//
// The mailbox EEPROMs are I2C target backends that platform-init creates; the
// server finds them in sysfs by their target address. A platform with another
// mailbox layout passes it on the command line:
//   --control-eeprom <path>     control EEPROM (default: target at 0x45)
//   --data-eeprom <path>        data EEPROM (default: target at 0x4f)
//   --block-offset <n>          dump block size in the data EEPROM, the block
//                               follows 4 bytes later (default 0x700)
//   --system-dump-types <list>  DiagnosticType per system dump sub-type, from
//                               1; "Type:Check" also sets HWCheckType
//                               (default FPGA,EROT)

#include "mailbox.hpp"
#include "server_util.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/sdbus.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace phosphor::dump::hmc_mailbox;

namespace
{

// Each command gets three D-Bus calls at most; together they stay inside the
// 30 second command timeout of hmc-dump-client.
constexpr sdbusplus::SdBusDuration dbusTimeout = std::chrono::seconds{5};

class Server
{
  public:
    explicit Server(Layout layout) :
        control(std::move(layout.control)), data(std::move(layout.data)),
        blockSizeOffset(layout.blockSizeOffset),
        systemDumpTypes(std::move(layout.systemDumpTypes))
    {}

    bool hasRequest()
    {
        auto request = eepromRead(control, request_offset, 1);
        return !request.empty() && request[0] == 1;
    }

    void processCommand()
    {
        runCommand(control, [this](std::span<const uint8_t> args) {
            switch (args[0])
            {
                case create_dump:
                    return createNewDump(args[1], args[2]);
                case dump_progress:
                    return checkDumpProgress(args[1], be16(args, 2));
                case dump_block:
                    return requestBlockData(args[1], be16(args, 2),
                                            be16(args, 4));
                default:
                    std::cerr
                        << std::format("ERROR: Unknown opcode: {}\n", args[0]);
                    return 1;
            }
        });
    }

  private:
    int createNewDump(uint8_t dumpType, uint8_t dumpSubType)
    {
        std::cerr << std::format("Received request to create new dump. Dump "
                                 "type: {}. Dump sub type: {}\n",
                                 dumpType, dumpSubType);
        const char* type = dumpTypeStr(dumpType);
        if (type == nullptr)
        {
            return 1;
        }

        std::map<std::string, std::variant<std::string>> params;
        if (dumpType == 2)
        {
            if (dumpSubType == 0 || dumpSubType > systemDumpTypes.size())
            {
                std::cerr << std::format(
                    "dump_sub_type({}) must be a number between 1 and {}\n",
                    dumpSubType, systemDumpTypes.size());
                return 1;
            }
            const auto& [diagnostic, check] = systemDumpTypes[dumpSubType - 1];
            params["DiagnosticType"] = diagnostic;
            if (!check.empty())
            {
                params["HWCheckType"] = check;
            }
        }

        sdbusplus::object_path entry;
        try
        {
            auto m = bus.new_method_call(
                "xyz.openbmc_project.Dump.Manager",
                std::format("/xyz/openbmc_project/dump/{}", type).c_str(),
                "xyz.openbmc_project.Dump.Create", "CreateDump");
            m.append(params);
            bus.call(m, dbusTimeout).read(entry);
        }
        catch (const sdbusplus::exception_t& e)
        {
            std::cerr << std::format("CreateDump failed: {}\n", e.what());
            std::string_view msg =
                e.description() != nullptr ? e.description() : "";
            if (msg.contains("The operation is not allowed") ||
                msg.contains("The service is temporarily unavailable"))
            {
                return dump_busy;
            }
            return 1;
        }

        unsigned long entryId = 0;
        try
        {
            entryId = std::stoul(entry.filename());
        }
        catch (const std::exception&)
        {
            std::cerr << std::format("CreateDump returned {}\n", entry.str);
        }
        if (!eepromWrite(control, result_offset, beBytes(entryId, 2)))
        {
            return 1;
        }
        std::cerr << std::format("Dump({}) creation initiated successfully.\n",
                                 entryId);
        return 0;
    }

    int checkDumpProgress(uint8_t dumpType, uint16_t entryId)
    {
        std::cerr << std::format("Received request to check dump progress. "
                                 "Dump type: {}. Entry id: {}\n",
                                 dumpType, entryId);
        const char* type = dumpTypeStr(dumpType);
        if (type == nullptr)
        {
            return 1;
        }
        const std::string entryPath =
            std::format("/xyz/openbmc_project/dump/{}/entry/{}", type, entryId);

        auto get = [&]<typename T>(const char* iface, const char* prop) {
            auto m = bus.new_method_call(
                "xyz.openbmc_project.Dump.Manager", entryPath.c_str(),
                "org.freedesktop.DBus.Properties", "Get");
            m.append(iface, prop);
            std::variant<T> v;
            bus.call(m, dbusTimeout).read(v);
            return std::get<T>(v);
        };

        std::string status;
        try
        {
            status = get.template operator()<std::string>(
                "xyz.openbmc_project.Common.Progress", "Status");
            status = status.substr(status.rfind('.') + 1);
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("Failed to check dump progress: {}\n",
                                     e.what());
            return 1;
        }

        uint8_t percentage = 0;
        try
        {
            percentage = get.template operator()<uint8_t>(
                "xyz.openbmc_project.Common.Progress", "Progress");
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format(
                "WARNING: Failed to get dump progress percentage: {}\n",
                e.what());
        }

        const uint8_t execStatus = executionStatus(status);
        uint64_t dumpSize = 0;
        if (execStatus == 0x01)
        {
            try
            {
                dumpSize = get.template operator()<uint64_t>(
                    "xyz.openbmc_project.Dump.Entry", "Size");
            }
            catch (const std::exception& e)
            {
                // The client retries a failed progress check.
                std::cerr << std::format("Failed to get dump size: {}\n",
                                         e.what());
                return 1;
            }
        }

        if (!eepromWrite(control, result_offset,
                         progressResult(execStatus, percentage, dumpSize)))
        {
            return 1;
        }

        std::cerr << std::format(
            "Dump({}) status: {}. Percentage: {}%. Dump size: {} bytes\n",
            entryId, status, percentage, dumpSize);
        return 0;
    }

    int requestBlockData(uint8_t dumpType, uint16_t entryId,
                         uint16_t blockNumber)
    {
        std::cerr << std::format("Received request get block data. Dump "
                                 "type: {}. Entry id: {}. Block number: {}\n",
                                 dumpType, entryId, blockNumber);
        const char* type = dumpTypeStr(dumpType);
        if (type == nullptr)
        {
            return 1;
        }

        // The entry directory must hold exactly one dump file.
        return stageBlock(
            std::format("/var/lib/logging/dumps/{}/{}", type, entryId),
            blockNumber, data, blockSizeOffset);
    }

    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    std::string control;
    std::string data;
    std::size_t blockSizeOffset;
    std::vector<std::pair<std::string, std::string>> systemDumpTypes;
};

} // namespace

int main(int argc, char** argv)
{
    auto options =
        parseOptions(std::vector<std::string>(argv + 1, argv + argc));
    if (!options)
    {
        return 1;
    }
    auto& layout = options->layout;
    const auto& controlPath = options->controlPath;
    const auto& dataPath = options->dataPath;

    std::cerr << "Checking if EEPROM files exist...\n";
    auto find = [](const std::string& path, uint8_t address) {
        std::error_code ec;
        if (path.empty())
        {
            return findTargetEeprom(address);
        }
        return std::filesystem::is_regular_file(path, ec)
                   ? path
                   : std::string{};
    };
    for (int i = 0; i < 3; ++i)
    {
        layout.control = find(controlPath, control_address);
        layout.data = find(dataPath, data_address);
        if (!layout.control.empty() && !layout.data.empty())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }
    if (layout.control.empty() || layout.data.empty())
    {
        std::cerr << std::format(
            "Need the control EEPROM ({}) and the data EEPROM ({}). "
            "Exiting...\n",
            controlPath.empty()
                ? std::format("target at 0x{:02x}", control_address)
                : controlPath,
            dataPath.empty() ? std::format("target at 0x{:02x}", data_address)
                             : dataPath);
        return 1;
    }

    Server server(std::move(layout));
    std::cerr
        << "Start processing commands. Waiting for the first command...\n";
    while (true)
    {
        if (server.hasRequest())
        {
            server.processCommand();
        }
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}
