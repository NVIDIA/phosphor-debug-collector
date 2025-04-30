/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "oem_dump_utils.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace phosphor
{
namespace dump
{

using MapperGetSubTreeResponse =
    std::map<std::string, std::map<std::string, std::vector<std::string>>>;

std::vector<std::string> splitString(const std::string& str, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);

    while (std::getline(tokenStream, token, delimiter))
    {
        // Remove leading and trailing whitespace
        token.erase(0, token.find_first_not_of(" "));
        token.erase(token.find_last_not_of(" ") + 1);
        if (!token.empty())
        {
            tokens.push_back(token);
        }
    }
    return tokens;
}

void OEMTypeAllowableValuesIf::populateDebugInfoDumpTypes(
    sdbusplus::server::com::nvidia::dump::AllowableValues& iface)
{
    auto& conn = AsioConnection::getAsioConnection();
    if (!conn)
    {
        lg2::error("Failed to get D-Bus connection");
        return;
    }

    conn->async_method_call(
        [conn, &iface](const boost::system::error_code& ec,
                       const MapperGetSubTreeResponse& mapperResponse) {
        if (ec)
        {
            lg2::error(
                "Failed to get subtree from '{IFACE}' interface: ERROR={ERROR}",
                "IFACE", DEBUG_INFO_INTERFACE, "ERROR", ec.message());
            return;
        }

        for (const auto& [path, serviceMap] : mapperResponse)
        {
            if (serviceMap.empty())
            {
                continue;
            }

            for (const auto& [service, interfaces] : serviceMap)
            {
                conn->async_method_call(
                    [service, path,
                     &iface](const boost::system::error_code& ec2,
                             const std::variant<std::string>& value) {
                    if (ec2)
                    {
                        lg2::error(
                            "Failed to get 'SupportedDumpType' property in '{IFACE}' interface: "
                            "PATH={PATH}; SERVICE={SERVICE}; ERROR={ERROR}",
                            "IFACE", DEBUG_INFO_INTERFACE, "PATH", path.c_str(),
                            "SERVICE", service.c_str(), "ERROR", ec2.message());
                        return;
                    }

                    if (std::holds_alternative<std::string>(value))
                    {
                        std::string dumpType = std::get<std::string>(value);
                        sdbusplus::message::object_path dumpDebugInfoId(path);
                        std::string dumpDebugInfoName =
                            dumpDebugInfoId.filename();

                        if (auto it = debugInfoDumpTypeMapping.find(dumpType);
                            it != debugInfoDumpTypeMapping.end())
                        {
                            try
                            {
                                // Get the DiagnosticType name from the
                                // mapping's value (it->second)
                                std::string diagTypeStr =
                                    "DiagnosticType=" + it->second +
                                    ";DeviceType=" + dumpDebugInfoName;

                                std::map<DumpType, std::vector<std::string>>
                                    oemAllowableValuesMap =
                                        iface.oemDataTypeAllowableValues();
                                auto& systemValues =
                                    oemAllowableValuesMap[DumpType::System];
                                if (std::ranges::find(systemValues,
                                                      diagTypeStr) ==
                                    systemValues.end())
                                {
                                    systemValues.emplace_back(diagTypeStr);
                                    std::sort(systemValues.begin(),
                                              systemValues.end());
                                    iface.oemDataTypeAllowableValues(
                                        oemAllowableValuesMap);
                                }
                            }
                            catch (const sdbusplus::exception_t& e)
                            {
                                lg2::error(
                                    "Failed to set OEM allowable values for System {TYPE} Dump: "
                                    "ERROR={ERROR}",
                                    "TYPE", it->second, "ERROR", e.what());
                            }
                        }
                    }
                    else
                    {
                        lg2::error(
                            "Invalid type for 'SupportedDumpType' property in '{IFACE}' interface: "
                            "PATH={PATH}; SERVICE={SERVICE}",
                            "IFACE", DEBUG_INFO_INTERFACE, "PATH", path.c_str(),
                            "SERVICE", service.c_str());
                    }
                },
                    service.c_str(), path.c_str(),
                    "org.freedesktop.DBus.Properties", "Get",
                    DEBUG_INFO_INTERFACE, "SupportedDumpType");
            }
        }
    },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", 0,
        std::array<const char*, 1>{DEBUG_INFO_INTERFACE});
}

void OEMTypeAllowableValuesIf::populateSystemOEMDataTypeAllowableValues(
    sdbusplus::server::com::nvidia::dump::AllowableValues& iface)
{
    // Get the OEM DiagnosticType from meson option
    std::vector<std::string> typeStrings =
        splitString(SYSTEM_DUMP_OEM_DIAGNOSTIC_ALLOWABLE_TYPE, ',');

    bool hasDebugInfoDumpType = false;
    for (const auto& typeStr : typeStrings)
    {
        if (typeStr.empty())
        {
            continue;
        }

        if (!std::ranges::any_of(debugInfoDumpTypeMapping,
                                 [&typeStr](const auto& pair) {
            return pair.second == typeStr;
        }))
        {
            try
            {
                std::string diagTypeStr = "DiagnosticType=" + typeStr;

                auto oemAllowableValuesMap = iface.oemDataTypeAllowableValues();
                auto& systemValues = oemAllowableValuesMap[DumpType::System];
                if (std::ranges::find(systemValues, diagTypeStr) ==
                    systemValues.end())
                {
                    systemValues.emplace_back(diagTypeStr);
                    std::sort(systemValues.begin(), systemValues.end());
                    iface.oemDataTypeAllowableValues(oemAllowableValuesMap);
                }
            }
            catch (const sdbusplus::exception_t& e)
            {
                lg2::error(
                    "Failed to set OEM allowable values for System {TYPE} Dump: "
                    "ERROR={ERROR}",
                    "TYPE", typeStr, "ERROR", e.what());
            }
        }
        else
        {
            hasDebugInfoDumpType = true;
        }
    }

    if (hasDebugInfoDumpType == true)
    {
        populateDebugInfoDumpTypes(iface);
    }
}

#ifdef FDR_DUMP_EXTENSION
void OEMTypeAllowableValuesIf::populateFDROEMDataTypeAllowableValues(
    sdbusplus::server::com::nvidia::dump::AllowableValues& iface)
{
    std::vector<std::string> oemAllowableValues;

    // FDR dump format is fixed
    oemAllowableValues.emplace_back(
        "DiagnosticType=FDR;TimeRangeStart=<yyyy>-<MM>-<dd> <HH>:<mm>:<ss>;TimeRangeEnd=<yyyy>-<MM>-<dd> <HH>:<mm>:<ss>;ExtendedSource=<source-info>");

    try
    {
        std::map<DumpType, std::vector<std::string>> oemAllowableValuesMap =
            iface.oemDataTypeAllowableValues();
        oemAllowableValuesMap[DumpType::FDR] = oemAllowableValues;
        iface.oemDataTypeAllowableValues(oemAllowableValuesMap);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to set OEM allowable values for FDR Dump: "
                   "ERROR={ERROR}",
                   "ERROR", e.what());
    }
}
#endif

} // namespace dump
} // namespace phosphor
