// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "i2c-dump-tools/server_util.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;
using namespace phosphor::dump::hmc_mailbox;

class TempDir : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::string tmpl = (fs::temp_directory_path() / "hmcdumpXXXXXX");
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir = tmpl;
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string makeFile(const fs::path& path, std::size_t size,
                         uint8_t fill = 0)
    {
        fs::create_directories(path.parent_path());
        std::ofstream f{path, std::ios::binary};
        const std::vector<char> bytes(size, static_cast<char>(fill));
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path.string();
    }

    fs::path dir;
};

TEST(ParseDumpTypes, TypesAndChecks)
{
    auto types = parseDumpTypes("FPGA,EROT:Full,X:");
    ASSERT_EQ(types.size(), 3U);
    EXPECT_EQ(types[0], std::make_pair(std::string("FPGA"), std::string()));
    EXPECT_EQ(types[1],
              std::make_pair(std::string("EROT"), std::string("Full")));
    EXPECT_EQ(types[2], std::make_pair(std::string("X"), std::string()));
    EXPECT_TRUE(parseDumpTypes("").empty());
}

TEST(ParseDumpTypes, RejectsEmptyType)
{
    EXPECT_THROW(parseDumpTypes(",FPGA"), std::invalid_argument);
    EXPECT_THROW(parseDumpTypes("FPGA,,EROT"), std::invalid_argument);
    EXPECT_THROW(parseDumpTypes(":Full"), std::invalid_argument);
}

TEST(ParseOptions, Defaults)
{
    auto parsed = parseOptions({});
    ASSERT_TRUE(parsed.has_value());
    const Options options = parsed.value_or(Options{});
    EXPECT_TRUE(options.controlPath.empty());
    EXPECT_TRUE(options.dataPath.empty());
    EXPECT_EQ(options.layout.blockSizeOffset, block_size_offset);
    ASSERT_EQ(options.layout.systemDumpTypes.size(), 2U);
    EXPECT_EQ(options.layout.systemDumpTypes[0].first, "FPGA");
    EXPECT_EQ(options.layout.systemDumpTypes[1].first, "EROT");
}

TEST(ParseOptions, AllOptions)
{
    const std::vector<std::string> args{
        "--control-eeprom", "/c",    "--data-eeprom",       "/d",
        "--block-offset",   "0x100", "--system-dump-types", "A:B"};
    auto parsed = parseOptions(args);
    ASSERT_TRUE(parsed.has_value());
    const Options options = parsed.value_or(Options{});
    EXPECT_EQ(options.controlPath, "/c");
    EXPECT_EQ(options.dataPath, "/d");
    EXPECT_EQ(options.layout.blockSizeOffset, 0x100U);
    ASSERT_EQ(options.layout.systemDumpTypes.size(), 1U);
    EXPECT_EQ(options.layout.systemDumpTypes[0].second, "B");
}

TEST(ParseOptions, Errors)
{
    using Args = std::vector<std::string>;
    EXPECT_FALSE(parseOptions(Args{"--bogus", "1"}));
    EXPECT_FALSE(parseOptions(Args{"--control-eeprom"}));
    EXPECT_FALSE(parseOptions(Args{"--block-offset", "x"}));
    EXPECT_FALSE(parseOptions(Args{"--system-dump-types", ""}));
    EXPECT_FALSE(parseOptions(Args{"--system-dump-types", "A,,B"}));
}

