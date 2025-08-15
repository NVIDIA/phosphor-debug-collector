/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <sys/stat.h> // for fstat
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#define VERSION "3.0"
#define SLEEP_DURING_WAIT_SECONDS 1

using namespace phosphor::logging;

std::string tempPath;
std::string targetDevice;

enum OperationStatus
{
    Success,
    InProgress,
    NoDataErased,
    Error,
};

enum class DataType
{
    Dump,
    Log,
    SavedInfo
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
    std::string interf, method;
    interf = "org.freedesktop.DBus.Properties";
    method = "Get";

    auto commandStatusMethod = bus.new_method_call("xyz.openbmc_project.NSM",
                                                   path.c_str(), interf.c_str(),
                                                   method.c_str());
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

        if ("com.nvidia.Dump.Erase.OperationStatus.Success" == eraseReason)
        {
            if ("com.nvidia.Dump.Erase.OperationStatus.Success" == eraseReason)
            {
                if ("com.nvidia.Dump.Erase.EraseStatus.DataEraseInProgress" ==
                    eraseStatus)
                {
                    return InProgress;
                }

                if ("com.nvidia.Dump.Erase.EraseStatus.DataErased" ==
                    eraseStatus)
                {
                    return Success;
                }

                if ("com.nvidia.Dump.Erase.EraseStatus.NoDataErased" ==
                    eraseStatus)
                {
                    return NoDataErased;
                }

                log<level::ERR>(eraseStatus.c_str());
                return Error;
            }
        }

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

    auto mapper = bus.new_method_call("xyz.openbmc_project.ObjectMapper",
                                      "/xyz/openbmc_project/object_mapper",
                                      "xyz.openbmc_project.ObjectMapper",
                                      "GetSubTreePaths");
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

    // Use ctime_r for thread safety
    struct tm time_info;
    char time_string[26]; // Buffer for ctime_r output
    ctime_r(&time_now, static_cast<char*>(time_string));

    // Parse time string (format: "Day Mon DD HH:MM:SS YYYY\n")
    strptime(static_cast<const char*>(time_string), "%a %b %d %H:%M:%S %Y",
             &time_info);

    sprintf(static_cast<char*>(time_string), "_%02d%02d%02d%02d%02d",
            time_info.tm_mon + 1, time_info.tm_mday, time_info.tm_hour,
            time_info.tm_min, time_info.tm_sec);

    std::string folderName = std::format("{}{}{}", "obmcdump_", dumpID,
                                         time_string);

    return folderName;
}

