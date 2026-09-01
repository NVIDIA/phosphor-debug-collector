// SPDX-License-Identifier: Apache-2.0

#include "nsm_device_utils.hpp"

#include <gtest/gtest.h>

namespace phosphor::dump::nsm
{
namespace
{

TEST(NsmDeviceUtils, PreservesLeafContainingChassisName)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_3/"
                  "NetworkAdapters/CX_NIC_3"),
              "CX_NIC_3");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, UsesChassisIndexForRepeatedQualifiedLeaf)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/BlueField_0/"
                  "NetworkAdapters/BlueField_NIC_0"),
              "BlueField_NIC_0");
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/BlueField_1/"
                  "NetworkAdapters/BlueField_NIC_0"),
              "BlueField_NIC_1");
}

TEST(NsmDeviceUtils, UsesChassisIndexForGenericLeaf)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_0"),
              "CX_NIC_7");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, DistinguishesMultipleAdaptersUnderOneChassis)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_0"),
              "CX_NIC_7");
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_1"),
              "CX_NIC_7_1");
}

TEST(NsmDeviceUtils, AddsMissingBoardNameWithoutDuplicatingCx)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/"
                  "Bay_C_CX_2/NetworkAdapters/CX_NIC_0"),
              "Bay_C_CX_NIC_2");
}

TEST(NsmDeviceUtils, MergesLongestMultiTokenOverlap)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/"
                  "Bay_C_CX_Switch_2/NetworkAdapters/CX_Switch_NIC_0"),
              "Bay_C_CX_Switch_NIC_2");
}

TEST(NsmDeviceUtils, PreservesBoardQualifiedLeaf)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/"
                  "Bay_C_CX_0/NetworkAdapters/Bay_C_CX_NIC_0"),
              "Bay_C_CX_NIC_0");
}

TEST(NsmDeviceUtils, UsesAdapterLeafForNestedObjects)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_2/"
                  "NetworkAdapters/NIC_0/Diagnostics"),
              "CX_NIC_2");
}

TEST(NsmDeviceUtils, PreservesNonNetworkAdapterLeaf)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/Diagnostics/"
                  "Dump/Bay_C_SMA_0"),
              "Bay_C_SMA_0");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, PreservesGpuDeviceDiagnosticsSelector)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/Diagnostics/"
                  "Dump/IO_Board_SMA_0"),
              "IO_Board_SMA_0");
}

TEST(NsmDeviceUtils, FallsBackToLeafForUnindexedChassis)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX/"
                  "NetworkAdapters/NIC_0"),
              "NIC_0");
}

TEST(NsmDeviceUtils, FallsBackToLeafForUnindexedAdapter)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_0/"
                  "NetworkAdapters/NIC"),
              "NIC");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, FallsBackToLeafForNonNumericChassisIndex)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_Switch/"
                  "NetworkAdapters/NIC_0"),
              "NIC_0");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, MatchesSelectorExactly)
{
    constexpr auto path = "/xyz/openbmc_project/inventory/system/chassis/CX_5/"
                          "NetworkAdapters/NIC_0";
    EXPECT_EQ(deviceSelectorMatch(path, "CX_NIC_5"),
              DeviceSelectorMatch::Exact);
    EXPECT_TRUE(pathMatchesDeviceSelector(path, "CX_NIC_5"));
    EXPECT_FALSE(pathMatchesDeviceSelector(path, "CX_NIC_0"));
    EXPECT_EQ(deviceSelectorMatch(path, "NIC_0"), DeviceSelectorMatch::Legacy);
    EXPECT_TRUE(pathMatchesDeviceSelector(path, "NIC_0"));
    EXPECT_EQ(deviceSelectorMatch(path, ""), DeviceSelectorMatch::None);
    EXPECT_EQ(deviceSelectorMatch("", ""), DeviceSelectorMatch::None);
    EXPECT_FALSE(pathMatchesDeviceSelector(path, ""));
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, MatchesLegacyPlatformPrefixedSelector)
{
    constexpr auto base = "/xyz/openbmc_project/inventory/system/chassis/";
    EXPECT_EQ(
        deviceSelectorMatch(std::string(base) + "HGX_GPU_SXM_1", "GPU_SXM_1"),
        DeviceSelectorMatch::Legacy);
    EXPECT_TRUE(pathMatchesDeviceSelector(std::string(base) + "HGX_GPU_SXM_1",
                                          "GPU_SXM_1"));
    EXPECT_TRUE(pathMatchesDeviceSelector(std::string(base) + "HGX_NVSwitch_0",
                                          "NVSwitch_0"));
    EXPECT_TRUE(pathMatchesDeviceSelector(
        std::string(base) + "HGX_NVLinkManagementNIC_0",
        "NVLinkManagementNIC_0"));
    EXPECT_EQ(
        deviceSelectorMatch(std::string(base) + "GPU_SXM_1/Dump", "GPU_SXM_1"),
        DeviceSelectorMatch::Legacy);
    EXPECT_TRUE(pathMatchesDeviceSelector(
        std::string(base) + "HGX_GPU_SXM_1/Dump", "GPU_SXM_1"));
    EXPECT_FALSE(pathMatchesDeviceSelector(std::string(base) + "HGX_GPU_SXM_10",
                                           "GPU_SXM_1"));
    EXPECT_FALSE(pathMatchesDeviceSelector(
        std::string(base) + "HGX_GPU_SXM_10/Dump", "GPU_SXM_1"));
}

} // namespace
} // namespace phosphor::dump::nsm
