/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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

#include "em_args.hpp"
#include "probe.hpp"

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace collectors
{

/** Parsed argv for every nv-collector-* binary: --out-dir, --entry-id,
 *  optional --em-config-path, plus any per-target --K=V / --K V flags. */
struct CollectorCli
{
    std::string outDir;
    std::string entryId;
    std::string emConfigPath; // EM D-Bus object path, optional
    std::unordered_map<std::string, std::string> extra;
};

inline CollectorCli parseCli(int argc, char** argv)
{
    CollectorCli c;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto take = [&](std::string& dst) {
            if (i + 1 < argc)
            {
                dst = argv[++i];
            }
        };
        if (a == "--out-dir" || a == "-p")
        {
            // -p alias: PDC passes "-p <dump path>" (legacy contract).
            take(c.outDir);
        }
        else if (a == "--entry-id" || a == "-i")
        {
            // -i alias: PDC passes "-i <dump id>" (legacy contract).
            take(c.entryId);
        }
        else if (a == "--em-config-path")
        {
            take(c.emConfigPath);
        }
        else if (a == "-d")
        {
            // -d alias: "-d <device-type>". Stored under both casings so
            // collector main()s can look up either.
            std::string val;
            take(val);
            c.extra["device-type"] = val;
            c.extra["DeviceType"] = val;
        }
        else if (a == "-t")
        {
            // -t alias: "-t <temp>". Stored for collectors that want it.
            std::string val;
            take(val);
            c.extra["temp-path"] = val;
        }
        else if (a == "-o")
        {
            // -o alias: "-o <OEM dump type>", accepted for legacy symmetry.
            std::string val;
            take(val);
            c.extra["oem-dump-type"] = val;
        }
        else if (a == "-e")
        {
            // -e alias: "-e <eid>" per-target EID; read as cli.extra["eid"].
            std::string val;
            take(val);
            c.extra["eid"] = val;
        }
        else if (a.rfind("--", 0) == 0)
        {
            std::string key = a.substr(2);
            // Support both "--K=V" and "--K V" forms.
            auto eq = key.find('=');
            if (eq != std::string::npos)
            {
                c.extra[key.substr(0, eq)] = key.substr(eq + 1);
            }
            else if (i + 1 < argc &&
                     std::string(argv[i + 1]).rfind("--", 0) != 0)
            {
                c.extra[key] = argv[++i];
            }
            else
            {
                c.extra[key] = "";
            }
        }
    }
    return c;
}

/** Pull the full EM SystemDumpAllowableType record for --em-config-path via a
 *  synchronous GetAll. Returns std::nullopt if the path is empty or the call
 *  fails. */
inline std::optional<std::pair<std::vector<std::string>, EmCollectorArgs>>
    fetchEmRecord(const std::string& emConfigPath)
{
    if (emConfigPath.empty())
    {
        return std::nullopt;
    }

    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);

    using Variant = std::variant<std::string, int64_t, uint8_t, bool,
                                 std::vector<std::string>, EmVariantMap>;
    std::map<std::string, Variant> props;
    try
    {
        // Find the owning service via ObjectMapper.GetObject (bus name varies).
        std::map<std::string, std::vector<std::string>> services;
        auto getObj = conn->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetObject");
        std::vector<std::string> ifaces{
            "xyz.openbmc_project.Configuration.SystemDumpAllowableType"};
        getObj.append(emConfigPath, ifaces);
        auto reply = conn->call(getObj);
        reply.read(services);
        if (services.empty())
        {
            lg2::warning("collector: no service owns EM path '{PATH}'", "PATH",
                         emConfigPath);
            return std::nullopt;
        }
        std::string service = services.begin()->first;

        auto getAll =
            conn->new_method_call(service.c_str(), emConfigPath.c_str(),
                                  "org.freedesktop.DBus.Properties", "GetAll");
        getAll.append(std::string(
            "xyz.openbmc_project.Configuration.SystemDumpAllowableType"));
        auto reply2 = conn->call(getAll);
        reply2.read(props);
    }
    catch (const std::exception& e)
    {
        lg2::warning("collector: failed to fetch EM record at '{PATH}': {ERR}",
                     "PATH", emConfigPath, "ERR", e.what());
        return std::nullopt;
    }

    std::vector<std::string> expected;
    EmCollectorArgs args;
    if (auto it = props.find("ExpectedFiles"); it != props.end())
    {
        if (auto p = std::get_if<std::vector<std::string>>(&it->second))
        {
            expected = *p;
        }
    }
    if (auto it = props.find("CollectorArgs"); it != props.end())
    {
        if (auto p = std::get_if<EmVariantMap>(&it->second))
        {
            args = parseCollectorArgs(*p);
        }
    }
    return std::make_pair(std::move(expected), std::move(args));
}

/** Run a shell command line via std::system(); returns its exit status (127 if
 *  it couldn't run). A shell is used intentionally so callers can use
 *  redirects/pipes/compound commands.
 *
 *  SECURITY: pass ONLY daemon- or EM-config-controlled data (dump paths, entry
 *  ids, entity-manager Pages/DeviceType). Never pass untrusted input; quote
 *  daemon-derived paths. Redfish user parameters reach collectors via execv
 *  arg-vectors in dump_manager_system.cpp, not this helper. */
inline int runExternal(const std::string& cmdline)
{
    // NOLINTNEXTLINE(cert-env33-c)
    int rc = std::system(cmdline.c_str());
    if (rc == -1)
    {
        return 127;
    }
    return WEXITSTATUS(rc);
}

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

/** Platform role from the collector's own EM record: HMC devices are named
 *  "HGX_*". Finds the Configuration.Dump record advertising `kind` and checks
 *  its Name. False (BMC) when no such record exists. */
