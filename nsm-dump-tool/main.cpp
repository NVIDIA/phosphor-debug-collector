/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#include "nsm_dump_utils.hpp"

#include <fcntl.h>
#include <sys/stat.h> // for fstat
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

#define VERSION "3.2"
#define SLEEP_DURING_WAIT_SECONDS 1

#ifndef NSM_DUMP_ASYNC_STATUS_DBUS_TIMEOUT_US
#define NSM_DUMP_ASYNC_STATUS_DBUS_TIMEOUT_US (120000000ULL) // 120 seconds
#endif

namespace
{
constexpr std::array<unsigned, 3> kAsyncStatusRetryBackoffSec{2, 4, 8};
constexpr size_t kAsyncStatusRetryPhases = kAsyncStatusRetryBackoffSec.size();

// The DebugInfo object only appears once nsmd has enumerated that device.
constexpr unsigned kDumpObjectWaitTimeoutSec = NSM_DUMP_OBJECT_WAIT_TIMEOUT_SEC;
constexpr unsigned kDumpObjectWaitIntervalSec =
    NSM_DUMP_OBJECT_WAIT_INTERVAL_SEC;
static_assert(kDumpObjectWaitIntervalSec >= 1,
              "poll interval must be at least 1 s");
static_assert(kDumpObjectWaitIntervalSec <= kDumpObjectWaitTimeoutSec,
              "poll interval must not exceed the wait timeout");
// Bound each call; the sd-bus default (25 s) would overrun the poll deadline.
constexpr uint64_t kDumpObjectLookupCallTimeoutUs = 5'000'000; // 5 s

bool isAsyncStatusDBusTimeout(
    const sdbusplus::exception::SdBusError& e) noexcept
{
    return (e.name() != nullptr && std::string_view(e.name()) ==
                                       "org.freedesktop.DBus.Error.Timeout") ||
           (e.get_errno() == ETIMEDOUT);
}
} // namespace

using namespace phosphor::logging;

std::string tempPath;
std::string targetDevice;

enum OperationStatus
{
    Success,
    InProgress,
    Error,
};

enum class DataType
{
    Dump,
    Log
};

// Dump type enumeration
enum class DumpType
{
    Network,
    Diagnostics,
    Unknown
};

// String to enum mapping
const std::map<std::string, DumpType> dumpTypeMap = {
    {"Network", DumpType::Network},
    {"Diagnostics", DumpType::Diagnostics},
};

// Helper function to get DumpType from string
DumpType getDumpType(const std::string& typeStr)
{
    auto it = dumpTypeMap.find(typeStr);
    return it != dumpTypeMap.end() ? it->second : DumpType::Unknown;
}

void logMsg(std::string msg)
{
    std::fstream log_file;
    log_file.open(tempPath + "/Execution_Report.txt", std::ios::app);
    if (log_file)
    {
        log_file << msg << std::endl;
        std::cout << msg << std::endl;
    }
    log_file.close();
}

OperationStatus getAsyncStatus(std::string path, std::string& response)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    const char* interf = "org.freedesktop.DBus.Properties";
    const char* method = "Get";

    for (size_t attempt = 0;; ++attempt)
    {
        auto commandStatusMethod = bus.new_method_call(
            "xyz.openbmc_project.NSM", path.c_str(), interf, method);
        commandStatusMethod.append("com.nvidia.Async.Status", "Status");

        try
        {
            auto statusReply = bus.call(commandStatusMethod,
                                        NSM_DUMP_ASYNC_STATUS_DBUS_TIMEOUT_US);
            std::variant<std::string> status;
            statusReply.read(status);
            response = (std::get<std::string>(status));
            if (response ==
                "com.nvidia.Async.Status.AsyncOperationStatus.Success")
            {
                return Success;
            }
            else if (response ==
                     "com.nvidia.Async.Status.AsyncOperationStatus.InProgress")
            {
                return InProgress;
            }
            else
            {
                log<level::ERR>(response.c_str());
                return Error;
            }
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            if (attempt < kAsyncStatusRetryPhases &&
                isAsyncStatusDBusTimeout(e))
            {
                log<level::WARNING>(
                    "getAsyncStatus: D-Bus timeout; retrying after backoff");
                sleep(kAsyncStatusRetryBackoffSec[attempt]);
                continue;
            }
            std::string errorStr("Function getAsyncStatus failed");
            log<level::ERR>(errorStr.c_str());
            log<level::ERR>(e.what());
            return Error;
        }
    }
}

