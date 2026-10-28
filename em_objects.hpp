// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// Reads entity-manager configuration records over D-Bus. Shared by the
// NVIDIA dump collectors and the I2C dump tools.

#pragma once

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace em
{

/** Property variant emitted by EM records. uint64_t is required because EM
 *  publishes JSON integers as D-Bus `t` by default. */
using EmPropVariant =
    std::variant<std::string, int64_t, int32_t, uint64_t, uint32_t, uint16_t,
                 uint8_t, double, bool, std::vector<std::string>,
                 std::vector<uint8_t>>;

/** One EM inventory object plus its property map for the matched interface.
 *  Returned by enumerateEmObjects(). */
struct EmObject
{
    std::string path;    // object path under /xyz/openbmc_project/inventory
    std::string service; // owning bus name (typically entity-manager)
    std::string interfaceMatched; // which requested interface this carried
    std::map<std::string, EmPropVariant>
        properties;               // GetAll result for that interface
};

/** Generic EM record reader: GetSubTree over the inventory for `interfaces`,
 *  then GetAll on the matched Configuration interface per path. Failures are
 *  logged at warn level and yield a partial result rather than throwing. */
inline std::vector<EmObject> enumerateEmObjects(
    const std::vector<std::string>& interfaces)
{
    std::vector<EmObject> out;

    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);

    using GetSubTreeResp =
        std::map<std::string, std::map<std::string, std::vector<std::string>>>;

    GetSubTreeResp subtree;
    try
    {
        auto call = conn->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        call.append(std::string("/xyz/openbmc_project/inventory"));
        call.append(int32_t{0});
        call.append(interfaces);
        auto reply = conn->call(call);
        reply.read(subtree);
    }
    catch (const std::exception& e)
    {
        lg2::warning("collector: EM enumeration failed (GetSubTree): {ERR}",
                     "ERR", e.what());
        return out;
    }

    for (const auto& [path, services] : subtree)
    {
        if (services.empty())
        {
            continue;
        }
        const auto& service = services.begin()->first;
        const auto& ifaces = services.begin()->second;
        if (ifaces.empty())
        {
            continue;
        }
        // Pick the first interface in the requested set that this path carries.
        std::string ifaceToQuery;
        for (const auto& want : interfaces)
        {
            if (std::find(ifaces.begin(), ifaces.end(), want) != ifaces.end())
            {
                ifaceToQuery = want;
                break;
            }
        }
        if (ifaceToQuery.empty())
        {
            continue;
        }

        std::map<std::string, EmPropVariant> props;
        try
        {
            auto getAll = conn->new_method_call(
                service.c_str(), path.c_str(),
                "org.freedesktop.DBus.Properties", "GetAll");
            getAll.append(ifaceToQuery);
            auto reply = conn->call(getAll);
            reply.read(props);
        }
        catch (const std::exception& e)
        {
            lg2::warning(
                "collector: GetAll on '{PATH}' interface '{IFACE}' failed: "
                "{ERR}",
                "PATH", path, "IFACE", ifaceToQuery, "ERR", e.what());
            continue;
        }

        out.push_back({path, service, ifaceToQuery, std::move(props)});
    }
    return out;
}

/** Extract a string property from an EmObject; empty string if absent or
 *  non-string. */
inline std::string emString(const EmObject& obj, const std::string& key)
{
    auto it = obj.properties.find(key);
    if (it == obj.properties.end())
    {
        return {};
    }
    if (auto p = std::get_if<std::string>(&it->second))
    {
        return *p;
    }
    return {};
}

/** Extract a uint8_t property from an EmObject, narrowing from EM's int types.
 *  Returns 0 when absent or non-integer. */
inline uint8_t emUint8(const EmObject& obj, const std::string& key)
{
    auto it = obj.properties.find(key);
    if (it == obj.properties.end())
    {
        return 0;
    }
    if (auto p = std::get_if<uint8_t>(&it->second))
    {
        return *p;
    }
    // EM publishes JSON integers as D-Bus `t` (uint64) by default.
    if (auto p = std::get_if<uint64_t>(&it->second))
    {
        return static_cast<uint8_t>(*p);
    }
    if (auto p = std::get_if<int64_t>(&it->second))
    {
        return static_cast<uint8_t>(*p);
    }
    return 0;
}

/** Extract a vector<string> property from an EmObject; empty vector if absent
 *  or non-array. */
inline std::vector<std::string> emStringVec(const EmObject& obj,
                                            const std::string& key)
{
    auto it = obj.properties.find(key);
    if (it == obj.properties.end())
    {
        return {};
    }
    if (auto p = std::get_if<std::vector<std::string>>(&it->second))
    {
        return *p;
    }
    return {};
}

} // namespace em
} // namespace dump
} // namespace phosphor
