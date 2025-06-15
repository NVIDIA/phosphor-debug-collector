/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#define VERSION "2.2"
#define MAX_IN_PROGRESS_COUNT 1000
#define MAX_ERROR_COUNT 3
#define SLEEP_DURING_WAIT 20

using namespace std;
using namespace phosphor::logging;

std::string tempPath;
std::string objectPath;
std::string targetDevice;
std::string outputFileName;
uint32_t outputFileSize;

enum OperationStatus
{
    Success,
    InProgress,
    Error,
};

enum class DataType
{
    Dump,
    Log,
    Diagnostics,
};

// Dump type enumeration
enum class DumpType
{
    Network,
    Diagnostics,
    Unknown
};

// String to enum mapping
const std::unordered_map<std::string, DumpType> dumpTypeMap = {
    {"Network", DumpType::Network}, {"Diagnostics", DumpType::Diagnostics}};

// Helper function to get DumpType from string
DumpType getDumpType(const std::string& typeStr)
{
    auto it = dumpTypeMap.find(typeStr);
    return it != dumpTypeMap.end() ? it->second : DumpType::Unknown;
}

void log_msg(std::string msg)
{
    fstream log_file;
    log_file.open(tempPath + "/Execution_Report.txt", ios::app);
    if (log_file)
    {
        log_file << msg << std::endl;
    }
    log_file.close();
}

uint8_t sendRequestRecordCommand(uint64_t nextRecord, DataType dataType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string interf, method;

    switch (dataType)
    {
        case DataType::Dump:
        case DataType::Diagnostics:
            interf = "com.nvidia.Dump.DebugInfo";
            method = "GetDebugInfo";
            break;
        case DataType::Log:
            interf = "com.nvidia.Dump.LogInfo";
            method = "GetLogInfo";
            break;
        default:
            interf = "com.nvidia.Dump.DebugInfo";
            method = "GetDebugInfo";
            break;
    }

    auto sendCommandMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    switch (dataType)
    {
        case DataType::Dump:
            sendCommandMethod.append("com.nvidia.Dump.DebugInfo."
                                     "DebugInformationType.DeviceInformation",
                                     nextRecord);
            break;
        case DataType::Diagnostics:
            sendCommandMethod.append("com.nvidia.Dump.DebugInfo."
                                     "DebugInformationType.DeviceDump",
                                     nextRecord);
            break;
        case DataType::Log:
            sendCommandMethod.append(nextRecord);
            break;
        default:
            sendCommandMethod.append("com.nvidia.Dump.DebugInfo."
                                     "DebugInformationType.DeviceInformation",
                                     nextRecord);
            break;
    }

    try
    {
        auto reply = bus.call(sendCommandMethod);
        reply.read();
    }

    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function sendRequestRecordCommand failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }

    return Success;
}

uint8_t getRequestRecordCommandStatus(DataType dataType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string interf, method;
    interf = "org.freedesktop.DBus.Properties";
    method = "Get";

    auto commandStatusMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    switch (dataType)
    {
        case DataType::Dump:
        case DataType::Diagnostics:
            commandStatusMethod.append("com.nvidia.Dump.DebugInfo", "Status");
            break;
        case DataType::Log:
            commandStatusMethod.append("com.nvidia.Dump.LogInfo", "Status");
            break;
        default:
            commandStatusMethod.append("com.nvidia.Dump.DebugInfo", "Status");
            break;
    }

    try
    {
        auto statusReply = bus.call(commandStatusMethod);
        std::variant<std::string> status;
        statusReply.read(status);
        std::string response(std::get<std::string>(status));
        if (DataType::Log == dataType)
        {
            if (response == "com.nvidia.Dump.LogInfo.OperationStatus.Success")
            {
                return Success;
            }
            else if (response ==
                     "com.nvidia.Dump.LogInfo.OperationStatus.InProgress")
            {
                return InProgress;
            }
            else
            {
                log<level::ERR>(response.c_str());
                return Error;
            }
        }
        else
        {
            if (response == "com.nvidia.Dump.DebugInfo.OperationStatus.Success")
            {
                return Success;
            }
            else if (response ==
                     "com.nvidia.Dump.DebugInfo.OperationStatus.InProgress")
            {
                return InProgress;
            }
            else
            {
                log<level::ERR>(response.c_str());
                return Error;
            }
        }
    }

    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function getRequestRecordCommandStatus failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
    }

    return Error;
}

