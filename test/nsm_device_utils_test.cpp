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
                  "NetworkAdapters/CX_NIC_3",
                  true),
              "CX_NIC_3");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, UsesChassisIndexForRepeatedQualifiedLeaf)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/BlueField_0/"
                  "NetworkAdapters/BlueField_NIC_0",
                  true),
              "BlueField_NIC_0");
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/BlueField_1/"
                  "NetworkAdapters/BlueField_NIC_0",
                  true),
              "BlueField_NIC_1");
}

TEST(NsmDeviceUtils, UsesChassisIndexForGenericLeaf)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_0",
                  true),
              "CX_NIC_7");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, DistinguishesMultipleAdaptersUnderOneChassis)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_0",
                  true),
              "CX_NIC_7");
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_1",
                  true),
              "CX_NIC_7_1");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, AvoidsRepeatedMirroredAdapterIndex)
{
    for (int index = 0; index < 8; ++index)
    {
        SCOPED_TRACE(index);
        const auto instance = std::to_string(index);
        std::string path =
            "/xyz/openbmc_project/inventory/system/chassis/HGX_ConnectX_";
        path += instance;
        path += "/NetworkAdapters/ConnectX_NIC_";
        path += instance;
        EXPECT_EQ(deviceSelectorFromPath(path, true),
                  "HGX_ConnectX_NIC_" + instance);
    }
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, RetainsDistinctLeafIndex)
{
    EXPECT_EQ(
        deviceSelectorFromPath("/xyz/openbmc_project/inventory/system/chassis/"
                               "HGX_ConnectX_7/NetworkAdapters/ConnectX_NIC_1",
                               true),
        "HGX_ConnectX_NIC_7_1");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, PreservesNetworkAdapterLeafWhenDisambiguationIsDisabled)
{
    EXPECT_EQ(
        deviceSelectorFromPath("/xyz/openbmc_project/inventory/system/chassis/"
                               "HGX_ConnectX_7/NetworkAdapters/ConnectX_NIC_7",
                               false),
        "ConnectX_NIC_7");
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_7/"
                  "NetworkAdapters/NIC_0",
                  false),
              "NIC_0");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, AddsMissingBoardNameWithoutDuplicatingCx)
{
    EXPECT_EQ(
        deviceSelectorFromPath("/xyz/openbmc_project/inventory/system/chassis/"
                               "Bay_C_CX_2/NetworkAdapters/CX_NIC_0",
                               true),
        "Bay_C_CX_NIC_2");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, MergesLongestMultiTokenOverlap)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/"
                  "Bay_C_CX_Switch_2/NetworkAdapters/CX_Switch_NIC_0",
                  true),
              "Bay_C_CX_Switch_NIC_2");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, PreservesBoardQualifiedLeaf)
{
    EXPECT_EQ(
        deviceSelectorFromPath("/xyz/openbmc_project/inventory/system/chassis/"
                               "Bay_C_CX_0/NetworkAdapters/Bay_C_CX_NIC_0",
                               true),
        "Bay_C_CX_NIC_0");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, UsesAdapterLeafForNestedObjects)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_2/"
                  "NetworkAdapters/NIC_0/Diagnostics",
                  true),
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
                  "NetworkAdapters/NIC_0",
                  true),
              "NIC_0");
}

TEST(NsmDeviceUtils, FallsBackToLeafForUnindexedAdapter)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_0/"
                  "NetworkAdapters/NIC",
                  true),
              "NIC");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, FallsBackToLeafForNonNumericChassisIndex)
{
    EXPECT_EQ(deviceSelectorFromPath(
                  "/xyz/openbmc_project/inventory/system/chassis/CX_Switch/"
                  "NetworkAdapters/NIC_0",
                  true),
              "NIC_0");
}

// NOLINTNEXTLINE(readability-identifier-naming)
TEST(NsmDeviceUtils, MatchesSelectorExactly)
{
    constexpr auto path = "/xyz/openbmc_project/inventory/system/chassis/CX_5/"
                          "NetworkAdapters/NIC_0";
    EXPECT_EQ(deviceSelectorMatch(path, "CX_NIC_5", true),
              DeviceSelectorMatch::Exact);
    EXPECT_TRUE(pathMatchesDeviceSelector(path, "CX_NIC_5", true));
    EXPECT_FALSE(pathMatchesDeviceSelector(path, "CX_NIC_0", true));
    EXPECT_EQ(deviceSelectorMatch(path, "NIC_0", true),
              DeviceSelectorMatch::Legacy);
    EXPECT_TRUE(pathMatchesDeviceSelector(path, "NIC_0", true));
    EXPECT_EQ(deviceSelectorMatch(path, "", true), DeviceSelectorMatch::None);
    EXPECT_EQ(deviceSelectorMatch("", "", true), DeviceSelectorMatch::None);
    EXPECT_FALSE(pathMatchesDeviceSelector(path, "", true));
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