inline bool isHmcFromEm(const std::string& kind)
{
    for (const auto& obj :
         enumerateEmObjects({"xyz.openbmc_project.Configuration.Dump"}))
    {
        auto sd = emStringVec(obj, "SupportedDumps");
        if (std::find(sd.begin(), sd.end(), kind) != sd.end())
        {
            return emString(obj, "Name").starts_with("HGX");
        }
    }
    return false;
}

/** Per-target view of a `Configuration.Dump` record with EID resolved.
 *  Returned by enumerateDumpTargetsForKind(). */
struct DumpTargetView
{
    std::string name;                      // e.g. "HGX_GPU_0"
    std::optional<std::string> deviceType; // present for fan-out kinds
    std::vector<std::string> supportedDumps;
    uint8_t eid = 0;
};

namespace detail
{

/** MCTPReactor endpoint subtree root (mctpd / codeconstruct). */
constexpr auto mctpReactorRoot = "/au/com/codeconstruct/mctp1";

/** Parse the EID from an endpoint path basename; nullopt if not a number
 *  <= 0xFF. */
inline std::optional<uint8_t> eidFromEndpointPath(const std::string& path)
{
    auto base = std::filesystem::path(path).filename().string();
    try
    {
        auto v = std::stoul(base, nullptr, 0);
        if (v <= 0xFF)
        {
            return static_cast<uint8_t>(v);
        }
    }
    catch (...)
    {}
    return std::nullopt;
}

/** Resolve every live MCTP endpoint to its dump capability: match each
 *  endpoint's `configured_by` device name against Configuration.Dump records
 *  to read SupportedDumps / DeviceType. Unmatched endpoints are skipped. */
inline std::vector<DumpTargetView> resolveEndpointDumps()
{
    std::vector<DumpTargetView> out;

    // Index every Configuration.Dump record by Name (one per device).
    struct DumpInfo
    {
        std::vector<std::string> supportedDumps;
        std::optional<std::string> deviceType;
    };
    std::map<std::string, DumpInfo> dumpByName;
    for (const auto& obj :
         enumerateEmObjects({"xyz.openbmc_project.Configuration.Dump"}))
    {
        auto name = emString(obj, "Name");
        if (name.empty())
        {
            continue;
        }
        DumpInfo di;
        di.supportedDumps = emStringVec(obj, "SupportedDumps");
        if (di.supportedDumps.empty())
        {
            continue;
        }
        if (auto dt = emString(obj, "DeviceType"); !dt.empty())
        {
            di.deviceType = std::move(dt);
        }
        dumpByName.emplace(std::move(name), std::move(di));
    }
    if (dumpByName.empty())
    {
        return out;
    }

    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    using Assoc = std::tuple<std::string, std::string, std::string>;
    using GetSubTreeResp =
        std::map<std::string, std::map<std::string, std::vector<std::string>>>;

    GetSubTreeResp subtree;
    try
    {
        auto call = conn->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        call.append(std::string(mctpReactorRoot));
        call.append(int32_t{0});
        call.append(std::vector<std::string>{
            "xyz.openbmc_project.Association.Definitions"});
        auto reply = conn->call(call);
        reply.read(subtree);
    }
    catch (const std::exception& e)
    {
        lg2::info("collector: endpoint enumeration failed: {ERR}", "ERR",
                  e.what());
        return out;
    }

    for (const auto& [path, services] : subtree)
    {
        auto eid = eidFromEndpointPath(path);
        if (!eid || services.empty())
        {
            continue;
        }
        // configured_by association target -> device name
        std::string targetPath;
        try
        {
            const auto& service = services.begin()->first;
            std::variant<std::vector<Assoc>> v;
            auto get =
                conn->new_method_call(service.c_str(), path.c_str(),
                                      "org.freedesktop.DBus.Properties", "Get");
            get.append("xyz.openbmc_project.Association.Definitions",
                       "Associations");
            auto reply = conn->call(get);
            reply.read(v);
            for (const auto& [forward, reverse, tgt] :
                 std::get<std::vector<Assoc>>(v))
            {
                (void)reverse;
                if (forward == "configured_by" && !tgt.empty())
                {
                    targetPath = tgt;
                    break;
                }
            }
        }
        catch (const std::exception&)
        {
            continue;
        }
        if (targetPath.empty())
        {
            continue;
        }

        // Match the device name to a Configuration.Dump record by Name.
        auto deviceName = std::filesystem::path(targetPath).filename().string();
        auto it = dumpByName.find(deviceName);
        if (it == dumpByName.end())
        {
            continue;
        }
        DumpTargetView view;
        view.name = deviceName;
        view.eid = *eid;
        view.supportedDumps = it->second.supportedDumps;
        view.deviceType = it->second.deviceType;
        out.push_back(std::move(view));
    }
    return out;
}

} // namespace detail

/** Resolve every live MCTP endpoint's dump capability (see
 *  detail::resolveEndpointDumps) and return those whose SupportedDumps
 *  contains `kind`. */
inline std::vector<DumpTargetView> enumerateDumpTargetsForKind(
    const std::string& kind)
{
    std::vector<DumpTargetView> out;
    for (auto& view : detail::resolveEndpointDumps())
    {
        if (std::find(view.supportedDumps.begin(), view.supportedDumps.end(),
                      kind) != view.supportedDumps.end())
        {
            out.push_back(std::move(view));
        }
    }
    return out;
}

/** @brief Ensure the collector's output directory exists. */
inline bool ensureOutDir(const std::string& outDir)
{
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec)
    {
        lg2::error("collector: cannot create out dir '{DIR}': {ERR}", "DIR",
                   outDir, "ERR", ec.message());
        return false;
    }
    return true;
}

} // namespace collectors
} // namespace dump
} // namespace phosphor
