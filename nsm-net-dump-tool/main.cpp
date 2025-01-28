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

#define VERSION "1.0"
#define MAX_IN_PROGRESS_COUNT 1000
#define MAX_ERROR_COUNT 3
#define SLEEP_DURING_WAIT 20

using namespace std;
using namespace phosphor::logging;

std::string dumpPath;
std::string targetDevice;
std::string outputFileName;
uint32_t outputFileSize;

enum OperationStatus
{
    Success,
    InProgress,
    Error,
};

enum class DeviceTypeData
{
    NVSwitch,
    NVLinkMgmtNIC_Dump,
    NVLinkMgmtNIC_Log,
    GPU_SXM,
};

void log_msg(std::string msg)
{
    fstream log_file;
    log_file.open(dumpPath + "/Execution_Report.txt", ios::app);
    if (log_file)
    {
        log_file << msg << std::endl;
    }
    log_file.close();
}

uint8_t sendRequestRecordCommand(uint8_t index, uint64_t nextRecord,
                                 DeviceTypeData dataType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string objectPath, interf, method;

    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                         "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                         std::to_string(index);
            interf = "com.nvidia.Dump.DebugInfo";
            method = "GetDebugInfo";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "com.nvidia.Dump.DebugInfo";
            method = "GetDebugInfo";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "com.nvidia.Dump.LogInfo";
            method = "GetLogInfo";
            break;
        case DeviceTypeData::GPU_SXM:
            objectPath = "/xyz/openbmc_project/inventory/system/"
                         "processors/GPU_SXM_" +
                         std::to_string(index);
            interf = "com.nvidia.Dump.DebugInfo";
            method = "GetDebugInfo";
            break;
        default:
            std::string errorStr(
                "Invalid data type in sendRequestRecordCommand");
            log<level::ERR>(errorStr.c_str());
            return Error;
            break;
    }

    auto sendCommandMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            sendCommandMethod.append("com.nvidia.Dump.DebugInfo."
                                     "DebugInformationType.DeviceInformation",
                                     nextRecord);
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            sendCommandMethod.append("com.nvidia.Dump.DebugInfo."
                                     "DebugInformationType.DeviceInformation",
                                     nextRecord);
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            sendCommandMethod.append(nextRecord);
            break;
        case DeviceTypeData::GPU_SXM:
            sendCommandMethod.append("com.nvidia.Dump.DebugInfo."
                                     "DebugInformationType.DeviceInformation",
                                     nextRecord);
            break;
        default:
            std::string errorStr(
                "Invalid data type in sendRequestRecordCommand");
            log<level::ERR>(errorStr.c_str());
            return Error;
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

