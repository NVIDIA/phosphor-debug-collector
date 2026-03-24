/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * ROT dump collector. Invoked by erot_dump.sh with -o <dir>
 * to collect discovery-based IROT/VROT entries (NSM via nsmd
 * D-Bus Raw API) into an existing workspace directory.
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

// Paths and constants
constexpr const char* NSM_DBUS_SERVICE = "xyz.openbmc_project.NSM";
constexpr const char* NSM_DBUS_RAW_PATH = "/xyz/openbmc_project/NSM/Raw";
constexpr const char* NSM_DBUS_RAW_INTF = "com.nvidia.Protocol.NSM.Raw";
constexpr const char* DBUS_PROP_INTF = "org.freedesktop.DBus.Properties";
constexpr const char* NSM_FRU_INTF = "xyz.openbmc_project.FruDevice";
constexpr const char* ASYNC_STATUS_INTF = "com.nvidia.Async.Status";
constexpr const char* ASYNC_VALUE_INTF = "com.nvidia.Async.Value";

constexpr uint8_t NSM_DEVICE_EROT_ID = 4;
constexpr uint8_t NSM_MESSAGE_TYPE_DIAG = 4;
constexpr uint8_t NSM_CMD_GET_DEVICE_DIAGNOSTICS = 64;
constexpr uint8_t NSM_MESSAGE_TYPE_DCD = 0;
constexpr uint8_t NSM_CMD_QUERY_DEVICE_IDENTIFICATION = 9;
constexpr uint8_t NSM_MESSAGE_TYPE_FW = 6;
constexpr uint8_t NSM_CMD_GET_ROT_STATE_INFO = 1;
constexpr uint8_t NSM_CMD_QUERY_FW_COMP_ID = 7;
constexpr uint16_t NSM_FW_COMPONENT_CLASS = 10;
constexpr uint8_t PCI_VDM_MSG_TYPE = 0x7E;
constexpr uint8_t NSM_MSG_FORMAT_VERSION = 1;
constexpr uint8_t NSM_DEVICE_ROLE = 0;

constexpr int PROBE_TIMEOUT_MS = 1000;

struct NsmDeviceInfo
{
    uint8_t deviceType{};
    uint8_t deviceRole{};
    uint8_t instanceId{};
};

class NsmDbusClient
{
  public:
    explicit NsmDbusClient(sdbusplus::bus_t& busRef) : bus(busRef) {}

    bool executeCommand(uint8_t eid, uint8_t messageType, uint8_t commandCode,
                        const std::vector<uint8_t>& payload,
                        std::vector<uint8_t>& response, int timeoutMs,
                        bool isLongRunning = false)
    {
        auto deviceInfo = getDeviceInfoByEid(eid);
        if (!deviceInfo)
        {
            if (lastError.empty())
            {
                setError("No device info found for EID " + std::to_string(eid));
            }
            return false;
        }

        const int reqFd = createRequestFd(payload);
        if (reqFd < 0)
        {
            setError("Failed to create request FD for EID " +
                     std::to_string(eid));
            return false;
        }
        sdbusplus::message::unix_fd requestFd(reqFd);

        sdbusplus::message::object_path asyncPath;
        try
        {
            auto method =
                bus.new_method_call(NSM_DBUS_SERVICE, NSM_DBUS_RAW_PATH,
                                    NSM_DBUS_RAW_INTF, "SendRequest");
            method.append(deviceInfo->deviceType, deviceInfo->deviceRole,
                          deviceInfo->instanceId, isLongRunning, messageType,
                          commandCode, requestFd, NSM_MSG_FORMAT_VERSION);
            auto reply = bus.call(method);
            reply.read(asyncPath);
        }
        catch (const std::exception& e)
        {
            setError("SendRequest failed for EID " + std::to_string(eid) +
                     ", type=" + std::to_string(messageType) +
                     ", cmd=" + std::to_string(commandCode) + ": " + e.what());
            close(reqFd);
            return false;
        }
        const std::string asyncPathStr = static_cast<std::string>(asyncPath);

        if (!waitForCompletion(asyncPathStr, timeoutMs))
        {
            close(reqFd);
            if (lastError.empty())
            {
                setError("Async completion failed for EID " +
                         std::to_string(eid) + " async_path=" + asyncPathStr);
            }
            return false;
        }
        if (!readAsyncValue(asyncPathStr, reqFd, response))
        {
            close(reqFd);
            if (lastError.empty())
            {
                setError("Failed to read async value for EID " +
                         std::to_string(eid) + " async_path=" + asyncPathStr);
            }
            return false;
        }
        close(reqFd);
        if (response.empty())
        {
            setError("Async value is empty for EID " + std::to_string(eid) +
                     " async_path=" + asyncPathStr +
                     " status=" + lastAsyncStatus);
            return false;
        }
        lastError.clear();
        return true;
    }

