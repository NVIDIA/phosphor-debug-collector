/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "nsm_dump_utils.hpp"

#include "nsm_device_utils.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cstring>
#include <format>
#include <map>
#include <variant>
#include <vector>

using namespace phosphor::logging;

bool deviceHasInterface(const std::string& targetDevice,
                        const std::string& iface)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    std::string rootPath("/xyz/openbmc_project/inventory/system");
    std::vector<std::string> paths;

    auto mapper = bus.new_method_call(
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
    mapper.append(rootPath.c_str());
    mapper.append(0);
    mapper.append(std::vector<std::string>({iface}));
    try
    {
        auto reply = bus.call(mapper);
        reply.read(paths);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("deviceHasInterface GetSubTreePaths failed");
        log<level::ERR>(e.what());
        return false;
    }
    for (const auto& path : paths)
    {
        if (phosphor::dump::nsm::pathMatchesDeviceSelector(path, targetDevice))
        {
            log<level::DEBUG>(
                std::format(
                    "deviceHasInterface: device={} iface={} -> found at {}",
                    targetDevice, iface, path)
                    .c_str());
            return true;
        }
    }
    log<level::DEBUG>(
        std::format(
            "deviceHasInterface: device={} iface={} -> not found ({} matching path(s) total)",
            targetDevice, iface, paths.size())
            .c_str());
    return false;
}

bool objectPathHasInterface(const std::string& objectPath,
                            const std::string& iface)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    auto mapper =
        bus.new_method_call("xyz.openbmc_project.ObjectMapper",
                            "/xyz/openbmc_project/object_mapper",
                            "xyz.openbmc_project.ObjectMapper", "GetObject");
    mapper.append(objectPath);
    mapper.append(std::vector<std::string>({iface}));
    try
    {
        auto reply = bus.call(mapper);
        std::map<std::string, std::vector<std::string>> svcToIfaces;
        reply.read(svcToIfaces);
        return !svcToIfaces.empty();
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        // GetObject throws (ResourceNotFound) when no service implements
        // `iface` at this exact path — treat as "interface not present".
        log<level::DEBUG>(
            std::format(
                "objectPathHasInterface: path={} iface={} -> not found ({})",
                objectPath, iface, e.what())
                .c_str());
        return false;
    }
}

AsyncStatusInfo asyncStatusToHuman(const std::string& enumStr)
{
    const std::string prefix = "com.nvidia.Async.Status.AsyncOperationStatus.";

    if (enumStr == prefix + "Success")
    {
        return {"Operation completed successfully", "OK", "", false};
    }
    if (enumStr == prefix + "InProgress")
    {
        return {"Operation in progress", "OK", "Wait for completion.", false};
    }
    if (enumStr == prefix + "Timeout")
    {
        return {"No response from device", "Warning",
                "Verify the device is powered and reachable, then retry.",
                true};
    }
    if (enumStr == prefix + "Unavailable")
    {
        return {"Device temporarily unavailable", "Critical",
                "Wait for device to become ready (boot/update/busy) and retry.",
                true};
    }
    if (enumStr == prefix + "UnsupportedRequest")
    {
        return {"Feature not supported by device firmware", "Critical",
                "Expected on devices that do not implement this dump type.",
                false};
    }
    if (enumStr == prefix + "ResourceNotFound")
    {
        return {
            "Device not found in inventory", "Critical",
            "Confirm the target device name and that the device is enumerated.",
            false};
    }
    if (enumStr == prefix + "WriteFailure")
    {
        return {"Data transfer error (BMC could not write dump file)",
                "Critical",
                "Check available eMMC space and filesystem health, then retry.",
                false};
    }
    if (enumStr == prefix + "ConflictingOperation")
    {
        return {"Another dump/log/erase operation already in progress",
                "Warning",
                "Wait for the in-progress operation to complete and retry.",
                true};
    }
    if (enumStr == prefix + "InvalidArgument")
    {
        return {
            "Invalid request parameters", "Warning",
            "Likely a BMC software bug; capture nsmd journal and file a defect.",
            false};
    }
    if (enumStr == prefix + "InternalFailure")
    {
        return {
            "Internal failure", "Critical",
            "Capture nsmd journal (raw NSM cc/reason in com.nvidia.Async.Value.Value) and retry once.",
            true};
    }
    return {enumStr, "", "", false};
}

UnpackedNsmError unpackNsmError(uint64_t packed)
{
    UnpackedNsmError out{};
    // memcpy (not static_cast) to portably round-trip a negative swRc.
    // Must mirror nsm::unpackNsmError().
    const uint32_t swRcBits = static_cast<uint32_t>(packed >> 32);
    std::memcpy(&out.swRc, &swRcBits, sizeof(out.swRc));
    out.reasonCode = static_cast<uint16_t>((packed >> 16) & 0xFFFFU);
    out.cc = static_cast<uint8_t>((packed >> 8) & 0xFFU);
    return out;
}

bool getAsyncValue(const std::string& path, uint64_t& outValue)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    auto valueMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", path.c_str(),
                            "org.freedesktop.DBus.Properties", "Get");
    valueMethod.append("com.nvidia.Async.Value", "Value");
    try
    {
        auto reply = bus.call(valueMethod);
        std::variant<uint64_t> value;
        reply.read(value);
        outValue = std::get<uint64_t>(value);
        return true;
    }
    catch (const std::exception& e)
    {
        // Async.Value may be absent on objects that haven't started an
        // operation yet, or on older nsmd builds. Treat as soft-failure.
        log<level::DEBUG>(
            std::format("getAsyncValue failed for {}: {}", path, e.what())
                .c_str());
        return false;
    }
}

std::string renderAsyncFailureBlock(
    const std::string& asyncHandlePath, const std::string& targetDevice,
    const std::string& dataLabel, const std::string& asyncStatusEnum)
{
    const auto info = asyncStatusToHuman(asyncStatusEnum);

    std::string out = std::format("Getting {} data failed for {}: {}",
                                  dataLabel, targetDevice, info.text);

    if (!info.severity.empty())
    {
        out += std::format("\n  Severity:   {} | Retryable: {}", info.severity,
                           info.retryable ? "yes" : "no");
    }
    if (!info.resolution.empty())
    {
        out += std::format("\n  Resolution: {}", info.resolution);
    }

    uint64_t packed = 0;
    if (getAsyncValue(asyncHandlePath, packed) && packed != 0)
    {
        const auto raw = unpackNsmError(packed);
        out += std::format(
            "\n  NSM detail: swRc=0x{:x} cc=0x{:02x} reasonCode=0x{:04x}",
            static_cast<uint32_t>(raw.swRc), raw.cc, raw.reasonCode);
    }

    return out;
}