uint8_t getRequestRecordCommandStatus(uint8_t index, DeviceTypeData dataType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string objectPath, interf, method;
    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                         "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                         std::to_string(index);
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::GPU_SXM:
            objectPath = "/xyz/openbmc_project/inventory/system/"
                         "processors/GPU_SXM_" +
                         std::to_string(index);
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        default:
            std::string errorStr(
                "Invalid data type in getRequestRecordCommandStatus");
            log<level::ERR>(errorStr.c_str());
            return Error;
            break;
    }

    auto commandStatusMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            commandStatusMethod.append("com.nvidia.Dump.DebugInfo", "Status");
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            commandStatusMethod.append("com.nvidia.Dump.DebugInfo", "Status");
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            commandStatusMethod.append("com.nvidia.Dump.LogInfo", "Status");
            break;
        case DeviceTypeData::GPU_SXM:
            commandStatusMethod.append("com.nvidia.Dump.DebugInfo", "Status");
            break;
        default:
            std::string errorStr(
                "Invalid data type in getRequestRecordCommandStatus");
            log<level::ERR>(errorStr.c_str());
            return Error;
            break;
    }

    try
    {
        auto statusReply = bus.call(commandStatusMethod);
        std::variant<std::string> status;
        statusReply.read(status);
        std::string response(std::get<std::string>(status));
        if (DeviceTypeData::NVLinkMgmtNIC_Log == dataType)
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

uint64_t getNextRecord(uint8_t index, DeviceTypeData dataType)
{
    uint64_t nextRecord = 0;
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string objectPath, interf, method;
    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                         "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                         std::to_string(index);
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::GPU_SXM:
            objectPath = "/xyz/openbmc_project/inventory/system/"
                         "processors/GPU_SXM_" +
                         std::to_string(index);
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        default:
            std::string errorStr("Invalid data type in getNextRecord");
            log<level::ERR>(errorStr.c_str());
            return Error;
            break;
    }

    auto getNextRecord = bus.new_method_call("xyz.openbmc_project.NSM",
                                             objectPath.c_str(), interf.c_str(),
                                             method.c_str());

    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            getNextRecord.append("com.nvidia.Dump.DebugInfo",
                                 "NextRecordHandle");
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            getNextRecord.append("com.nvidia.Dump.DebugInfo",
                                 "NextRecordHandle");
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            getNextRecord.append("com.nvidia.Dump.LogInfo", "NextRecordHandle");
            break;
        case DeviceTypeData::GPU_SXM:
            getNextRecord.append("com.nvidia.Dump.DebugInfo",
                                 "NextRecordHandle");
            break;
        default:
            std::string errorStr("Invalid data type in getNextRecord");
            log<level::ERR>(errorStr.c_str());
            return Error;
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

uint8_t saveRecord(uint8_t index, DeviceTypeData dataType)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    std::string objectPath, interf, method;
    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                         "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                         std::to_string(index);
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            objectPath = "/xyz/openbmc_project/inventory/system/chassis/"
                         "HGX_NVLinkManagementNIC_0/NetworkAdapters/"
                         "NVLinkManagementNIC_0";
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        case DeviceTypeData::GPU_SXM:
            objectPath = "/xyz/openbmc_project/inventory/system/"
                         "processors/GPU_SXM_" +
                         std::to_string(index);
            interf = "org.freedesktop.DBus.Properties";
            method = "Get";
            break;
        default:
            std::string errorStr("Invalid data type in saveRecord");
            log<level::ERR>(errorStr.c_str());
            return Error;
            break;
    }

    auto getFdHandleMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            interf.c_str(), method.c_str());

    switch (dataType)
    {
        case DeviceTypeData::NVSwitch:
            getFdHandleMethod.append("com.nvidia.Dump.DebugInfo", "Fd");
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Dump:
            getFdHandleMethod.append("com.nvidia.Dump.DebugInfo", "Fd");
            break;
        case DeviceTypeData::NVLinkMgmtNIC_Log:
            getFdHandleMethod.append("com.nvidia.Dump.LogInfo", "Fd");
            break;
        case DeviceTypeData::GPU_SXM:
            getFdHandleMethod.append("com.nvidia.Dump.DebugInfo", "Fd");
            break;
        default:
            std::string errorStr("Invalid data type in saveRecord");
            log<level::ERR>(errorStr.c_str());
            return Error;
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

uint8_t sendSwitchResetCommand(uint8_t switchIndex)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    auto objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                      "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                      std::to_string(switchIndex);
    auto sendCommandMethod =
        bus.new_method_call("xyz.openbmc_project.NSM", objectPath.c_str(),
                            "xyz.openbmc_project.Control.ResetAsync", "Reset");
    try
    {
        auto reply = bus.call(sendCommandMethod);
        reply.read();
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function sendSwitchResetCommand failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }
    return Success;
}

uint8_t sendSwitchEraseCommand(uint8_t switchIndex)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    auto objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                      "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                      std::to_string(switchIndex);
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
        std::string errorStr("Function sendSwitchEraseCommand failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }
    return Success;
}

