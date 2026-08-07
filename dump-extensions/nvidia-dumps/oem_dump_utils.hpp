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
#pragma once
#include "config.h"

#include "com/nvidia/Dump/AllowableValues/server.hpp"
#include "dump_utils.hpp"

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

constexpr auto DUMP_OEM_ALLOWABLE_VALUES_PATH =
    "/xyz/openbmc_project/dump/oem_allowable_values";

constexpr auto DEBUG_INFO_INTERFACE = "com.nvidia.Dump.DebugInfo";

namespace phosphor
{
namespace dump
{

std::vector<std::string> splitString(const std::string& str, char delimiter);

/** @brief Extract string or decimal from a CreateDump variant value. */
std::optional<std::string> variantAsString(
    const phosphor::dump::DumpCreateParams::mapped_type& v);

/** @brief Read a CreateDump param by exact key; empty when absent or unset. */
std::string lookupCreateParam(const phosphor::dump::DumpCreateParams& params,
                              std::string_view key);

#ifdef VHMC_HOST
/** @brief Allowable OEMDiagnosticDataType values for the BMC (Manager) dump,
 *  parsed from BMC_DUMP_OEM_DIAGNOSTIC_ALLOWABLE_TYPE.
 */
const std::vector<std::string>& bmcOemAllowableValues();
#endif

using AllowableValuesIface = sdbusplus::server::object::object<
    sdbusplus::com::nvidia::Dump::server::AllowableValues>;
using DumpType = AllowableValuesIface::DumpType;

using OEMDataTypeAllowableValuesObject =
    sdbusplus::server::object::object<AllowableValuesIface>;

// <SupportedDumpType, DiagnosticType>
const std::unordered_map<std::string, std::string> debugInfoDumpTypeMapping{
    {"com.nvidia.Dump.DebugInfo.DumpType.Network", "NetIR"},
    {"com.nvidia.Dump.DebugInfo.DumpType.Diagnostics", "GPUDeviceDiagnostics"}};

class AsioConnection
{
  public:
    AsioConnection() = delete;
    AsioConnection(const AsioConnection&) = delete;
    AsioConnection& operator=(const AsioConnection&) = delete;
    AsioConnection(AsioConnection&&) = delete;
    AsioConnection& operator=(AsioConnection&&) = delete;
    ~AsioConnection() = delete;

    /** @brief Get the asio connection. */
    static auto& getAsioConnection()
    {
        static boost::asio::io_context io;
        static auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        return conn;
    }
};

class OEMTypeAllowableValuesIf : public OEMDataTypeAllowableValuesObject
{
  public:
    OEMTypeAllowableValuesIf() = delete;
    OEMTypeAllowableValuesIf(const OEMTypeAllowableValuesIf&) = delete;
    OEMTypeAllowableValuesIf& operator=(const OEMTypeAllowableValuesIf&) =
        delete;
    OEMTypeAllowableValuesIf(OEMTypeAllowableValuesIf&&) = delete;
    OEMTypeAllowableValuesIf& operator=(OEMTypeAllowableValuesIf&&) = delete;
    virtual ~OEMTypeAllowableValuesIf() = default;

    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] path - Path to attach at.
     */
    OEMTypeAllowableValuesIf(const char* path) :
        OEMDataTypeAllowableValuesObject(*AsioConnection::getAsioConnection(),
                                         path)
    {
        populateSystemOEMDataTypeAllowableValues(*this);

#ifdef VHMC_HOST
        populateManagerOEMDataTypeAllowableValues(*this);
#endif

#ifdef FDR_DUMP_EXTENSION
        populateFDROEMDataTypeAllowableValues(*this);
#endif

        auto& conn = AsioConnection::getAsioConnection();
        debugInfoMatch = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*conn),
            "type='signal',member='PropertiesChanged',"
            "arg0='" +
                std::string(DEBUG_INFO_INTERFACE) + "'",
            [this](sdbusplus::message::message& msg) {
                std::string interface;
                std::map<std::string, std::variant<std::string>> properties;
                msg.read(interface, properties);

                auto supportedTypeIt = properties.find("SupportedDumpType");
                if (supportedTypeIt != properties.end())
                {
                    const std::string* dumpType =
                        std::get_if<std::string>(&supportedTypeIt->second);
                    if (dumpType)
                    {
                        if (auto it = debugInfoDumpTypeMapping.find(*dumpType);
                            it != debugInfoDumpTypeMapping.end())
                        {
                            try
                            {
                                auto path = msg.get_path();
                                std::string dumpDebugInfoName =
                                    sdbusplus::message::object_path(path)
                                        .filename();
                                std::string diagTypeStr =
                                    "DiagnosticType=" + it->second +
                                    ";DeviceType=" + dumpDebugInfoName;
                                std::map<DumpType, std::vector<std::string>>
                                    oemAllowableValuesMap =
                                        this->oemDataTypeAllowableValues();
                                auto& systemValues =
                                    oemAllowableValuesMap[DumpType::System];
                                if (std::ranges::find(systemValues,
                                                      diagTypeStr) ==
                                    systemValues.end())
                                {
                                    systemValues.emplace_back(diagTypeStr);
                                    std::sort(systemValues.begin(),
                                              systemValues.end());
                                    this->oemDataTypeAllowableValues(
                                        oemAllowableValuesMap);
                                }
                            }
                            catch (const sdbusplus::exception_t& e)
                            {
                                lg2::error(
                                    "Failed to update OEM allowable values for System {TYPE} Dump: "
                                    "ERROR={ERROR}",
                                    "TYPE", it->second, "ERROR", e.what());
                            }
                        }
                    }
                }
            });
    }

    /** @brief Populate OEM allowable values for System dump */
    void populateSystemOEMDataTypeAllowableValues(
        sdbusplus::server::com::nvidia::dump::AllowableValues& iface);

#ifdef VHMC_HOST
    /** @brief Populate OEM allowable values for BMC (Manager) dump */
    void populateManagerOEMDataTypeAllowableValues(
        sdbusplus::server::com::nvidia::dump::AllowableValues& iface);
#endif

#ifdef FDR_DUMP_EXTENSION
    /** @brief Populate OEM allowable values for FDR dump */
    void populateFDROEMDataTypeAllowableValues(
        sdbusplus::server::com::nvidia::dump::AllowableValues& iface);
#endif

  private:
    /** @brief Collect and populate DebugInfo device types for system dump */
    void populateDebugInfoDumpTypes(
        sdbusplus::server::com::nvidia::dump::AllowableValues& iface);

    /** @brief D-Bus match for monitoring DebugInfo interface property changes
     */
    std::unique_ptr<sdbusplus::bus::match::match> debugInfoMatch;
};

} // namespace dump
} // namespace phosphor