uint64_t getNextRecord(DataType dataType)
{
    uint64_t nextRecord = 0;
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string interf, method;
    interf = "org.freedesktop.DBus.Properties";
    method = "Get";

    auto getNextRecord = bus.new_method_call("xyz.openbmc_project.NSM",
                                             objectPath.c_str(), interf.c_str(),
                                             method.c_str());

    switch (dataType)
    {
        case DataType::Dump:
        case DataType::Diagnostics:
            getNextRecord.append("com.nvidia.Dump.DebugInfo",
                                 "NextRecordHandle");
            break;
        case DataType::Log:
            getNextRecord.append("com.nvidia.Dump.LogInfo", "NextRecordHandle");
            break;
        default:
            getNextRecord.append("com.nvidia.Dump.DebugInfo",
                                 "NextRecordHandle");
            break;
    }

    try
    {
        auto reply = bus.call(getNextRecord);
        std::variant<uint64_t> NextRecordHandle;
        reply.read(NextRecordHandle);
        nextRecord = std::get<uint64_t>(NextRecordHandle);
    }

    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function getNextRecord failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return 0;
    }

    return nextRecord;
}

uint8_t saveRecord(DataType dataType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string interf, method;

    interf = "org.freedesktop.DBus.Properties";
    method = "Get";

    auto getFdHandleMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    switch (dataType)
    {
        case DataType::Dump:
        case DataType::Diagnostics:
            getFdHandleMethod.append("com.nvidia.Dump.DebugInfo", "Fd");
            break;
        case DataType::Log:
            getFdHandleMethod.append("com.nvidia.Dump.LogInfo", "Fd");
            break;
        default:
            getFdHandleMethod.append("com.nvidia.Dump.DebugInfo", "Fd");
            break;
    }

    try
    {
        auto reply = bus.call(getFdHandleMethod);
        std::variant<sdbusplus::message::unix_fd> response;
        reply.read(response);
        sdbusplus::message::unix_fd responseFd(
            std::get<sdbusplus::message::unix_fd>(response));

        char buffer[4096];
        ssize_t bytesRead;
        int fd = static_cast<int>(responseFd);

        fstream outputStream;
        outputStream.open(outputFileName, ios::app | ios::binary);

        while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0)
        {
            outputFileSize += bytesRead;
            outputStream.write(buffer, bytesRead);
        }
        outputStream.close();
    }

    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function saveRecord failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }

    return Success;
}

uint8_t sendEraseCommand()
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
        std::string errorStr("Function sendEraseCommand failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }
    return Success;
}

uint8_t getEraseStatus()
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

void getGpuDeviceDiagnosticsData(DataType dataType)
{
    std::string statusStr;
    uint64_t currentRecord = 0;
    uint64_t segmentsCounter = 0;
    uint8_t errorCounter = 0;
    uint16_t busyCounter = 0;
    uint8_t res;
    outputFileSize = 0;

    outputFileName = tempPath + "/" + targetDevice + "_dump.bin";
    statusStr = "Started to get the " + targetDevice + " dump";
    log_msg(statusStr);
    do
    {
        res = InProgress;
        errorCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT && res != Success)
        {
            res = sendRequestRecordCommand(currentRecord, dataType);
            if (res != Success)
            {
                sleep(SLEEP_DURING_WAIT);
                errorCounter++;
            }
        }
        if (res != Success)
        {
            break;
        }
        res = InProgress;
        errorCounter = 0;
        busyCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT &&
               busyCounter < MAX_IN_PROGRESS_COUNT && res != Success)
        {
            res = getRequestRecordCommandStatus(dataType);
            errorCounter += (res == Error);
            busyCounter += (res == InProgress);
        }
        res = InProgress;
        statusStr = "Getting the " + targetDevice + " dump";
        if (MAX_ERROR_COUNT == errorCounter)
        {
            statusStr += " reported errors";
            log_msg(statusStr);
            break;
        }
        if (MAX_IN_PROGRESS_COUNT == busyCounter)
        {
            statusStr += " timeout";
            log_msg(statusStr);
            break;
        }
        if (saveRecord(dataType))
        {
            statusStr = "Saving the " + targetDevice + " dump reported errors";
            log_msg(statusStr);
            break;
        }
        res = Success;
        segmentsCounter++;
        currentRecord = getNextRecord(dataType);
    } while (currentRecord != 0xFF);
    statusStr = "Total number of segments: " + std::to_string(segmentsCounter);
    log_msg(statusStr);
    statusStr = "Output file size: " + std::to_string(outputFileSize);
    log_msg(statusStr);
    if (res != Success)
    {
        statusStr = "Getting the " + targetDevice +
                    " dump completed with errors";
        log_msg(statusStr);
    }
    else
    {
        statusStr = "Getting the " + targetDevice +
                    " dump completed successfully";
        log_msg(statusStr);
    }
    return;
}