  private:
    std::optional<NsmDeviceInfo> getDeviceInfoByEid(uint8_t eid)
    {
        auto cacheIt = deviceInfoCache.find(eid);
        if (cacheIt != deviceInfoCache.end())
        {
            return cacheIt->second;
        }

        // Common inventory shape in nsmd is
        // /xyz/openbmc_project/FruDevice/<eid>.
        const std::string fruPath =
            std::string("/xyz/openbmc_project/FruDevice/") +
            std::to_string(eid);
        auto info = queryDeviceInfoByPath(fruPath);
        if (info)
        {
            deviceInfoCache.emplace(eid, *info);
            return info;
        }
        if (lastError.empty())
        {
            setError("No FruDevice object for EID " + std::to_string(eid) +
                     " at path " + fruPath);
        }
        return std::nullopt;
    }

    std::optional<NsmDeviceInfo> queryDeviceInfoByPath(const std::string& path)
    {
        auto method = bus.new_method_call(NSM_DBUS_SERVICE, path.c_str(),
                                          DBUS_PROP_INTF, "GetAll");
        method.append(NSM_FRU_INTF);

        using FruProp = std::variant<uint8_t, uint16_t, uint32_t, std::string>;
        std::map<std::string, FruProp> props;
        try
        {
            auto reply = bus.call(method);
            reply.read(props);
        }
        catch (const std::exception& e)
        {
            setError("FruDevice GetAll failed at " + path + ": " + e.what());
            return std::nullopt;
        }

        const auto typeIt = props.find("DEVICE_TYPE");
        const auto instIt = props.find("INSTANCE_NUMBER");
        if (typeIt == props.end() || instIt == props.end())
        {
            setError("Missing DEVICE_TYPE or INSTANCE_NUMBER at " + path);
            return std::nullopt;
        }

        auto toByte = [](const FruProp& v) -> std::optional<uint8_t> {
            if (std::holds_alternative<uint8_t>(v))
            {
                return std::get<uint8_t>(v);
            }
            if (std::holds_alternative<uint16_t>(v))
            {
                return static_cast<uint8_t>(std::get<uint16_t>(v) & 0xFF);
            }
            if (std::holds_alternative<uint32_t>(v))
            {
                return static_cast<uint8_t>(std::get<uint32_t>(v) & 0xFF);
            }
            return std::nullopt;
        };

        auto dtype = toByte(typeIt->second);
        auto inst = toByte(instIt->second);
        if (!dtype || !inst)
        {
            setError("Invalid DEVICE_TYPE or INSTANCE_NUMBER type at " + path);
            return std::nullopt;
        }

        uint8_t role = NSM_DEVICE_ROLE;
        const auto roleIt = props.find("DEVICE_ROLE");
        if (roleIt != props.end())
        {
            auto maybeRole = toByte(roleIt->second);
            if (maybeRole)
            {
                role = *maybeRole;
            }
        }
        return NsmDeviceInfo{*dtype, role, *inst};
    }