OperationStatus getEraseStatus(std::string objectPath)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    auto eraseStatusMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            "org.freedesktop.DBus.Properties", "Get");
    eraseStatusMethod.append("com.nvidia.Dump.Erase", "EraseDebugInfoStatus");
    try
    {
        auto reply = bus.call(eraseStatusMethod);
        std::variant<std::tuple<std::string, std::string>> response;
        reply.read(response);
        std::tuple<std::string, std::string> eraseResponse(
            std::get<std::tuple<std::string, std::string>>(response));
        std::string eraseReason(std::get<0>(eraseResponse));
        std::string eraseStatus(std::get<1>(eraseResponse));
        if ("com.nvidia.Dump.Erase.OperationStatus.InProgress" == eraseReason)
        {
            return InProgress;
        }
        else if ("com.nvidia.Dump.Erase.OperationStatus.Success" == eraseReason)
        {
            if ("com.nvidia.Dump.Erase.EraseStatus.DataEraseInProgress" ==
                eraseStatus)
            {
                return InProgress;
            }
            // OperationStatus.Success pairs with two terminal EraseStatus
            // values that both mean the operation completed successfully:
            //   - DataErased: device flash was cleared.
            //   - NoDataErased: device had nothing to clear (e.g. the
            //     preceding GetDebugInfo already drained the FW saved-dump
            //     buffer). NSM publishes this state from
            //     nsmd/nsmDumpCollection/nsmEraseTrace.cpp on
            //     ERASE_TRACE_NO_DATA_ERASED.
            else if ("com.nvidia.Dump.Erase.EraseStatus.DataErased" ==
                         eraseStatus ||
                     "com.nvidia.Dump.Erase.EraseStatus.NoDataErased" ==
                         eraseStatus)
            {
                return Success;
            }
            else
            {
                log<level::ERR>(eraseStatus.c_str());
                return Error;
            }
        }
        else
        {
            log<level::ERR>(eraseReason.c_str());
            return Error;
        }
    }

    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function getEraseStatus failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }

    return Success;
}

// logIfMissing=false suppresses the per-attempt error logs for retrying
// callers; the reason comes back through outLastError instead.
std::string getDBusObject(const std::string& targetDevice, DataType dataType,
                          DumpType dumpType, bool logIfMissing = true,
                          std::string* outLastError = nullptr)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();

    std::string rootPath("/xyz/openbmc_project/inventory/system");

    std::vector<std::string> paths;

    auto mapper = bus.new_method_call(
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
    mapper.append(rootPath.c_str());
    mapper.append(0); // Depth 0 to search all
    switch (dataType)
    {
        case DataType::Dump:
            mapper.append(
                std::vector<std::string>({"com.nvidia.Dump.DebugInfo"}));
            break;
        case DataType::Log:
            mapper.append(
                std::vector<std::string>({"com.nvidia.Dump.LogInfo"}));
            break;
        default:
            std::string errorStr("Invalid data type in getDBusObject");
            log<level::ERR>(errorStr.c_str());
            break;
    }

    auto reply = bus.call(mapper, kDumpObjectLookupCallTimeoutUs);

    reply.read(paths);
    for (auto& path : paths)
    {
        if (path.find(targetDevice) != std::string::npos)
        {
            auto GetDumpTypeMethod =
                bus.new_method_call("xyz.openbmc_project.NSM", path.c_str(),
                                    "org.freedesktop.DBus.Properties", "Get");
            GetDumpTypeMethod.append("com.nvidia.Dump.DebugInfo",
                                     "SupportedDumpType");
            try
            {
                auto statusReply =
                    bus.call(GetDumpTypeMethod, kDumpObjectLookupCallTimeoutUs);
                std::variant<std::string> dumpTypeResponse;
                statusReply.read(dumpTypeResponse);
                std::string response(std::get<std::string>(dumpTypeResponse));
                switch (dumpType)
                {
                    case DumpType::Network:
                        if ("com.nvidia.Dump.DebugInfo.DumpType.Network" ==
                            response)
                        {
                            return path;
                        }
                        break;
                    case DumpType::Diagnostics:
                        if ("com.nvidia.Dump.DebugInfo.DumpType.Diagnostics" ==
                            response)
                        {
                            return path;
                        }
                        break;
                    default:
                        break;
                }
            }
            catch (const sdbusplus::exception::SdBusError& e)
            {
                if (outLastError != nullptr)
                {
                    *outLastError =
                        std::format("SupportedDumpType read failed on {}: {}",
                                    path, e.what());
                }
                if (logIfMissing)
                {
                    std::string errorStr("Function getDBusObject failed");
                    log<level::ERR>(errorStr.c_str());
                    log<level::ERR>(e.what());
                }
                else
                {
                    log<level::DEBUG>(
                        std::format("getDBusObject: SupportedDumpType read "
                                    "failed on {}: {}",
                                    path, e.what())
                            .c_str());
                }
            }
        }
    }

    if (logIfMissing)
    {
        std::string errorStr("D-Bus path not found for ");
        errorStr += targetDevice;
        log<level::ERR>(errorStr.c_str());
    }

    return {};
}