uint8_t getSwitchEraseStatus(uint8_t switchIndex)
{
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    auto objectPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                      "HGX_NVLinkFabric_0/Switches/NVSwitch_" +
                      std::to_string(switchIndex);
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
        if (eraseReason != "com.nvidia.Dump.Erase.OperationStatus.Success")
        {
            log<level::ERR>(eraseReason.c_str());
            return Error;
        }
        else
        {
            if (eraseStatus ==
                "com.nvidia.Dump.Erase.EraseStatus.DataEraseInProgress")
            {
                return InProgress;
            }
            else
            {
                if (eraseStatus ==
                    "com.nvidia.Dump.Erase.EraseStatus.DataErased")
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

    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::string errorStr("Function getSwitchEraseStatus failed");
        log<level::ERR>(errorStr.c_str());
        log<level::ERR>(e.what());
        return Error;
    }

    return Success;
}

uint8_t getSwitchDump(uint8_t switchIndex)
{
    uint64_t currentRecord = 0;
    uint64_t segmentsCounter = 0;
    uint8_t errorCounter = 0;
    uint16_t busyCounter = 0;
    uint8_t res;
    outputFileSize = 0;
    outputFileName = dumpPath + "/NVSwitch_" + std::to_string(switchIndex) +
                     "_dump.bin";
    std::string statusStr = "Started to get the Net_NVSwitch_" +
                            std::to_string(switchIndex) + " dump";
    log_msg(statusStr);
    do
    {
        res = InProgress;
        errorCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT && res != Success)
        {
            res = sendRequestRecordCommand(switchIndex, currentRecord,
                                           DeviceTypeData::NVSwitch);
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
            res = getRequestRecordCommandStatus(switchIndex,
                                                DeviceTypeData::NVSwitch);
            errorCounter += (res == Error);
            busyCounter += (res == InProgress);
        }
        res = InProgress;
        statusStr = "Getting the Net_NVSwitch_" + std::to_string(switchIndex);
        if (MAX_ERROR_COUNT == errorCounter)
        {
            statusStr += " dump reported errors";
            log_msg(statusStr);
            break;
        }
        if (MAX_IN_PROGRESS_COUNT == busyCounter)
        {
            statusStr += " dump timeout";
            log_msg(statusStr);
            break;
        }
        if (saveRecord(switchIndex, DeviceTypeData::NVSwitch))
        {
            statusStr = "Saving the Net_NVSwitch_" +
                        std::to_string(switchIndex) + " dump reported errors";
            log_msg(statusStr);
            break;
        }
        res = Success;
        segmentsCounter++;
        currentRecord = getNextRecord(switchIndex, DeviceTypeData::NVSwitch);
    } while (currentRecord != 0);
    statusStr = "Total number of segments: " + std::to_string(segmentsCounter);
    log_msg(statusStr);
    statusStr = "Output file size: " + std::to_string(outputFileSize);
    log_msg(statusStr);
    if (res != Success)
    {
        statusStr = "Getting the Net_NVSwitch_" + std::to_string(switchIndex) +
                    " dump completed with errors";
        log_msg(statusStr);
    }
    else
    {
        statusStr = "Getting the Net_NVSwitch_" + std::to_string(switchIndex) +
                    " dump completed successfully";
        log_msg(statusStr);
        statusStr = "Started to erase the Net_NVSwitch_" +
                    std::to_string(switchIndex) + " dump contents";
        log_msg(statusStr);
        res = InProgress;
        errorCounter = 0;
        busyCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT &&
               busyCounter < MAX_IN_PROGRESS_COUNT && res != Success)
        {
            res = sendSwitchEraseCommand(switchIndex);
            errorCounter += (Error == res);
            if (Success == res)
            {
                res = getSwitchEraseStatus(switchIndex);
                errorCounter += (Error == res);
                busyCounter += (InProgress == res);
            }
        }
        if (res != Success)
        {
            statusStr = "Erasing the Net_NVSwitch_" +
                        std::to_string(switchIndex) +
                        " dump completed with errors";
            log_msg(statusStr);
        }
        else
        {
            log_msg("Done.");
        }
    }
    return res;
}

