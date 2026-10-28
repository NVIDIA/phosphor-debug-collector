// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// The parts of hmc-dump-server that need neither D-Bus nor the HMC: option
// parsing, EEPROM file access and the mailbox command framing.

#pragma once

#include "mailbox.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace phosphor::dump::hmc_mailbox
{

// <root>/<bus>-10<address>/slave-eeprom; 0x1000 marks an I2C target
// backend. Empty when there is none, or more than one.
inline std::string findTargetEeprom(
    uint8_t address, const std::filesystem::path& root = "/sys/bus/i2c/devices")
{
    const std::string suffix = std::format("-10{:02x}", address);
    std::string found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec))
    {
        const auto& path = it->path();
        auto eeprom = path / "slave-eeprom";
        std::error_code fileEc;
        if (!path.filename().string().ends_with(suffix) ||
            !std::filesystem::is_regular_file(eeprom, fileEc))
        {
            continue;
        }
        if (!found.empty())
        {
            std::cerr << std::format("More than one I2C target at 0x{:02x}\n",
                                     address);
            return {};
        }
        found = eeprom.string();
    }
    return found;
}

inline bool eepromWrite(const std::string& path, std::size_t offset,
                        std::span<const uint8_t> data)
{
    std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f)
    {
        std::cerr << std::format("cannot write {} bytes to {} at {}\n",
                                 data.size(), path, offset);
        return false;
    }
    return true;
}

// Shorter than len, or empty, when the file cannot be read.
inline std::vector<uint8_t> eepromRead(const std::string& path,
                                       std::size_t offset, std::size_t len)
{
    std::vector<uint8_t> buf(len);
    std::ifstream f{path, std::ios::binary};
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(len));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return buf;
}

inline std::vector<uint8_t> beBytes(uint64_t value, std::size_t count)
{
    std::vector<uint8_t> out(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        out[count - 1 - i] = static_cast<uint8_t>(value >> (8 * i));
    }
    return out;
}

inline uint16_t be16(std::span<const uint8_t> b, std::size_t i)
{
    return static_cast<uint16_t>((b[i] << 8) | b[i + 1]);
}

struct Layout
{
    std::string control;
    std::string data;
    std::size_t blockSizeOffset = block_size_offset;
    std::vector<std::pair<std::string, std::string>> systemDumpTypes = {
        {"FPGA", ""}, {"EROT", ""}};
};

// "Type[:Check],..." to DiagnosticType and HWCheckType pairs.
inline std::vector<std::pair<std::string, std::string>> parseDumpTypes(
    std::string_view list)
{
    std::vector<std::pair<std::string, std::string>> types;
    while (!list.empty())
    {
        auto entry = list.substr(0, list.find(','));
        list.remove_prefix(std::min(list.size(), entry.size() + 1));
        auto colon = entry.find(':');
        auto diagnostic = entry.substr(0, colon);
        if (diagnostic.empty())
        {
            throw std::invalid_argument("empty DiagnosticType");
        }
        types.emplace_back(std::string(diagnostic),
                           colon == std::string_view::npos
                               ? std::string{}
                               : std::string(entry.substr(colon + 1)));
    }
    return types;
}

struct Options
{
    std::string controlPath;
    std::string dataPath;
    Layout layout;
};

// Empty, after printing why, when an option is unknown or has no or a bad
// value.
inline std::optional<Options> parseOptions(std::span<const std::string> args)
{
    Options options;
    for (std::size_t i = 0; i + 1 < args.size(); i += 2)
    {
        const std::string& opt = args[i];
        const std::string& value = args[i + 1];
        try
        {
            if (opt == "--control-eeprom")
            {
                options.controlPath = value;
            }
            else if (opt == "--data-eeprom")
            {
                options.dataPath = value;
            }
            else if (opt == "--block-offset")
            {
                options.layout.blockSizeOffset = std::stoul(value, nullptr, 0);
            }
            else if (opt == "--system-dump-types")
            {
                options.layout.systemDumpTypes = parseDumpTypes(value);
            }
            else
            {
                std::cerr << std::format("Unknown option {}\n", opt);
                return std::nullopt;
            }
        }
        catch (const std::exception&)
        {
            std::cerr << std::format("Bad value for {}: {}\n", opt, value);
            return std::nullopt;
        }
    }
    if (args.size() % 2 != 0)
    {
        std::cerr << "Options take one value each\n";
        return std::nullopt;
    }
    if (options.layout.systemDumpTypes.empty())
    {
        std::cerr << "--system-dump-types needs at least one type\n";
        return std::nullopt;
    }
    return options;
}