// An invalid targetDevice is indistinguishable from a not-yet-enumerated one,
// so it costs the full timeout before failing.
std::string waitForDBusObject(const std::string& targetDevice,
                              DataType dataType, DumpType dumpType)
{
    using std::chrono::seconds;
    using std::chrono::steady_clock;

    const auto start = steady_clock::now();
    const auto deadline = start + seconds(kDumpObjectWaitTimeoutSec);

    std::string lastError;
    bool waiting = false;

    for (;;)
    {
        try
        {
            auto objectPath = getDBusObject(targetDevice, dataType, dumpType,
                                            /*logIfMissing=*/false, &lastError);
            if (!objectPath.empty())
            {
                if (waiting)
                {
                    logMsg(std::format(
                        "Device {} became ready after {} s — proceeding",
                        targetDevice,
                        std::chrono::duration_cast<seconds>(
                            steady_clock::now() - start)
                            .count()));
                }
                return objectPath;
            }
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            // Unreachable mapper counts as not-ready-yet.
            lastError = e.what();
        }

        if (steady_clock::now() >= deadline)
        {
            break;
        }

        if (!waiting)
        {
            logMsg(std::format(
                "Device {} has no dump object yet (nsmd may still be enumerating it) — waiting up to {} s",
                targetDevice, kDumpObjectWaitTimeoutSec));
            waiting = true;
        }

        sleep(kDumpObjectWaitIntervalSec);
    }

    auto errorStr =
        std::format("D-Bus path not found for {} after waiting {} s",
                    targetDevice, kDumpObjectWaitTimeoutSec);
    if (!lastError.empty())
    {
        errorStr += std::format("; last D-Bus error: {}", lastError);
    }
    log<level::ERR>(errorStr.c_str());

    return {};
}

std::string generateTempFolderName(std::string dumpID)
{
    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);

    return std::format("obmcdump_{}_{}", dumpID, time_now);
}

std::string startAsyncDump(std::string objectPath, DataType dataType,
                           DumpType dumpType, sdbusplus::message::unix_fd fd)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    std::string interf;
    std::string method;

    switch (dataType)
    {
        case DataType::Dump:
            interf = "com.nvidia.Dump.DebugInfo";
            switch (dumpType)
            {
                case DumpType::Network:
                    method = "GetDebugInfo";
                    break;
                case DumpType::Diagnostics:
                    method = "GetDiagnostics";
                    break;
                default:
                    log<level::ERR>("Invalid dump type in startAsyncDump");
                    return "";
            }
            break;
        case DataType::Log:
            interf = "com.nvidia.Dump.LogInfo";
            method = "GetLogInfo";
            break;
        default:
            log<level::ERR>("Invalid data type in startAsyncDump");
            return "";
    }

    auto startAsyncDumpMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    if (dataType == DataType::Dump && dumpType == DumpType::Network)
    {
        startAsyncDumpMethod.append(
            "com.nvidia.Dump.DebugInfo.DebugInformationType.DeviceInformation");
    }
    startAsyncDumpMethod.append(fd);

    sdbusplus::object_path path;
    try
    {
        auto reply = bus.call(startAsyncDumpMethod);
        reply.read(path);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        throw std::runtime_error(
            std::format("Function startAsyncDump failed: {}", e.what()));
    }
    // NOLINTBEGIN
    return std::string(path);
    // NOLINTEND
}