uint8_t getLinkMgmtNICDump()
{
    uint64_t currentRecord = 0;
    uint64_t segmentsCounter = 0;
    uint8_t errorCounter = 0;
    uint16_t busyCounter = 0;
    uint8_t res;
    outputFileSize = 0;
    outputFileName = dumpPath + "/NVLinkMgmtNIC_0_dump.bin";
    std::string statusStr = "Started to get the Net_NVLinkManagementNIC_0 dump";
    log_msg(statusStr);

    do
    {
        res = InProgress;
        errorCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT && res != Success)
        {
            res = sendRequestRecordCommand(0, currentRecord,
                                           DeviceTypeData::NVLinkMgmtNIC_Dump);
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
            res = getRequestRecordCommandStatus(
                0, DeviceTypeData::NVLinkMgmtNIC_Dump);
            errorCounter += (res == Error);
            busyCounter += (res == InProgress);
        }
        res = InProgress;
        statusStr = "Getting the Net_NVLinkManagementNIC_0";
        if (MAX_ERROR_COUNT == errorCounter)
        {
            statusStr += " dump reported errors";
            log_msg(statusStr);
            break;
        }
        if (MAX_IN_PROGRESS_COUNT == busyCounter)
        {
            statusStr += " dump timeout";
            log_msg(statusStr);
            break;
        }
        if (saveRecord(0, DeviceTypeData::NVLinkMgmtNIC_Dump))
        {
            statusStr =
                "Saving the Net_NVLinkManagementNIC_0 dump reported errors";
            log_msg(statusStr);
            break;
        }
        res = Success;
        segmentsCounter++;
        currentRecord = getNextRecord(0, DeviceTypeData::NVLinkMgmtNIC_Dump);
    } while (currentRecord != 0);
    statusStr = "Total number of segments: " + std::to_string(segmentsCounter);
    log_msg(statusStr);
    statusStr = "Output file size: " + std::to_string(outputFileSize);
    log_msg(statusStr);
    if (res != Success)
    {
        statusStr =
            "Getting the Net_NVLinkManagementNIC_0 dump completed with errors";
        log_msg(statusStr);
    }
    else
    {
        statusStr =
            "Getting the Net_NVLinkManagementNIC_0 dump completed successfully";
        log_msg(statusStr);
    }
    return res;
}

uint8_t getLinkMgmtNICLog()
{
    uint64_t currentRecord = 0;
    uint64_t segmentsCounter = 0;
    uint8_t errorCounter = 0;
    uint16_t busyCounter = 0;
    uint8_t res;
    outputFileSize = 0;
    outputFileName = dumpPath + "/NVLinkMgmtNIC_0_Log.bin";
    std::string statusStr = "Started to get the Net_NVLinkManagementNIC_0 Log";
    log_msg(statusStr);
    do
    {
        res = InProgress;
        errorCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT && res != Success)
        {
            res = sendRequestRecordCommand(0, currentRecord,
                                           DeviceTypeData::NVLinkMgmtNIC_Log);
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
            res = getRequestRecordCommandStatus(
                0, DeviceTypeData::NVLinkMgmtNIC_Log);
            errorCounter += (res == Error);
            busyCounter += (res == InProgress);
        }
        res = InProgress;
        statusStr = "Getting the Net_NVLinkManagementNIC_0";
        if (MAX_ERROR_COUNT == errorCounter)
        {
            statusStr += " Log reported errors";
            log_msg(statusStr);
            break;
        }
        if (MAX_IN_PROGRESS_COUNT == busyCounter)
        {
            statusStr += " Log timeout";
            log_msg(statusStr);
            break;
        }
        if (saveRecord(0, DeviceTypeData::NVLinkMgmtNIC_Log))
        {
            statusStr =
                "Saving the Net_NVLinkManagementNIC_0 Log reported errors";
            log_msg(statusStr);
            break;
        }
        res = Success;
        segmentsCounter++;
        currentRecord = getNextRecord(0, DeviceTypeData::NVLinkMgmtNIC_Log);
    } while (currentRecord != 0);
    statusStr = "Total number of segments: " + std::to_string(segmentsCounter);
    log_msg(statusStr);
    statusStr = "Output file size: " + std::to_string(outputFileSize);
    log_msg(statusStr);
    if (res != Success)
    {
        statusStr =
            "Getting the Net_NVLinkManagementNIC_0 Log completed with errors";
        log_msg(statusStr);
    }
    else
    {
        statusStr =
            "Getting the Net_NVLinkManagementNIC_0 Log completed successfully";
        log_msg(statusStr);
    }
    return res;
}

