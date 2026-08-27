// SPDX-License-Identifier: Apache-2.0
#include <ramoops_manager.hpp>
#include <xyz/openbmc_project/Dump/Create/common.hpp>

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

using phosphor::dump::ramoops::Manager;

namespace
{

using CreateParameters =
    sdbusplus::common::xyz::openbmc_project::dump::Create::CreateParameters;
using DumpType =
    sdbusplus::common::xyz::openbmc_project::dump::Create::DumpType;
using DumpIntr = sdbusplus::common::xyz::openbmc_project::dump::Create;

} // namespace

// NOLINTBEGIN(readability-identifier-naming)
TEST(RamoopsCreateDumpParams, SetsRamoopsDumpType)
{
    auto params = Manager::createDumpParams({"/var/lib/systemd/pstore"});

    auto it = params.find(
        DumpIntr::convertCreateParametersToString(CreateParameters::DumpType));
    ASSERT_NE(it, params.end());
    EXPECT_EQ(std::get<std::string>(it->second),
              DumpIntr::convertDumpTypeToString(DumpType::Ramoops));
}

TEST(RamoopsCreateDumpParams, SetsFilePathToFirstFile)
{
    auto params = Manager::createDumpParams(
        {"/var/lib/systemd/pstore", "/var/lib/systemd/pstore/unused"});

    auto it = params.find(
        DumpIntr::convertCreateParametersToString(CreateParameters::FilePath));
    ASSERT_NE(it, params.end());
    EXPECT_EQ(std::get<std::string>(it->second), "/var/lib/systemd/pstore");
}
// NOLINTEND(readability-identifier-naming)