void eraseDump(std::string objectPath)
{
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    auto sendCommandMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            "com.nvidia.Dump.Erase", "EraseDebugInfo");
    sendCommandMethod.append(
        "com.nvidia.Dump.Erase.EraseInfoType.FWSavedDumpInfo");
    try
    {
        auto reply = bus.call(sendCommandMethod);
        reply.read();
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        throw std::runtime_error(
            std::format("Function eraseDump failed: {}", e.what()));
    }

    OperationStatus status;
    do
    {
        sleep(SLEEP_DURING_WAIT_SECONDS);
        status = getEraseStatus(objectPath);
    } while (status == OperationStatus::InProgress);
    if (status != OperationStatus::Success)
    {
        auto errorStr = std::format("Erase failed for {}", objectPath);
        throw std::runtime_error(errorStr);
    }
}

void getDumpData(std::string objectPath, DataType dataType, DumpType dumpType)
{
    std::string outputFileName;
    switch (dataType)
    {
        case DataType::Dump:
            outputFileName = tempPath + "/" + targetDevice + "_dump.bin";
            break;
        case DataType::Log:
            outputFileName = tempPath + "/" + targetDevice + "_log.bin";
            break;
        default:
            throw std::runtime_error("Invalid data type in getDumpData");
    }
    int fd = open(outputFileName.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        throw std::runtime_error(
            "Failed to open output file: " + outputFileName);
    }
    auto path = startAsyncDump(objectPath, dataType, dumpType, fd);
    if (path.empty())
    {
        throw std::runtime_error(
            "Failed to start async dump for " + targetDevice);
    }

    OperationStatus status;
    std::string response;
    do
    {
        sleep(SLEEP_DURING_WAIT_SECONDS);
        status = getAsyncStatus(path, response);
    } while (status == OperationStatus::InProgress);
    // Get file size after dump operation
    struct stat fileStat;
    if (fstat(fd, &fileStat) != 0)
    {
        logMsg("Warning: Could not determine dump file size");
    }
    close(fd);

    if (status == OperationStatus::Success)
    {
        if (fileStat.st_size > 0)
        {
            logMsg(std::format(
                "Finished getting {} data for {} - file '{}' size: {} bytes",
                dataType == DataType::Dump ? "dump" : "log", targetDevice,
                outputFileName, fileStat.st_size));
        }
        else
        {
            logMsg(std::format("{} file '{}' is empty for {}",
                               dataType == DataType::Dump ? "Dump" : "Log",
                               outputFileName, targetDevice));
        }
    }
    else
    {
        // Multi-line execution-report entry: human-readable summary plus
        // severity / retry hint / resolution from the categorized status,
        // followed by the raw NSM cc/reason/swRc decoded from
        // com.nvidia.Async.Value.Value when available
        const char* dataLabel = dataType == DataType::Dump ? "dump" : "log";
        logMsg(
            renderAsyncFailureBlock(path, targetDevice, dataLabel, response));

        // The raw enum string also goes to journald so log-scraping
        // assertions stay machine-parseable.
        auto errorStr = std::format("nsm-dump-tool failure for {}: status={}",
                                    targetDevice, response);
        log<level::ERR>(errorStr.c_str());
        throw std::runtime_error(errorStr);
    }
}

void dumpData(DumpType dumpType)
{
    // DebugInfo is mandatory for every dump, so wait rather than fail outright.
    auto objectPath = waitForDBusObject(targetDevice, DataType::Dump, dumpType);
    if (objectPath.empty())
    {
        throw std::runtime_error(std::format(
            "D-Bus path with DebugInfo interface not found for {} after waiting {} s — device not ready",
            targetDevice, kDumpObjectWaitTimeoutSec));
    }

    logMsg(std::format("Starting getting dump data for target device: {}",
                       targetDevice));
    // Get the dump data for Network or Diagnostics
    getDumpData(objectPath, DataType::Dump, dumpType);

    if (dumpType == DumpType::Network)
    {
        const auto eraseDumpPath = objectPath;
        const bool eraseSupportedOnDumpPath =
            deviceHasInterface(targetDevice, "com.nvidia.Dump.Erase");
        if (eraseSupportedOnDumpPath)
        {
            eraseDump(eraseDumpPath);
        }
        else
        {
            logMsg(std::format(
                "Erase not supported on device {} — skipping (com.nvidia.Dump.Erase not exposed)",
                targetDevice));
        }

        // LogInfo is optional. Pre-check the interface before getDBusObject()
        // so an unsupported device skips cleanly instead of getDBusObject()
        // emitting an error-level "path not found" log for the expected case.
        if (!deviceHasInterface(targetDevice, "com.nvidia.Dump.LogInfo"))
        {
            logMsg(std::format(
                "LogInfo not supported on device {} — skipping (com.nvidia.Dump.LogInfo not exposed)",
                targetDevice));
            return;
        }

        objectPath = getDBusObject(targetDevice, DataType::Log, dumpType);
        if (objectPath.empty())
        {
            logMsg(std::format(
                "LogInfo not supported on device {} — skipping (com.nvidia.Dump.LogInfo not exposed)",
                targetDevice));
            return;
        }

        logMsg(std::format("Starting getting log data for target device: {}",
                           targetDevice));
        getDumpData(objectPath, DataType::Log, dumpType);
        // Erase on the log object only when THAT exact path exposes
        // com.nvidia.Dump.Erase.
        if (objectPath != eraseDumpPath &&
            objectPathHasInterface(objectPath, "com.nvidia.Dump.Erase"))
        {
            eraseDump(objectPath);
        }
    }
}

