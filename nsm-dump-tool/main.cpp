/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "nsm_dump_utils.hpp"

#include <fcntl.h>
#include <sys/stat.h> // for fstat
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

#define VERSION "3.1"
#define SLEEP_DURING_WAIT_SECONDS 1

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
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string interf;
    std::string method;
    interf = "org.freedesktop.DBus.Properties";
    method = "Get";

    auto commandStatusMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", path.c_str(),
                            interf.c_str(), method.c_str());
    commandStatusMethod.append("com.nvidia.Async.Status", "Status");

    try
    {
        auto statusReply = bus.call(commandStatusMethod);
        std::variant<std::string> status;
        statusReply.read(status);
        response = (std::get<std::string>(status));
        if (response == "com.nvidia.Async.Status.AsyncOperationStatus.Success")
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
        std::string errorStr("Function getAsyncStatus failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
    }

    return Error;
}

OperationStatus getEraseStatus(std::string objectPath)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
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
            {
                if ("com.nvidia.Dump.Erase.EraseStatus.DataEraseInProgress" ==
                    eraseStatus)
                {
                    return InProgress;
                }
                else
                {
                    if ("com.nvidia.Dump.Erase.EraseStatus.DataErased" ==
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
            }
        }
        else
        {
            log<level::ERR>(eraseStatus.c_str());
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

std::string getDBusObject(const std::string& targetDevice, DataType dataType,
                          DumpType dumpType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();

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

    auto reply = bus.call(mapper);

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
                auto statusReply = bus.call(GetDumpTypeMethod);
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
                std::string errorStr("Function getDBusObject failed");
                log<level::ERR>(errorStr.c_str());
                log<level::ERR>(e.what());
            }
        }
    }

    std::string errorStr("D-Bus path not found for ");
    errorStr += targetDevice;
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
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
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

    sdbusplus::message::object_path path;
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
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
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
        log<level::ERR>(errorStr.c_str());
        logMsg(errorStr);
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
<<<<<<< HEAD
        auto errorStr =
            std::format("Getting {} data failed for {} with status: {}",
                        dataType == DataType::Dump ? "dump" : "log",
                        targetDevice, response);
        log<level::ERR>(errorStr.c_str());
        logMsg(errorStr);
||||||| constructed merge base
        auto errorStr =
            std::format("Getting {} data failed for {} with status: {}",
                        dataType == DataType::Dump ? "dump" : "log",
                        targetDevice, response);
        throw std::runtime_error(errorStr);
=======
        // Multi-line execution-report entry: human-readable summary plus
        // severity / retry hint / resolution from the categorized status,
        // followed by the raw NSM cc/reason/swRc decoded from
        // com.nvidia.Async.Value.Value when available
        const char* dataLabel = dataType == DataType::Dump ? "dump" : "log";
        logMsg(
            renderAsyncFailureBlock(path, targetDevice, dataLabel, response));

        // The raw enum string also goes to journald so log-scraping
        // assertions stay machine-parseable.
        log<level::ERR>(std::format("nsm-dump-tool failure for {}: status={}",
                                    targetDevice, response)
                            .c_str());
>>>>>>> nsm-dump-tool: categorized dump failures and interface gating
    }
}

void dumpData(DumpType dumpType)
{
    auto objectPath = getDBusObject(targetDevice, DataType::Dump, dumpType);
    if (objectPath.empty())
    {
        throw std::runtime_error(
            "D-Bus path with DebugInfo interface not found for " +
            targetDevice);
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
        result = system(command.c_str());
        // NOLINTEND

        if (result != 0)
        {
            auto errorStr =
                std::format("Command failed with error code: {}", result);
            log<level::ERR>(errorStr.c_str());
        }

        std::filesystem::remove_all(tempDir);
    }

    return result;
}