TEST(Encoding, BigEndian)
{
    EXPECT_EQ(beBytes(0x01020304, 4), (std::vector<uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(beBytes(0x1234, 2), (std::vector<uint8_t>{0x12, 0x34}));
    const std::vector<uint8_t> b{0, 0xab, 0xcd};
    EXPECT_EQ(be16(b, 1), 0xabcd);
}

TEST(Encoding, DumpType)
{
    EXPECT_STREQ(dumpTypeStr(1), "bmc");
    EXPECT_STREQ(dumpTypeStr(2), "system");
    EXPECT_EQ(dumpTypeStr(0), nullptr);
    EXPECT_EQ(dumpTypeStr(3), nullptr);
}

TEST(Encoding, Progress)
{
    EXPECT_EQ(executionStatus("InProgress"), 0x00);
    EXPECT_EQ(executionStatus("Completed"), 0x01);
    EXPECT_EQ(executionStatus("Failed"), 0x02);
    EXPECT_EQ(progressResult(1, 100, 0x00010203),
              (std::vector<uint8_t>{1, 100, 0, 1, 2, 3}));
}

TEST_F(TempDir, EepromReadWrite)
{
    auto file = makeFile(dir / "eeprom", 16);
    EXPECT_TRUE(eepromWrite(file, 4, std::vector<uint8_t>{7, 8}));
    EXPECT_EQ(eepromRead(file, 3, 4), (std::vector<uint8_t>{0, 7, 8, 0}));
    EXPECT_EQ(eepromRead(file, 14, 4).size(), 2U);
    EXPECT_TRUE(eepromRead((dir / "none").string(), 0, 4).empty());
    EXPECT_FALSE(
        eepromWrite((dir / "none").string(), 0, std::vector<uint8_t>{1}));
}

TEST_F(TempDir, FindTargetEeprom)
{
    EXPECT_TRUE(findTargetEeprom(0x45, dir).empty());
    makeFile(dir / "3-0045" / "slave-eeprom", 1);
    fs::create_directories(dir / "4-104f");
    EXPECT_TRUE(findTargetEeprom(0x45, dir).empty());
    EXPECT_TRUE(findTargetEeprom(0x4f, dir).empty());
    auto found = makeFile(dir / "3-1045" / "slave-eeprom", 1);
    EXPECT_EQ(findTargetEeprom(0x45, dir), found);
    makeFile(dir / "5-1045" / "slave-eeprom", 1);
    EXPECT_TRUE(findTargetEeprom(0x45, dir).empty());
    EXPECT_TRUE(findTargetEeprom(0x45, dir / "none").empty());
}

TEST_F(TempDir, RunCommand)
{
    auto control = makeFile(dir / "control", 64);
    std::vector<uint8_t> request(1 + max_args);
    for (std::size_t i = 0; i < request.size(); ++i)
    {
        request[i] = static_cast<uint8_t>(i + 1);
    }
    ASSERT_TRUE(eepromWrite(control, opcode_offset, request));
    ASSERT_TRUE(eepromWrite(control, request_offset, std::vector<uint8_t>{1}));

    std::vector<uint8_t> seen;
    EXPECT_TRUE(runCommand(control, [&](std::span<const uint8_t> args) {
        seen.assign(args.begin(), args.end());
        EXPECT_EQ(eepromRead(control, request_offset, 2),
                  (std::vector<uint8_t>{0, 1}));
        return 7;
    }));
    EXPECT_EQ(seen, request);
    EXPECT_EQ(eepromRead(control, request_offset, 3),
              (std::vector<uint8_t>{0, 0, 7}));
}

TEST_F(TempDir, RunCommandShortControl)
{
    // A control EEPROM shorter than the arguments reads as zeros.
    auto control = makeFile(dir / "control", request_offset);
    std::vector<uint8_t> seen;
    EXPECT_TRUE(runCommand(control, [&](std::span<const uint8_t> args) {
        seen.assign(args.begin(), args.end());
        return 0;
    }));
    EXPECT_EQ(seen, std::vector<uint8_t>(1 + max_args, 0));
}

TEST_F(TempDir, RunCommandNoControl)
{
    bool called = false;
    EXPECT_FALSE(
        runCommand((dir / "none").string(), [&](std::span<const uint8_t>) {
            called = true;
            return 0;
        }));
    EXPECT_FALSE(called);
}

TEST_F(TempDir, StageBlock)
{
    const std::size_t offset = 0x10;
    auto data = makeFile(dir / "data", offset + 4 + max_block_size);
    const auto entry = dir / "entry";
    makeFile(entry / "dump.tar.xz", max_block_size + 100, 0x5a);
    makeFile(entry / ".hidden", 1);

    EXPECT_EQ(stageBlock(entry, 0, data, offset), 0);
    EXPECT_EQ(eepromRead(data, offset, 4), beBytes(max_block_size, 4));
    EXPECT_EQ(eepromRead(data, offset + 4, max_block_size),
              std::vector<uint8_t>(max_block_size, 0x5a));

    EXPECT_EQ(stageBlock(entry, 1, data, offset), 0);
    EXPECT_EQ(eepromRead(data, offset, 4), beBytes(100, 4));

    EXPECT_EQ(stageBlock(entry, 2, data, offset), 1);
    EXPECT_EQ(stageBlock(entry, 0, (dir / "none").string(), offset), 1);
}

TEST_F(TempDir, StageBlockNeedsOneDump)
{
    auto data = makeFile(dir / "data", 16);
    EXPECT_EQ(stageBlock(dir / "missing", 0, data, 0), 1);
    const auto entry = dir / "entry";
    fs::create_directories(entry / "sub");
    EXPECT_EQ(stageBlock(entry, 0, data, 0), 1);
    makeFile(entry / "a", 1);
    makeFile(entry / "b", 1);
    EXPECT_EQ(stageBlock(entry, 0, data, 0), 1);
}

} // namespace