int main(int argc, char** argv)
{
    int result = 0;
    if (argc <= 7)
    {
        // Print version and usage
        printf("nsm-dump-tool version " VERSION "\n");
        printf(
            "Usage: nsm-dump-tool -p <file_path> -i <dump_id> -t <temp_path> -d <target_device> -o <dump_type>\n");
    }
    else
    {
        // Parse command line arguments
        std::string dumpPath = argv[2];
        std::string dumpID = argv[4];
        tempPath = argv[6];
        targetDevice = argv[8];

        auto dumpType = getDumpType(argv[10]);
        if (dumpType == DumpType::Unknown)
        {
            logMsg(std::format("Dump Type {} not found", argv[10]));
            // Return non-zero exit code to indicate error
            return 1;
        }

        using std::chrono::duration_cast;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;
        auto t1 = high_resolution_clock::now();
        std::string tempFolderName = generateTempFolderName(dumpID);
        std::string tempDir;

        switch (dumpType)
        {
            case DumpType::Network:
                tempDir = tempPath + "/NetIR_dump/";
                break;
            case DumpType::Diagnostics:
                tempDir = tempPath + "/Diagnostics_dump/";
                break;
            default:
                throw std::runtime_error("Invalid dump type in dumpData");
        }

        tempPath = tempDir + tempFolderName;
        if (!std::filesystem::exists(tempPath))
        {
            std::filesystem::create_directories(tempPath);
        }

        if (!std::filesystem::exists(dumpPath))
        {
            std::filesystem::create_directories(dumpPath);
        }

        try
        {
            dumpData(dumpType);
        }
        catch (const std::exception& e)
        {
            logMsg(e.what());
            log<level::ERR>(e.what());
            std::error_code ec;
            std::filesystem::remove_all(tempPath, ec);
            std::filesystem::remove(dumpPath, ec);
            return 1;
        }

        auto t2 = high_resolution_clock::now();
        auto ms_int = duration_cast<milliseconds>(t2 - t1);
        int msecs = static_cast<int>(ms_int.count());
        int hours = msecs / (60 * 60 * 1000);
        msecs -= hours * (60 * 60 * 1000);
        int mins = msecs / (60 * 1000);
        msecs -= mins * (60 * 1000);
        int seconds = msecs / 1000;
        msecs -= (seconds * 1000);

        logMsg(std::format(
            "Execution time: {} hours, {} minutes, {} seconds, {} milliseconds",
            hours, mins, seconds, msecs));

        std::string command = "tar -Jcf " + dumpPath + '/' + tempFolderName +
                              ".tar.xz -C " + tempDir + " " + tempFolderName;

        logMsg(std::format("Compressing dump to `{}`",
                           dumpPath + '/' + tempFolderName + ".tar.xz"));
        // NOLINTBEGIN
        int waitStatus = system(command.c_str());
        // NOLINTEND

        // system() returns a "wait status", not an exit code; examine it with
        // the macros described in waitpid(2).
        if (waitStatus == -1)
        {
            // fork/exec failed; the command never ran
            result = 1;
        }
        else
        {
            result = WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 1;
        }

        if (result != 0)
        {
            auto errorStr =
                std::format("Command failed with error code: {}", result);
            log<level::ERR>(errorStr.c_str());
        }

        std::filesystem::remove_all(tempPath);
    }

    return result;
}