void getNetIRData(DataType dataType)
{
    std::string statusStr;
    uint64_t currentRecord = 0;
    uint64_t segmentsCounter = 0;
    uint8_t errorCounter = 0;
    uint16_t busyCounter = 0;
    uint8_t res;
    outputFileSize = 0;

    switch (dataType)
    {
        case DataType::Dump:
            outputFileName = tempPath + "/" + targetDevice + "_dump.bin";
            statusStr = "Started to get the " + targetDevice + " dump";
            log_msg(statusStr);
            break;
        case DataType::Log:
            outputFileName = tempPath + "/" + targetDevice + "_log.bin";
            break;
        default:
            std::string errorStr("Invalid data type in getDumpData");
            log<level::ERR>(errorStr.c_str());
            break;
    }
    do
    {
        res = InProgress;
        errorCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT && res != Success)
        {
            res = sendRequestRecordCommand(currentRecord, dataType);
            if (res != Success)
            {
                sleep(SLEEP_DURING_WAIT);
                errorCounter++;
            }
        }
        if (res != Success)
        {
            break;
        }
        res = InProgress;
        errorCounter = 0;
        busyCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT &&
               busyCounter < MAX_IN_PROGRESS_COUNT && res != Success)
        {
            res = getRequestRecordCommandStatus(dataType);
            errorCounter += (res == Error);
            busyCounter += (res == InProgress);
        }
        res = InProgress;
        if (MAX_ERROR_COUNT == errorCounter)
        {
            break;
        }
        if (MAX_IN_PROGRESS_COUNT == busyCounter)
        {
            break;
        }
        if (saveRecord(dataType))
        {
            break;
        }
        res = Success;
        segmentsCounter++;
        currentRecord = getNextRecord(dataType);
    } while (currentRecord != 0);
    if (res != Success)
    {
        switch (dataType)
        {
            case DataType::Dump:
                statusStr = "Getting the " + targetDevice +
                            " dump completed with errors";
                log_msg(statusStr);
                break;
            case DataType::Log:
                break;
            default:
                std::string errorStr("Invalid data type in getNetIRData");
                log<level::ERR>(errorStr.c_str());
                break;
        }
    }
    else
    {
        switch (dataType)
        {
            case DataType::Dump:
                statusStr = "Getting the " + targetDevice +
                            " dump completed successfully";
                log_msg(statusStr);
                statusStr = "Total number of segments: " +
                            std::to_string(segmentsCounter);
                log_msg(statusStr);
                statusStr = "Output file size: " +
                            std::to_string(outputFileSize);
                log_msg(statusStr);
                res = InProgress;
                errorCounter = 0;
                busyCounter = 0;
                res = sendEraseCommand();
                if (Success == res)
                {
                    do
                    {
                        res = getEraseStatus();
                        errorCounter += (Error == res);
                        busyCounter += (InProgress == res);
                    } while (errorCounter < MAX_ERROR_COUNT &&
                             busyCounter < MAX_IN_PROGRESS_COUNT &&
                             res != Success);
                }
                if (res == Success)
                {
                    statusStr = "Started to erase the " + targetDevice +
                                " dump contents";
                    log_msg(statusStr);
                    log_msg("Done.");
                }
                break;
            case DataType::Log:
                if (outputFileSize != 0)
                {
                    statusStr = "Started to get the " + targetDevice + " log";
                    log_msg(statusStr);
                    statusStr = "Getting the " + targetDevice +
                                " log completed successfully";
                    log_msg(statusStr);
                    statusStr = "Total number of segments: " +
                                std::to_string(segmentsCounter);
                    log_msg(statusStr);
                    statusStr = "Output file size: " +
                                std::to_string(outputFileSize);
                    log_msg(statusStr);
                }
                else
                {
                    std::filesystem::remove(outputFileName);
                }
                break;
            default:
                std::string errorStr("Invalid data type in getNetIRData");
                log<level::ERR>(errorStr.c_str());
                break;
        }
    }
    return;
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
        case DataType::Diagnostics:
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
    ctime_r(&time_now, time_string);

    // Parse time string (format: "Day Mon DD HH:MM:SS YYYY\n")
    strptime(time_string, "%a %b %d %H:%M:%S %Y", &time_info);

    sprintf(time_string, "_%02d%02d%02d%02d%02d", time_info.tm_mon + 1,
            time_info.tm_mday, time_info.tm_hour, time_info.tm_min,
            time_info.tm_sec);

    std::string folderName = std::format("{}{}{}", "obmcdump_", dumpID,
                                         time_string);

    return folderName;
}