std::string startAsyncDump(std::string objectPath, DataType dataType,
                           DumpType dumpType, sdbusplus::message::unix_fd fd)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string interf, method;

    switch (dataType)
    {
        case DataType::Dump:
        case DataType::SavedInfo:
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
    if (dataType == DataType::SavedInfo)
    {
        startAsyncDumpMethod.append(
            "com.nvidia.Dump.DebugInfo.DebugInformationType.FWSavedInfo");
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
        std::string errorStr("Function startAsyncDump failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return "";
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
        std::string errorStr("Function eraseDump failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return;
    }

    OperationStatus status;
    do
    {
        sleep(SLEEP_DURING_WAIT_SECONDS);
        status = getEraseStatus(objectPath);
    } while (status == OperationStatus::InProgress);
    if (status == OperationStatus::Success)
    {
        logMsg("Data erased successfully");
        return;
    }
    if (status == OperationStatus::NoDataErased)
    {
        logMsg("No Data to erase");
        return;
    }
    auto errorStr = std::format("Erase failed for {}", objectPath);
    log<level::ERR>(errorStr.c_str());
    logMsg("Data erase command completed with errors");
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
        case DataType::SavedInfo:
            outputFileName = tempPath + "/" + targetDevice + "_saved_info.bin";
            break;
        default:
            logMsg(std::format("Invalid data type"));
            return;
    }
    int fd = open(outputFileName.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        logMsg(std::format("Failed to open output file: {}", outputFileName));
        return;
    }
    auto path = startAsyncDump(objectPath, dataType, dumpType, fd);
    if (path.empty())
    {
        logMsg(std::format("Failed to start async dump for: {}", targetDevice));
        return;
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
            // Logs are optional, and they might not exist, so we declare that
            // we started collecting data only if completion was successful.
            if (dataType == DataType::Log)
            {
                logMsg(std::format("Started to get the {} log", targetDevice));
            }
            // Target device may not implement saved info data, so we declare
            // that we started collecting data only if completion was
            // successful.
            if (dataType == DataType::SavedInfo)
            {
                logMsg(std::format("Started to get the {} saved info dump",
                                   targetDevice));
            }
            logMsg(std::format("Output file size: {} bytes", fileStat.st_size));
            logMsg(std::format("Getting the {} {} completed successfully",
                               targetDevice,
                               dataType == DataType::Log ? "log" : "dump"));
            if (dataType == DataType::SavedInfo)
            {
                logMsg(std::format("Started to erase saved data"));
                eraseDump(objectPath);
            }
        }
        else
        {
            // Logs are optional, and they might not exist, so we don't issue
            // errors for other than 'Dump' types
            if (dataType == DataType::Dump)
            {
                logMsg(std::format("Dump file is empty for {}", targetDevice));
            }
            std::filesystem::remove(outputFileName);
        }
    }
    else
    {
        auto errorStr =
            std::format("Getting {} data failed for {} with status: {}",
                        dataType == DataType::Dump ? "dump" : "log",
                        targetDevice, response);
        log<level::ERR>(errorStr.c_str());
        // Logs are optional, and they might not exist, so we don't issue
        // errors for other than 'Dump' types
        if (dataType == DataType::Dump)
        {
            logMsg(std::format("Getting the {} dump completed with errors",
                               targetDevice));
        }
        if (fileStat.st_size == 0)
        {
            std::filesystem::remove(outputFileName);
        }
    }
}

void dumpData(DumpType dumpType)
{
    switch (dumpType)
    {
        case DumpType::Network:
        case DumpType::Diagnostics:
            break;
        default:
            logMsg(std::format("Invalid dump type"));
            return;
    }
    auto objectPath = getDBusObject(targetDevice, DataType::Dump, dumpType);
    if (objectPath.empty())
    {
        logMsg(
            std::format("D-Bus path with DebugInfo interface not found for {}",
                        targetDevice));
        return;
    }

    logMsg(objectPath);
    logMsg(std::format("Started to get the {} debug dump", targetDevice));
    // Get the dump data for Network or Diagnostics
    getDumpData(objectPath, DataType::Dump, dumpType);

    if (dumpType == DumpType::Network)
    {
        getDumpData(objectPath, DataType::SavedInfo, dumpType);
        objectPath = getDBusObject(targetDevice, DataType::Log, dumpType);
        if (objectPath.empty())
        {
            logMsg(std::format(
                "D-Bus path with LogInfo interface not found for {}",
                targetDevice));
            return;
        }
        getDumpData(objectPath, DataType::Log, dumpType);
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
                // Let it continue so that the dump file is created. Will fail
                // in the dumpData function later
                std::string errorStr("Invalid dump type: ");
                errorStr += argv[10];
                log<level::ERR>(errorStr.c_str());
                tempDir = tempPath + "/NetIR_dump/";
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

        logMsg(targetDevice);

        try
        {
            dumpData(dumpType);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(e.what());
            // Don't return so that the dump file is created
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

        auto infoStr = std::format("Compressing dump to {}",
                                   dumpPath + '/' + tempFolderName + ".tar.xz");
        log<level::INFO>(infoStr.c_str());
        // NOLINTBEGIN
        result = system(command.c_str());
        // NOLINTEND
        log<level::INFO>("Done.");

        if (result != 0)
        {
            auto errorStr = std::format("Command failed with error code: {}",
                                        result);
            log<level::ERR>(errorStr.c_str());
        }

        log<level::INFO>("Cleaning temp folder");
        std::filesystem::remove_all(tempDir);
        log<level::INFO>("Done.");
    }

    // Cannot return non-zero errors as Redfish will still deem it successful
    // but yet it wont show in the entries path
    return 0;
}