    static int createRequestFd(const std::vector<uint8_t>& payload)
    {
        int flags = MFD_CLOEXEC;
#ifdef MFD_NOEXEC_SEAL
        flags |= MFD_NOEXEC_SEAL;
#endif
        // NOLINTNEXTLINE(cert-env33-c)
        int fd = memfd_create("rot_dump_nsm_req", flags);
        if (fd < 0)
        {
            return -1;
        }
        if (!payload.empty())
        {
            const ssize_t written = write(fd, payload.data(), payload.size());
            if (written < 0 || static_cast<size_t>(written) != payload.size())
            {
                close(fd);
                return -1;
            }
        }
        lseek(fd, 0, SEEK_SET);
        return fd;
    }

    bool waitForCompletion(const std::string& asyncPath, int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto method = bus.new_method_call(
                NSM_DBUS_SERVICE, asyncPath.c_str(), DBUS_PROP_INTF, "Get");
            method.append(ASYNC_STATUS_INTF, "Status");
            try
            {
                auto reply = bus.call(method);
                std::variant<std::string> status;
                reply.read(status);
                const std::string& statusStr = std::get<std::string>(status);
                lastAsyncStatus = statusStr;
                if (statusStr ==
                    "com.nvidia.Async.Status.AsyncOperationStatus.Success")
                {
                    return true;
                }
                if (statusStr !=
                    "com.nvidia.Async.Status.AsyncOperationStatus.InProgress")
                {
                    std::string errMsg = "Async status terminal failure at ";
                    errMsg += asyncPath;
                    errMsg += " status=";
                    errMsg += statusStr;
                    setError(errMsg);
                    return false;
                }
            }
            catch (const std::exception& e)
            {
                std::string errMsg = "Async status read failed at ";
                errMsg += asyncPath;
                errMsg += ": ";
                errMsg += e.what();
                setError(errMsg);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::string errMsg = "Async wait timeout at ";
        errMsg += asyncPath;
        setError(errMsg);
        return false;
    }

    static bool readAllFromFd(int fd, std::vector<uint8_t>& out)
    {
        if (lseek(fd, 0, SEEK_SET) < 0)
        {
            return false;
        }
        out.clear();
        std::array<uint8_t, 4096> buf{};
        while (true)
        {
            const ssize_t got = read(fd, buf.data(), buf.size());
            if (got < 0)
            {
                return false;
            }
            if (got == 0)
            {
                break;
            }
            out.insert(out.end(), buf.begin(), buf.begin() + got);
        }
        return true;
    }

    bool readAsyncValue(const std::string& asyncPath, int requestFd,
                        std::vector<uint8_t>& out)
    {
        try
        {
            auto method = bus.new_method_call(
                NSM_DBUS_SERVICE, asyncPath.c_str(), DBUS_PROP_INTF, "Get");
            method.append(ASYNC_VALUE_INTF, "Value");
            auto reply = bus.call(method);
            std::variant<uint8_t, bool, std::vector<uint8_t>> wrapped;
            reply.read(wrapped);

            if (std::holds_alternative<uint8_t>(wrapped))
            {
                // nsmd Raw API can return completion-code in Value and write
                // response payload to the provided request FD.
                const uint8_t cc = std::get<uint8_t>(wrapped);
                if (cc != 0)
                {
                    setError("Async value completion-code failure at " +
                             asyncPath + " cc=" + std::to_string(cc));
                    return false;
                }

                std::vector<uint8_t> payload;
                if (!readAllFromFd(requestFd, payload))
                {
                    setError("Async completion-code success but failed to read "
                             "response fd at " +
                             asyncPath);
                    return false;
                }
                // ([cc, reason, data_size_l, data_size_h, payload...]).
                out = std::move(payload);
                return true;
            }

            if (std::holds_alternative<bool>(wrapped))
            {
                setError("Async value bool result at " + asyncPath);
                return false;
            }

            out = std::get<std::vector<uint8_t>>(wrapped);
            return !out.empty();
        }
        catch (const std::exception& e)
        {
            setError("Async value read failed at " + asyncPath + ": " +
                     e.what());
            return false;
        }
    }

    sdbusplus::bus_t& bus;
    std::unordered_map<uint8_t, NsmDeviceInfo> deviceInfoCache;
    std::string lastError;
    std::string lastAsyncStatus;

    void setError(const std::string& msg)
    {
        lastError = msg;
    }
};

static bool writeFile(const fs::path& path, const std::string& content)
{
    std::ofstream f(path);
    if (!f)
    {
        return false;
    }
    f << content;
    return f.good();
}

static bool endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

static std::string toHexByte(uint8_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(2)
       << static_cast<unsigned>(value);
    return os.str();
}

struct NsmResponseMeta
{
    bool valid = false;
    uint8_t completionCode = 0xFF;
    uint16_t dataSize = 0;
    size_t payloadOffset = 0;
    std::string layout;
};

static std::string bytesToHex(const std::vector<uint8_t>& bytes)
{
    std::ostringstream os;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i != 0)
        {
            os << " ";
        }
        os << toHexByte(bytes[i]);
    }
    return os.str();
}