int main(int argc, char** argv)
{
    if (argc > 7)
    {
        std::string dumpPath = argv[2];
        std::string dumpID = argv[4];
        tempPath = argv[6];
        targetDevice = argv[8];

        using std::chrono::duration_cast;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;

        auto t1 = high_resolution_clock::now();

        auto dumpType = getDumpType(argv[10]);

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

        log_msg(targetDevice);

        switch (dumpType)
        {
            case DumpType::Network:
                objectPath = getDBusObject(targetDevice, DataType::Dump,
                                           dumpType);

                if ("" == objectPath)
                {
                    std::string errorStr(
                        "D-Bus path with DebugInfo interface not found for ");
                    errorStr += targetDevice;
                    log_msg(errorStr);
                }
                else
                {
                    log_msg(objectPath);
                    getNetIRData(DataType::Dump);
                }

                objectPath = getDBusObject(targetDevice, DataType::Log,
                                           dumpType);

                if ("" == objectPath)
                {
                    std::string errorStr(
                        "D-Bus path with LogInfo interface not found for ");
                    errorStr += targetDevice;
                    log_msg(errorStr);
                }
                else
                {
                    getNetIRData(DataType::Log);
                }
                break;
            case DumpType::Diagnostics:
                objectPath = getDBusObject(targetDevice, DataType::Dump,
                                           dumpType);
                if ("" == objectPath)
                {
                    std::string errorStr(
                        "D-Bus path with DebugInfo interface not found for ");
                    errorStr += targetDevice;
                    log_msg(errorStr);
                }
                else
                {
                    log_msg(objectPath);
                    getGpuDeviceDiagnosticsData(DataType::Diagnostics);
                }
                break;
            default:
                std::string errorStr = std::format("{}{}{}", "Dump Type ",
                                                   argv[10], " not found");
                log_msg(errorStr);
                break;
        }

        auto t2 = high_resolution_clock::now();
        auto ms_int = duration_cast<milliseconds>(t2 - t1);
        int msecs = ms_int.count();
        int hours = msecs / (60 * 60 * 1000);
        msecs -= hours * (60 * 60 * 1000);
        int mins = msecs / (60 * 1000);
        msecs -= mins * (60 * 1000);
        int seconds = msecs / 1000;
        msecs -= (seconds * 1000);

        std::string executionTime =
            "Execution time: " + std::to_string(hours) + " hours, " +
            std::to_string(mins) + " minutes, " + std::to_string(seconds) +
            " seconds, " + std::to_string(msecs) + " milliseconds";
        log_msg(executionTime);

        std::string command = "tar -Jcf " + dumpPath + '/' + tempFolderName +
                              ".tar.xz -C " + tempDir + " " + tempFolderName;

        int result = system(command.c_str());

        if (result != 0)
        {
            std::string errorStr("Command failed with error code: ");
            errorStr += std::to_string(result);
            log<level::ERR>(errorStr.c_str());
        }

        std::filesystem::remove_all(tempDir);
    }
    else
    {
        printf("nsm-dump-tool version " VERSION "\n");
        printf(
            "Usage: nsm-dump-tool -p <file_path> -i <dump_id> -t <temp_path> -d <target_device> -o <dump_type>\n");
    }

    return 0;
}
