// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include "dump-extensions/nvidia-dumps/oem_dump_utils.hpp"

#include <xyz/openbmc_project/Common/error.hpp>

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace phosphor::dump
{
namespace
{

TEST(LookupCreateParam, ReadsStringAndNumericValues)
{
    DumpCreateParams params;
    params["Duration"] = std::string("300");
    params["SamplingRate"] = uint64_t{5};

    EXPECT_EQ(lookupCreateParam(params, "Duration"), "300");
    EXPECT_EQ(lookupCreateParam(params, "SamplingRate"), "5");
}

TEST(LookupCreateParam, MissingOrEmptyIsEmpty)
{
    DumpCreateParams params;
    params["DiagnosticType"] = std::string();

    EXPECT_EQ(lookupCreateParam(params, "DiagnosticType"), "");
    EXPECT_EQ(lookupCreateParam(params, "Duration"), "");
}

#ifdef VHMC_HOST
// BMC_DUMP_OEM_DIAGNOSTIC_ALLOWABLE_TYPE is the only source of what the BMC
// (Manager) dump accepts; every configured value must survive the split
// intact, with no empty or padded entries. Assumes the option is given without
// padding around the commas, as openbmc passes it.
TEST(BmcOemAllowableValues, SplitsConfiguredValues)
{
    const std::string configured = BMC_DUMP_OEM_DIAGNOSTIC_ALLOWABLE_TYPE;
    const auto& values = bmcOemAllowableValues();

    std::string joined;
    for (const auto& value : values)
    {
        ASSERT_FALSE(value.empty());
        EXPECT_NE(value.front(), ' ');
        EXPECT_NE(value.back(), ' ');
        EXPECT_TRUE(value.starts_with("DiagnosticType="));
        if (!joined.empty())
        {
            joined += ',';
        }
        joined += value;
    }
    EXPECT_EQ(joined, configured);
}

using InvalidArgument =
    sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;

TEST(BmcOemDiagnosticType, AbsentIsStandardDump)
{
    DumpCreateParams params;
    params["xyz.openbmc_project.Dump.Create.CreateParameters.OriginatorId"] =
        std::string("192.0.2.1");
    EXPECT_EQ(bmcOemDiagnosticType(params), "");
}

TEST(BmcOemDiagnosticType, ReturnsTheType)
{
    DumpCreateParams params;
    params["DiagnosticType"] = std::string("ContainerRuntimeMetrics");
    EXPECT_EQ(bmcOemDiagnosticType(params), "ContainerRuntimeMetrics");
}

// Redfish "DiagnosticType=" must be rejected, not run as a standard dump.
TEST(BmcOemDiagnosticType, PresentButEmptyIsInvalidArgument)
{
    DumpCreateParams params;
    params["DiagnosticType"] = std::string();
    params["Duration"] = std::string("300");
    params["SamplingRate"] = std::string("1");
    EXPECT_THROW(bmcOemDiagnosticType(params), InvalidArgument);
}

DumpCreateParams oemDumpParams()
{
    DumpCreateParams params;
    params["DiagnosticType"] = std::string("ContainerRuntimeMetrics");
    params["Duration"] = std::string("300");
    params["SamplingRate"] = uint64_t{1};
    params["xyz.openbmc_project.Dump.Create.CreateParameters.OriginatorId"] =
        std::string("192.0.2.1");
    params["xyz.openbmc_project.Dump.Create.CreateParameters.OriginatorType"] =
        std::string(
            "xyz.openbmc_project.Common.OriginatedBy.OriginatorTypes.Client");
    return params;
}

TEST(BmcOemDumpParamKeys, OemAndOriginatorKeysPass)
{
    EXPECT_NO_THROW(checkBmcOemDumpParamKeys(oemDumpParams()));
}

// Unused keys are refused, and so are the ones a standard dump acts on.
TEST(BmcOemDumpParamKeys, AnyOtherKeyIsInvalidArgument)
{
    for (const char* key :
         {"Garbage", "diagnostictype", "",
          "xyz.openbmc_project.Dump.Create.CreateParameters.DumpType",
          "xyz.openbmc_project.Dump.Create.CreateParameters.FilePath",
          "xyz.openbmc_project.Dump.Create.CreateParameters.ErrorType"})
    {
        DumpCreateParams params = oemDumpParams();
        params[key] = std::string("x");
        EXPECT_THROW(checkBmcOemDumpParamKeys(params), InvalidArgument) << key;
    }
}
#endif

} // namespace
} // namespace phosphor::dump