inline const char* dumpTypeStr(uint8_t dumpType)
{
    switch (dumpType)
    {
        case 1:
            return "bmc";
        case 2:
            return "system";
        default:
            std::cerr << std::format(
                "dump_type({}) must be a number between 1 and 2\n", dumpType);
            return nullptr;
    }
}

// Clears the request flag and marks the command running, hands the opcode
// and arguments to handle, then publishes its return code and clears the
// running flag. Stops at the first failed control write: a request it cannot
// take is left for the next poll, and a return code it cannot publish leaves
// the running flag set so the client times out instead of reading a stale
// code.
template <typename Handler>
bool runCommand(const std::string& control, Handler&& handle)
{
    if (!eepromWrite(control, request_offset, std::to_array<uint8_t>({0, 1})))
    {
        return false;
    }
    auto args = eepromRead(control, opcode_offset, 1 + max_args);
    args.resize(1 + max_args);
    const int rc =
        std::forward<Handler>(handle)(std::span<const uint8_t>(args));
    if (!eepromWrite(control, return_code_offset,
                     std::array{static_cast<uint8_t>(rc)}))
    {
        return false;
    }
    return eepromWrite(control, running_offset, std::to_array<uint8_t>({0}));
}

// Stages block blockNumber of the one dump file in dir in the data EEPROM:
// its size at blockSizeOffset, the data 4 bytes later. Returns the command
// return code.
inline int stageBlock(const std::filesystem::path& dir, uint16_t blockNumber,
                      const std::string& data, std::size_t blockSizeOffset)
{
    std::vector<std::filesystem::path> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (!e.path().filename().string().starts_with('.'))
        {
            names.push_back(e.path());
        }
    }
    if (names.size() != 1 || !std::filesystem::is_regular_file(names[0], ec))
    {
        std::cerr << std::format("Couldn't find dump in {}\n", dir.string());
        return 1;
    }
    const auto& dumpPath = names[0];
    auto dumpSize = std::filesystem::file_size(dumpPath, ec);
    if (ec)
    {
        std::cerr << std::format("Couldn't read dump size of {}\n",
                                 dumpPath.string());
        return 1;
    }

    const std::size_t skip = std::size_t{blockNumber} * max_block_size;
    if (skip > dumpSize)
    {
        std::cerr << std::format(
            "block_number({}) is too big for a given file. {} * {} > {}\n",
            blockNumber, max_block_size, blockNumber, dumpSize);
        return 1;
    }
    const std::size_t blockSize =
        std::min<std::size_t>(dumpSize - skip, max_block_size);

    if (!eepromWrite(data, blockSizeOffset, beBytes(blockSize, 4)))
    {
        std::cerr << "Failed to write block size to eeprom file.\n";
        return 1;
    }

    std::vector<uint8_t> block(blockSize);
    std::ifstream f{dumpPath, std::ios::binary};
    f.seekg(static_cast<std::streamoff>(skip));
    f.read(reinterpret_cast<char*>(block.data()),
           static_cast<std::streamsize>(blockSize));
    if (!f || !eepromWrite(data, blockSizeOffset + 4, block))
    {
        std::cerr << "Failed to write memory block to eeprom file.\n";
        return 1;
    }

    std::cerr << std::format(
        "data block {} successfully prepared. Block size: {}\n", blockNumber,
        blockSize);
    return 0;
}

// Execution status for a Progress Status value: 0 in progress, 1 completed,
// 2 failed.
inline uint8_t executionStatus(std::string_view status)
{
    if (status == "InProgress")
    {
        return 0x00;
    }
    if (status == "Completed")
    {
        return 0x01;
    }
    return 0x02;
}

// The dump progress result: execution status, percent, size (big-endian).
inline std::vector<uint8_t> progressResult(uint8_t status, uint8_t percentage,
                                           uint64_t dumpSize)
{
    std::vector<uint8_t> result{status, percentage};
    auto size = beBytes(dumpSize, 4);
    result.insert(result.end(), size.begin(), size.end());
    return result;
}

} // namespace phosphor::dump::hmc_mailbox