static NsmResponseMeta parseNsmResponseMeta(const std::vector<uint8_t>& bytes)
{
    // [cc, rsvd, rsvd, data_size_l, data_size_h, payload...]
    if (bytes.size() >= 5)
    {
        const uint16_t dataSize = static_cast<uint16_t>(
            bytes[3] + (static_cast<uint16_t>(bytes[4]) << 8));
        const size_t payloadOffset = 5;
        if (bytes.size() >= payloadOffset + dataSize)
        {
            return NsmResponseMeta{true, bytes[0], dataSize, payloadOffset,
                                   "compact"};
        }
    }

    return {};
}

static std::string makeRotStatePayloadHex(
    uint16_t componentClass, uint16_t componentId, uint8_t componentIndex)
{
    std::ostringstream payload;
    payload << toHexByte(static_cast<uint8_t>(componentClass & 0xFF))
            << toHexByte(static_cast<uint8_t>((componentClass >> 8) & 0xFF))
            << toHexByte(static_cast<uint8_t>(componentId & 0xFF))
            << toHexByte(static_cast<uint8_t>((componentId >> 8) & 0xFF))
            << toHexByte(componentIndex);
    return payload.str();
}

static void pruneIntermediateArtifacts(const fs::path& tmpDir)
{
    for (const auto& entry : fs::directory_iterator(tmpDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string name = entry.path().filename().string();

        // Keep error logs only when non-empty to reduce archive clutter.
        if ((endsWith(name, ".err") || endsWith(name, "_rot_error.log")) &&
            entry.file_size() == 0)
        {
            fs::remove(entry.path());
        }
    }
}

static void writeBootStatusTextLog(
    const fs::path& outFile, uint32_t eid, const std::string& componentSource,
    uint16_t componentClass, uint16_t componentId, uint8_t componentIndex,
    const std::vector<uint8_t>& resp)
{
    const NsmResponseMeta meta = parseNsmResponseMeta(resp);
    std::ostringstream os;
    size_t byteCount = resp.size();
    unsigned cc = meta.valid ? meta.completionCode : 0;
    unsigned dataSize = meta.valid ? meta.dataSize : 0;
    size_t payloadAvailable = (meta.valid && byteCount > meta.payloadOffset)
                                  ? (byteCount - meta.payloadOffset)
                                  : 0;

    os << "source=nsm_get_rot_state_info\n";
    os << "eid=" << eid << "\n";
    os << "component_source=" << componentSource << "\n";
    os << "component_class=0x" << std::hex << std::setfill('0') << std::setw(4)
       << componentClass << " component_id=0x" << std::setw(4) << componentId
       << " component_index=0x" << std::setw(2)
       << static_cast<unsigned>(componentIndex) << std::dec << "\n";
    os << "completion_code=" << cc << "\n";
    os << "response_size_bytes=" << byteCount << "\n";
    os << "data_size_field=" << dataSize << "\n";
    os << "payload_bytes_available=" << payloadAvailable << "\n";
    os << "payload_bytes_expected=" << dataSize << "\n";

    const std::string rawHex = bytesToHex(resp);
    std::string payloadHex;
    if (meta.valid && meta.payloadOffset < byteCount)
    {
        for (size_t i = meta.payloadOffset; i < byteCount; ++i)
        {
            payloadHex += toHexByte(resp[i]) + " ";
        }
    }
    if (!payloadHex.empty())
    {
        payloadHex.pop_back();
    }
    os << "raw_response_hex=" << rawHex << "\n";
    os << "payload_hex=" << payloadHex << "\n";

    writeFile(outFile, os.str());
}

// Return component_class, component_id, component_index for first component
// with component_class == NSM_FW_COMPONENT_CLASS.
static bool queryFwComponentForEid(NsmDbusClient& client, uint32_t eid,
                                   uint16_t& outClass, uint16_t& outId,
                                   uint8_t& outIndex)
{
    std::vector<uint8_t> bytes;
    if (!client.executeCommand(static_cast<uint8_t>(eid), NSM_MESSAGE_TYPE_FW,
                               NSM_CMD_QUERY_FW_COMP_ID, {}, bytes,
                               PROBE_TIMEOUT_MS))
    {
        return false;
    }
    const NsmResponseMeta meta = parseNsmResponseMeta(bytes);
    if (!meta.valid)
    {
        return false;
    }
    if (meta.completionCode != 0 || meta.dataSize < 6)
    {
        return false;
    }
    if (bytes.size() < meta.payloadOffset + meta.dataSize)
    {
        return false;
    }

    const size_t payloadBase = meta.payloadOffset;
    unsigned componentCount = bytes[payloadBase];
    if (componentCount == 0)
    {
        return false;
    }

    // Scan for component with class == NSM_FW_COMPONENT_CLASS (per-component
    // record: class(2), id(2), index(1) = 5 bytes each)
    size_t offset = payloadBase + 1;
    const size_t payloadEnd = payloadBase + meta.dataSize;
    for (unsigned i = 0; i < componentCount && offset + 5 <= payloadEnd; ++i)
    {
        uint16_t cclass =
            bytes[offset] + (static_cast<uint16_t>(bytes[offset + 1]) << 8);
        if (cclass == NSM_FW_COMPONENT_CLASS)
        {
            outClass = cclass;
            outId = bytes[offset + 2] +
                    (static_cast<uint16_t>(bytes[offset + 3]) << 8);
            outIndex = bytes[offset + 4];
            return true;
        }
        offset += 5;
    }
    return false;
}

static std::vector<uint8_t> payloadFromHex(const std::string& payloadHex)
{
    std::vector<uint8_t> payload;
    if ((payloadHex.size() % 2) != 0)
    {
        return payload;
    }
    payload.reserve(payloadHex.size() / 2);
    for (size_t i = 0; i < payloadHex.size(); i += 2)
    {
        auto hi = payloadHex[i];
        auto lo = payloadHex[i + 1];
        auto hexToNibble = [](char c) -> int {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        };
        int upper = hexToNibble(hi);
        int lower = hexToNibble(lo);
        if (upper < 0 || lower < 0)
        {
            payload.clear();
            return payload;
        }
        payload.push_back(static_cast<uint8_t>((upper << 4) | lower));
    }
    return payload;
}

static bool isRotDeviceEid(NsmDbusClient& client, uint32_t eid)
{
    std::vector<uint8_t> bytes;
    if (!client.executeCommand(static_cast<uint8_t>(eid), NSM_MESSAGE_TYPE_DCD,
                               NSM_CMD_QUERY_DEVICE_IDENTIFICATION, {}, bytes,
                               PROBE_TIMEOUT_MS))
    {
        return false;
    }

    const NsmResponseMeta meta = parseNsmResponseMeta(bytes);
    if (!meta.valid)
    {
        return false;
    }
    if (meta.completionCode != 0 || meta.dataSize < 1 ||
        bytes.size() < meta.payloadOffset + meta.dataSize)
    {
        return false;
    }

    const uint8_t deviceId = bytes[meta.payloadOffset];

    // NSM device_id=4 identifies RoT-class endpoints, including IROT and VROT.
    if (deviceId != NSM_DEVICE_EROT_ID)
    {
        return false;
    }
    return true;
}

// Discover ROT targets via D-Bus (MCTP endpoints with PCI VDM + DCD probe).
using EidNamePair = std::pair<uint32_t, std::string>;
static std::vector<EidNamePair> discoverRotTargets(sdbusplus::bus_t& bus,
                                                   NsmDbusClient& client)
{
    std::vector<EidNamePair> targets;
    const char* mapperService = "xyz.openbmc_project.ObjectMapper";
    const char* mapperPath = "/xyz/openbmc_project/object_mapper";
    const char* mapperInterface = "xyz.openbmc_project.ObjectMapper";
    const char* endpointInterface = "xyz.openbmc_project.MCTP.Endpoint";

    auto method = bus.new_method_call(mapperService, mapperPath,
                                      mapperInterface, "GetSubTree");
    method.append("/", 0, std::array<const char*, 1>{endpointInterface});

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        result;
    try
    {
        auto reply = bus.call(method);
        reply.read(result);
    }
    catch (const std::exception&)
    {
        return targets;
    }

    for (const auto& entry : result)
    {
        const auto& path = entry.first;
        const auto& services = entry.second;
        if (services.empty())
        {
            continue;
        }
        std::string service = services.begin()->first;

        auto getProp = [&](const char* prop)
            -> std::variant<uint8_t, uint16_t, std::vector<uint8_t>> {
            auto m =
                bus.new_method_call(service.c_str(), path.c_str(),
                                    "org.freedesktop.DBus.Properties", "Get");
            m.append(endpointInterface, prop);
            try
            {
                auto r = bus.call(m);
                std::variant<uint8_t, uint16_t, std::vector<uint8_t>> v;
                r.read(v);
                return v;
            }
            catch (...)
            {
                return std::vector<uint8_t>{};
            }
        };

        auto eidVar = getProp("EID");
        uint32_t eid = 0;
        if (std::holds_alternative<uint8_t>(eidVar))
        {
            eid = std::get<uint8_t>(eidVar);
        }
        else if (std::holds_alternative<uint16_t>(eidVar))
        {
            eid = std::get<uint16_t>(eidVar);
        }
        else
        {
            continue;
        }

        auto typesVar = getProp("SupportedMessageTypes");
        bool hasPciVdm = false;
        if (std::holds_alternative<std::vector<uint8_t>>(typesVar))
        {
            const auto& arr = std::get<std::vector<uint8_t>>(typesVar);
            hasPciVdm = std::find(arr.begin(), arr.end(), PCI_VDM_MSG_TYPE) !=
                        arr.end();
        }
        if (!hasPciVdm)
        {
            continue;
        }

        if (!isRotDeviceEid(client, eid))
        {
            continue;
        }

        std::string name = "EID_" + std::to_string(eid);
        targets.emplace_back(eid, name);
    }

    std::sort(targets.begin(), targets.end(),
              [](const EidNamePair& a, const EidNamePair& b) {
                  return a.first < b.first;
              });
    return targets;
}

static bool nsmDiagDumpEid(NsmDbusClient& client, const std::string& name,
                           uint32_t eid, const fs::path& tmpDir)
{
    fs::path outputFile = tmpDir / (name + "_rot_dump.bin");
    fs::path errFile = tmpDir / (name + "_rot_error.log");
    writeFile(errFile, "");

    std::ofstream out(outputFile, std::ios::binary);
    if (!out)
    {
        return false;
    }

    uint8_t segmentId = 0;
    std::set<uint8_t> seen;
    const int timeoutMs = 5000; // full segment reads use default timeout

    for (int i = 0; i < 256; ++i)
    {
        if (seen.contains(segmentId))
        {
            return true;
        }
        seen.insert(segmentId);

        std::vector<uint8_t> bytes;
        if (!client.executeCommand(
                static_cast<uint8_t>(eid), NSM_MESSAGE_TYPE_DIAG,
                NSM_CMD_GET_DEVICE_DIAGNOSTICS, {segmentId}, bytes, timeoutMs))
        {
            std::ofstream err(errFile, std::ios::app);
            err << name << ": nsmd D-Bus command failed for segment "
                << static_cast<unsigned>(segmentId) << "\n";
            return false;
        }

        if (bytes.size() < 6 || bytes[0] != 0)
        {
            std::ofstream err(errFile, std::ios::app);
            err << name << ": short or error response\n";
            return false;
        }
        const NsmResponseMeta meta = parseNsmResponseMeta(bytes);
        if (!meta.valid || meta.completionCode != 0 || meta.dataSize < 1 ||
            bytes.size() < meta.payloadOffset + meta.dataSize)
        {
            std::ofstream err(errFile, std::ios::app);
            err << name << ": invalid diagnostics response (hex="
                << bytesToHex(bytes) << ")\n";
            return false;
        }
        uint8_t nextSegmentId = bytes[meta.payloadOffset];
        size_t segmentLen = meta.dataSize - 1;
        if (segmentLen > 0)
        {
            out.write(reinterpret_cast<const char*>(
                          bytes.data() + meta.payloadOffset + 1),
                      static_cast<std::streamsize>(segmentLen));
        }

        if (nextSegmentId == 255 || nextSegmentId == segmentId)
        {
            return true;
        }
        segmentId = nextSegmentId;
    }

    std::ofstream err(errFile, std::ios::app);
    err << name << ": reached max diagnostics segments\n";
    return false;
}

static bool collectBootStatusForEid(NsmDbusClient& client,
                                    const std::string& name, uint32_t eid,
                                    const fs::path& tmpDir)
{
    fs::path outFile = tmpDir / (name + "_rot_query_boot_status.log");

    uint16_t componentClass = NSM_FW_COMPONENT_CLASS;
    uint16_t componentId = 0;
    uint8_t componentIndex = 0;
    if (!queryFwComponentForEid(client, eid, componentClass, componentId,
                                componentIndex))
    {
        return false;
    }

    const std::string payloadHex =
        makeRotStatePayloadHex(componentClass, componentId, componentIndex);
    const std::vector<uint8_t> payload = payloadFromHex(payloadHex);
    std::vector<uint8_t> resp;
    if (payload.empty() ||
        !client.executeCommand(static_cast<uint8_t>(eid), NSM_MESSAGE_TYPE_FW,
                               NSM_CMD_GET_ROT_STATE_INFO, payload, resp, 5000))
    {
        return false;
    }
    const NsmResponseMeta meta = parseNsmResponseMeta(resp);
    if (meta.valid && meta.completionCode != 0)
    {
        return false;
    }

    // For GET_ROT_STATE_INFO, downstream decoder expects raw response body
    // bytes (starting with completion code and telemetry count)
    writeBootStatusTextLog(outFile, eid, "query_fw_comp_id", componentClass,
                           componentId, componentIndex, resp);
    return true;
}

int main(int argc, char* argv[])
{
    std::string outputDirArg;

    int c;
    while ((c = getopt(argc, argv, "ho:")) != -1)
    {
        switch (c)
        {
            case 'h':
                std::cout
                    << "Usage: rot_dump [-h] -o <output_dir>\n"
                       "  -o  (required) directory to place IROT/VROT files\n";
                return 0;
            case 'o':
                outputDirArg = optarg;
                break;
            default:
                return 1;
        }
    }

    if (outputDirArg.empty())
    {
        std::cerr << "argument -o is required" << std::endl;
        return 1;
    }

    fs::path outputDir(outputDirArg);
    fs::create_directories(outputDir);

    bool anyOutput = false;

    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    NsmDbusClient nsmClient(bus);
    std::vector<EidNamePair> rotTargets = discoverRotTargets(bus, nsmClient);

    for (const auto& [eid, name] : rotTargets)
    {
        bool targetProducedOutput = false;
        if (nsmDiagDumpEid(nsmClient, name, eid, outputDir))
        {
            targetProducedOutput = true;
        }
        if (collectBootStatusForEid(nsmClient, name, eid, outputDir))
        {
            targetProducedOutput = true;
        }
        anyOutput = anyOutput || targetProducedOutput;
    }

    if (!anyOutput)
    {
        std::cerr << "No ROT data collected from NSM discovery"
                  << std::endl;
        return 1;
    }

    pruneIntermediateArtifacts(outputDir);
    return 0;
}