uint8_t getGPUDump(uint8_t GPUIndex)
{
    uint64_t currentRecord = 0;
    uint64_t segmentsCounter = 0;
    uint8_t errorCounter = 0;
    uint16_t busyCounter = 0;
    uint8_t res;
    outputFileSize = 0;
    outputFileName = dumpPath + "/GPU_SXM_" + std::to_string(GPUIndex) +
                     "_dump.bin";
    std::string statusStr = "Started to get the Net_GPU_SXM_" +
                            std::to_string(GPUIndex) + " dump";
    log_msg(statusStr);

    do
    {
        res = InProgress;
        errorCounter = 0;
        while (errorCounter < MAX_ERROR_COUNT && res != Success)
        {
            res = sendRequestRecordCommand(GPUIndex, currentRecord,
                                           DeviceTypeData::GPU_SXM);
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
            res = getRequestRecordCommandStatus(GPUIndex,
                                                DeviceTypeData::GPU_SXM);
            errorCounter += (res == Error);
            busyCounter += (res == InProgress);
        }
        res = InProgress;
        statusStr = "Getting the Net_GPU_SXM_" + std::to_string(GPUIndex);
        if (MAX_ERROR_COUNT == errorCounter)
        {
            statusStr += " dump reported errors";
            log_msg(statusStr);
            break;
        }
        if (MAX_IN_PROGRESS_COUNT == busyCounter)
        {
            statusStr += " dump timeout";
            log_msg(statusStr);
            break;
        }
        if (saveRecord(GPUIndex, DeviceTypeData::GPU_SXM))
        {
            statusStr = "Saving the Net_GPU_SXM_" + std::to_string(GPUIndex) +
                        " dump reported errors";
            log_msg(statusStr);
            break;
        }
        res = Success;
        segmentsCounter++;
        currentRecord = getNextRecord(GPUIndex, DeviceTypeData::GPU_SXM);
    } while (currentRecord != 0);
    statusStr = "Total number of segments: " + std::to_string(segmentsCounter);
    log_msg(statusStr);
    statusStr = "Output file size: " + std::to_string(outputFileSize);
    log_msg(statusStr);
    if (res != Success)
    {
        statusStr = "Getting the Net_GPU_SXM_" + std::to_string(GPUIndex) +
                    " dump completed with errors";
        log_msg(statusStr);
    }
    else
    {
        statusStr = "Getting the Net_GPU_SXM_" + std::to_string(GPUIndex) +
                    " dump completed successfully";
        log_msg(statusStr);
    }
    return res;
}

int main(int argc, char** argv)
{
    uint8_t res_dump = Error;

    if (argc > 2)
    {
        dumpPath = argv[1];
        targetDevice = argv[2];
        log_msg(dumpPath);
        log_msg(targetDevice);

        std::string switchStr("Net_NVSwitch_");

        using std::chrono::duration_cast;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;

        auto t1 = high_resolution_clock::now();

        if (targetDevice.find(switchStr) != std::string::npos)
        {
            uint16_t switchIndex =
                atoi(targetDevice.substr(switchStr.length(), std::string::npos)
                         .c_str());
            res_dump = getSwitchDump(switchIndex);
        }
        else if ("Net_NVLinkManagementNIC_0" == targetDevice)
        {
            res_dump = getLinkMgmtNICDump();
            getLinkMgmtNICLog();
        }
        else
        {
            switchStr = "Net_GPU_SXM_";
            if (targetDevice.find(switchStr) != std::string::npos)
            {
                uint16_t GPUIndex = atoi(
                    targetDevice.substr(switchStr.length(), std::string::npos)
                        .c_str());
                res_dump = getGPUDump(GPUIndex);
            }
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
    }
    else
    {
        printf("nsm-net-dump-tool version " VERSION "\n");
        printf("Usage: nsm-net-dump-tool <temp folder> <target device>\n");
    }

    return res_dump;
}
