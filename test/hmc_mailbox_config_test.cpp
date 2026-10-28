// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "i2c-dump-tools/hmc_mailbox_config.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace hm = phosphor::dump::hmc_mailbox;
namespace fs = std::filesystem;

class HmcMailboxDetect : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dir = fs::temp_directory_path() /
              ("hmc_mailbox_test_" +
               std::string(::testing::UnitTest::GetInstance()
                               ->current_test_info()
                               ->name()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override
    {
        fs::remove_all(dir);
    }
    void eeprom(const std::string& name)
    {
        fs::create_directories(dir / "devices" / name);
        std::ofstream(dir / "devices" / name / "slave-eeprom") << "";
    }
    void write(const fs::path& p, const std::string& text)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::path dir;
};

TEST(HmcMailboxConfig, MissingFileKeepsDefaults)
{
    std::istringstream in;
    auto c = hm::parseConfig(in);
    EXPECT_FALSE(c.telemetryBus);
    EXPECT_FALSE(c.controlBus);
    EXPECT_EQ(c.telemetryAddress, 0x4f);
    EXPECT_EQ(c.controlAddress, 0x45);
    EXPECT_TRUE(c.rwfsRecovery.empty());
    EXPECT_FALSE(c.fixedMctpStatus);
    EXPECT_FALSE(c.forceReboot);
    EXPECT_TRUE(c.signatureCheck);
    EXPECT_EQ(c.softRebootReason,
              "Reboot triggered by external command over I2C");
    EXPECT_EQ(c.startupDelay, 0U);
    EXPECT_EQ(c.bmcSyslogAddress, "172.31.13.241");
}

TEST(HmcMailboxConfig, AllKeys)
{
    std::istringstream in(
        "# comment\n"
        "\n"
        "TelemetryBus=0\n"
        "ControlBus = 1\n"
        "TelemetryAddress=0x54\n"
        "ControlAddress=84\n"
        "RwfsRecovery=Initramfs\n"
        "FixedMctpStatus=true\n"
        "ForceReboot=1\n"
        "SignatureCheck=false\n"
        "SoftRebootReason=Graceful Restart triggered by external command "
        "over I2C\n"
        "StartupDelay=5\n"
        "BmcSyslogAddress=192.168.31.2\n");
    auto c = hm::parseConfig(in);
    EXPECT_EQ(c.telemetryBus, 0U);
    EXPECT_EQ(c.controlBus, 1U);
    EXPECT_EQ(c.telemetryAddress, 0x54);
    EXPECT_EQ(c.controlAddress, 0x54);
    EXPECT_EQ(c.rwfsRecovery, "Initramfs");
    EXPECT_TRUE(c.fixedMctpStatus);
    EXPECT_TRUE(c.forceReboot);
    EXPECT_FALSE(c.signatureCheck);
    EXPECT_EQ(c.softRebootReason,
              "Graceful Restart triggered by external command over I2C");
    EXPECT_EQ(c.startupDelay, 5U);
    EXPECT_EQ(c.bmcSyslogAddress, "192.168.31.2");
}

TEST(HmcMailboxConfig, BadLinesIgnored)
{
    std::istringstream in(
        "TelemetryAddress=0x80\n"
        "RwfsRecovery=Format\n"
        "ForceReboot=yes\n"
        "StartupDelay=-1\n"
        "Unknown=1\n"
        "no equals sign\n"
        "ControlBus=x9\n"
        "SoftRebootReason=\n");
    auto c = hm::parseConfig(in);
    EXPECT_EQ(c.telemetryAddress, 0x4f);
    EXPECT_TRUE(c.rwfsRecovery.empty());
    EXPECT_FALSE(c.forceReboot);
    EXPECT_EQ(c.startupDelay, 0U);
    EXPECT_FALSE(c.controlBus);
    EXPECT_EQ(c.softRebootReason,
              "Reboot triggered by external command over I2C");
}

TEST_F(HmcMailboxDetect, EepromBusFound)
{
    eeprom("3-104f");
    eeprom("0-1045");
    fs::create_directories(dir / "devices" / "5-104f"); // no slave-eeprom
    fs::create_directories(dir / "devices" / "i2c-3");
    EXPECT_EQ(hm::findEepromBus(dir / "devices", 0x4f), 3U);
    EXPECT_EQ(hm::findEepromBus(dir / "devices", 0x45), 0U);
}

TEST_F(HmcMailboxDetect, EepromBusMissingOrAmbiguous)
{
    eeprom("0-1054");
    eeprom("1-1054");
    eeprom("6-1054");
    EXPECT_FALSE(hm::findEepromBus(dir / "devices", 0x4f));
    EXPECT_FALSE(hm::findEepromBus(dir / "devices", 0x54));
    EXPECT_FALSE(hm::findEepromBus(dir / "missing", 0x54));
}

TEST_F(HmcMailboxDetect, RwfsOnMtd)
{
    write(dir / "mounts", "/dev/root / ext4 rw 0 0\n"
                          "/dev/mtdblock6 /run/initramfs/rw jffs2 rw 0 0\n");
    EXPECT_EQ(hm::detectRwfsRecovery(dir / "mounts", dir / "mtd",
                                     "/run/initramfs/rw"),
              "MtdErase");
}

TEST_F(HmcMailboxDetect, RwfsOnEmmc)
{
    write(dir / "mounts", "/dev/mmcblk0p2 /run/initramfs/rw ext4 rw 0 0\n");
    write(dir / "mtd" / "mtd6" / "name", "rwfs\n");
    EXPECT_EQ(hm::detectRwfsRecovery(dir / "mounts", dir / "mtd",
                                     "/run/initramfs/rw"),
              "None");
}

TEST_F(HmcMailboxDetect, RwfsNotMounted)
{
    write(dir / "mounts", "/dev/root / ext4 rw 0 0\n");
    EXPECT_EQ(hm::detectRwfsRecovery(dir / "mounts", dir / "mtd",
                                     "/run/initramfs/rw"),
              "None");
    write(dir / "mtd" / "mtd6" / "name", "rwfs\n");
    EXPECT_EQ(hm::detectRwfsRecovery(dir / "mounts", dir / "mtd",
                                     "/run/initramfs/rw"),
              "MtdErase");
}

TEST(HmcMailboxUsb0, FirstIpv4)
{
    using A = std::array<uint8_t, 4>;
    EXPECT_EQ(hm::firstIpv4({}), (A{0, 0, 0, 0}));
    EXPECT_EQ(hm::firstIpv4({"169.254.3.4", "172.31.13.251"}),
              (A{172, 31, 13, 251}));
    EXPECT_EQ(hm::firstIpv4({"169.254.9.9", "169.254.1.1"}),
              (A{169, 254, 1, 1}));
    EXPECT_EQ(hm::firstIpv4({"10.1.2.3", "9.1.2.3"}), (A{9, 1, 2, 3}));
    EXPECT_EQ(hm::firstIpv4({"bad", "1.2.3", "1.2.3.256", "1.2.3.4.5"}),
              (A{0, 0, 0, 0}));
}

// The meta layers' files against what hmc-mailboxd's Platform table had for
// those HMCs; the buses of the VR HMCs are detected.
TEST(HmcMailboxConfig, VrNvlHmcAst2700File)
{
    std::istringstream in("RwfsRecovery=Initramfs\n"
                          "StartupDelay=5\n");
    auto c = hm::parseConfig(in);
    EXPECT_FALSE(c.telemetryBus);
    EXPECT_FALSE(c.controlBus);
    EXPECT_EQ(c.telemetryAddress, 0x4f);
    EXPECT_EQ(c.controlAddress, 0x45);
    EXPECT_EQ(c.rwfsRecovery, "Initramfs");
    EXPECT_FALSE(c.fixedMctpStatus);
    EXPECT_FALSE(c.forceReboot);
    EXPECT_TRUE(c.signatureCheck);
    EXPECT_EQ(c.softRebootReason,
              "Reboot triggered by external command over I2C");
    EXPECT_EQ(c.startupDelay, 5U);
    EXPECT_EQ(c.bmcSyslogAddress, "172.31.13.241");
}

TEST(HmcMailboxConfig, VruNvlHmcFile)
{
    std::istringstream in("RwfsRecovery=None\n");
    auto c = hm::parseConfig(in);
    EXPECT_FALSE(c.telemetryBus);
    EXPECT_FALSE(c.controlBus);
    EXPECT_EQ(c.telemetryAddress, 0x4f);
    EXPECT_EQ(c.controlAddress, 0x45);
    EXPECT_EQ(c.rwfsRecovery, "None");
    EXPECT_FALSE(c.fixedMctpStatus);
    EXPECT_FALSE(c.forceReboot);
    EXPECT_TRUE(c.signatureCheck);
    EXPECT_EQ(c.softRebootReason,
              "Reboot triggered by external command over I2C");
    EXPECT_EQ(c.startupDelay, 0U);
    EXPECT_EQ(c.bmcSyslogAddress, "172.31.13.241");
}

TEST(HmcMailboxConfig, HgxrHmcFile)
{
    std::istringstream in(
        "TelemetryBus=0\n"
        "ControlBus=1\n"
        "TelemetryAddress=0x54\n"
        "ControlAddress=0x54\n"
        "FixedMctpStatus=true\n"
        "ForceReboot=true\n"
        "SignatureCheck=false\n"
        "SoftRebootReason=Graceful Restart triggered by external command over "
        "I2C\n"
        "StartupDelay=5\n"
        "BmcSyslogAddress=192.168.31.2\n");
    auto c = hm::parseConfig(in);
    EXPECT_EQ(c.telemetryBus, 0U);
    EXPECT_EQ(c.controlBus, 1U);
    EXPECT_EQ(c.telemetryAddress, 0x54);
    EXPECT_EQ(c.controlAddress, 0x54);
    // rwfs on MTD: detected as MtdErase.
    EXPECT_TRUE(c.rwfsRecovery.empty());
    EXPECT_TRUE(c.fixedMctpStatus);
    EXPECT_TRUE(c.forceReboot);
    EXPECT_FALSE(c.signatureCheck);
    EXPECT_EQ(c.softRebootReason,
              "Graceful Restart triggered by external command over I2C");
    EXPECT_EQ(c.startupDelay, 5U);
    EXPECT_EQ(c.bmcSyslogAddress, "192.168.31.2");
}
