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

#include "dump_manager_system.hpp"

#include "dump_utils.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <regex>
#include <unordered_map>

namespace phosphor
{
namespace dump
{
namespace system
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

// Diagnostic type enumeration
enum class DiagnosticType
{
    SelfTest,
    FPGA,
    EROT,
    ROT,
    SMA,
    NVSwitch,
    NVLinkManagementNIC,
    GPU_SXM,
    NetIR,
    GPUDeviceDiagnostics,
    RetLTSSM,
    RetRegister,
    FirmwareAttributes,
    HardwareCheckout,
    Unknown
};

// String to enum mapping
const std::unordered_map<std::string, DiagnosticType> diagnosticTypeMap = {
    {"SelfTest", DiagnosticType::SelfTest},
    {"FPGA", DiagnosticType::FPGA},
    {"EROT", DiagnosticType::EROT},
    {"ROT", DiagnosticType::ROT},
    {"SMA", DiagnosticType::SMA},
    {"Net_NVSwitch", DiagnosticType::NVSwitch},
    {"Net_NVLinkManagementNIC", DiagnosticType::NVLinkManagementNIC},
    {"Net_GPU_SXM", DiagnosticType::GPU_SXM},
    {"NetIR", DiagnosticType::NetIR},
    {"GPUDeviceDiagnostics", DiagnosticType::GPUDeviceDiagnostics},
    {"RetLTSSM", DiagnosticType::RetLTSSM},
    {"RetRegister", DiagnosticType::RetRegister},
    {"FirmwareAttributes", DiagnosticType::FirmwareAttributes},
    {"HardwareCheckout", DiagnosticType::HardwareCheckout}};

// Helper function to get DiagnosticType from string
DiagnosticType getDiagnosticType(const std::string& typeStr)
{
    auto it = diagnosticTypeMap.find(typeStr);
    return it != diagnosticTypeMap.end() ? it->second : DiagnosticType::Unknown;
}

// TODO: Merge system dump with bmc dump to avoid code duplication.

void Manager::limitDumpEntries()
{
    // Delete dumps only when system dump max limit is configured
#if SYSTEM_DUMP_MAX_LIMIT == 0
    // Do nothing - system dump max limit is not configured
    return;
#else  // #if SYSTEM_DUMP_MAX_LIMIT == 0
    // Delete dumps on reaching allowed entries
    auto totalDumps = entries.size();
    if (totalDumps < SYSTEM_DUMP_MAX_LIMIT)
    {
        // Do nothing - Its within allowed entries
        return;
    }
    // Get the oldest dumps
    size_t excessDumps = totalDumps - (SYSTEM_DUMP_MAX_LIMIT - 1);
    // Delete the oldest dumps
    auto d = entries.begin();
    while (d != entries.end() && excessDumps != 0U)
    {
        auto& entry = d->second;
        d++;
        entry->delete_();
        --excessDumps;
    }

    return;
#endif // #if SYSTEM_DUMP_MAX_LIMIT == 0
}

sdbusplus::message::object_path Manager::createDump(
    phosphor::dump::DumpCreateParams params)
{
    // Limit dumps to max allowed entries
    limitDumpEntries();

    auto diagnosticTypeStr = std::get<std::string>(params["DiagnosticType"]);
    auto diagnosticType = getDiagnosticType(diagnosticTypeStr);

    // Check whether there is same dump already running
    // Also ensure RetLTSSM and RetRegister will not run at the same time
    if ((Manager::dumpInProgress.contains(diagnosticTypeStr)) ||
        (Manager::dumpInProgress.contains("RetLTSSM") &&
         diagnosticType == DiagnosticType::RetRegister) ||
        (Manager::dumpInProgress.contains("RetRegister") &&
         diagnosticType == DiagnosticType::RetLTSSM))
    {
        elog<Unavailable>();
    }

    // Ensure NetIR and GPUDeviceDiagnostics will not run at the same time
    if ((Manager::dumpInProgress.contains("NetIR") &&
         diagnosticType == DiagnosticType::GPUDeviceDiagnostics) ||
        (Manager::dumpInProgress.contains("GPUDeviceDiagnostics") &&
         diagnosticType == DiagnosticType::NetIR))
    {
        elog<Unavailable>();
    }

    // Ensure Net_GPU_SXM and GPUDeviceDiagnostics will not run at the same time
    if ((Manager::dumpInProgress.contains("Net_GPU_SXM") &&
         diagnosticType == DiagnosticType::GPUDeviceDiagnostics) ||
        (Manager::dumpInProgress.contains("GPUDeviceDiagnostics") &&
         diagnosticType == DiagnosticType::GPU_SXM))
    {
        elog<Unavailable>();
    }

    auto id = captureDump(params);

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        // Get the originator id and type from params
        std::string originatorId;
        originatorTypes originatorType;

        phosphor::dump::extractOriginatorProperties(params, originatorId,
                                                    originatorType);
        std::time_t timeStamp = std::time(nullptr);
        entries.insert(std::make_pair(
            id, std::make_unique<system::Entry>(
                    bus, objPath.c_str(), id, timeStamp, 0, std::string(),
                    phosphor::dump::OperationStatus::InProgress, originatorId,
                    originatorType, *this, diagnosticTypeStr)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating system dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id));
        elog<InternalFailure>();
    }

    return objPath.string();
}

// captureDump helper functions
// NOLINTBEGIN
// Helper function to build and execute dump command
uint32_t executeDumpCommand(
    const std::string& binPath, const std::string& dumpId,
    const std::string& dumpPath,
    const std::vector<std::pair<std::string, std::string>>& options,
    const std::string& errorMsg)
{
    std::vector<char*> arg_v;

    // Add binary path
    arg_v.push_back(const_cast<char*>(binPath.c_str()));

    // Add path and id options which are common to all dumps
    arg_v.push_back(const_cast<char*>("-p"));
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    arg_v.push_back(const_cast<char*>("-i"));
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));

    // Add additional options
    for (const auto& opt : options)
    {
        arg_v.push_back(const_cast<char*>(opt.first.c_str()));
        if (!opt.second.empty())
        {
            arg_v.push_back(const_cast<char*>(opt.second.c_str()));
        }
    }

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // If we get here, execution failed
    auto error = errno;
    log<level::ERR>(errorMsg.c_str(), entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t executeDreport(const std::string& dumpType, const std::string& dumpId,
                        const std::string& dumpPath, const size_t size,
                        const std::array<std::string, 3>& addArgs)
{
    std::vector<std::pair<std::string, std::string>> options = {
        {"-d", dumpPath},
        {"-s", std::to_string(size)},
        {"-q", ""},
        {"-v", ""},
        {"-t", dumpType}};

    // Add additional arguments
    for (const auto& arg : addArgs)
    {
        if (!arg.empty())
        {
            options.push_back({"-a", arg});
        }
    }

    return executeDumpCommand(
        "/usr/bin/dreport", dumpId, dumpPath, options,
        "System dump: Error occurred during dreport function execution");
}

uint32_t selfTest(const std::string& dumpId, const std::string& dumpPath)
{
    return executeDumpCommand(
        SELFTEST_BIN_PATH, dumpId, dumpPath, {{"-v", ""}},
        "System dump: Error occurred during self test execution");
}

uint32_t fpgaRegDump(const std::string& dumpId, const std::string& dumpPath)
{
    return executeDumpCommand(
        FPGA_DUMP_BIN_PATH, dumpId, dumpPath, {},
        "System dump: Error occurred during FPGA register dump execution");
}

uint32_t smaRegDump(const std::string& dumpId, const std::string& dumpPath)
{
    return executeDumpCommand(
        SMA_DUMP_BIN_PATH, dumpId, dumpPath, {},
        "System dump: Error occurred during SMA register dump execution");
}

uint32_t nsmDump(const std::string& dumpId, const std::string& dumpPath,
                 const std::string& tempPath, const std::string& targetDevice,
                 const std::string& dumpType)
{
    return executeDumpCommand(
        NSM_DUMP_BIN_PATH, dumpId, dumpPath,
        {{"-t", tempPath}, {"-d", targetDevice}, {"-o", dumpType}},
        "System dump: Error occurred during nsmDump function execution");
}

uint32_t erotDump(const std::string& dumpId, const std::string& dumpPath)
{
    return executeDumpCommand(
        EROT_DUMP_BIN_PATH, dumpId, dumpPath, {},
        "System dump: Error occurred during dreport function execution");
}

uint32_t retimerLtssmDump(const std::string& dumpId,
                          const std::string& dumpPath,
                          const std::string& vendorId)
{
    // Construct additional options for Retimer Ltssm Dump
    std::vector<std::pair<std::string, std::string>> options;
    if (!vendorId.empty())
    {
        options.push_back({"-v", vendorId});
    }

    return executeDumpCommand(
        RETIMER_LTSSM_DUMP_BIN_PATH, dumpId, dumpPath, options,
        "System dump: Error occurred during retimerLtssmDump function execution");
}

uint32_t retimerRegisterDump(
    const std::string& dumpId, const std::string& dumpPath,
    const std::string& retimer_address, const std::string& vendorId)
{
    // Construct additional options for Retimer Register Dump
    std::vector<std::pair<std::string, std::string>> options;
    if (!retimer_address.empty())
    {
        options.push_back({"-a", retimer_address});
    }
    if (!vendorId.empty())
    {
        options.push_back({"-v", vendorId});
    }

    return executeDumpCommand(
        RETIMER_REGISTER_DUMP_BIN_PATH, dumpId, dumpPath, options,
        "System dump: Error occurred during retimerRegisterDump function execution");
}

uint32_t fwAttrsDump(const std::string& dumpId, const std::string& dumpPath)
{
    return executeDumpCommand(
        FWATTRS_DUMP_BIN_PATH, dumpId, dumpPath, {{"-v", ""}},
        "System dump: Error occurred during firmware attributes dump execution");
}

uint32_t hwCheckoutDump(const std::string& dumpId, const std::string& dumpPath)
{
    return executeDumpCommand(
        HWCHECKOUT_DUMP_BIN_PATH, dumpId, dumpPath, {{"-v", ""}},
        "System dump: Error occurred during hardware checkout dump execution");
}

// NOLINTEND
uint32_t Manager::captureDump(phosphor::dump::DumpCreateParams params)
{
    // check if minimum required space is available on destination partition
    std::error_code ec{};
    fs::path partitionPath(dumpDir);

#if (JFFS_SPACE_CALC_INACCURACY_OFFSET_WORKAROUND_PERCENT > 0)
    /* jffs2 space available problem is worked around by substracting 2%
       of capacity from currently available space, eg. 200M - 4M = 196M
       it solves problem of failed dump when user request it close to space
       limit so instead if silently failing the task user receives appropriate
       message. Test it yourself - fill up the partition until 'no space left'
       message appears, check `df -T` for available space, if there seems to be
       at least 1% space available then you just reproduced the issue*/
    uintmax_t offset = (fs::space(partitionPath, ec).capacity *
                        JFFS_SPACE_CALC_INACCURACY_OFFSET_WORKAROUND_PERCENT) /
                       100;
    uintmax_t spaceAvailable = fs::space(partitionPath, ec).available;
    uintmax_t sizeLeftKb = 0;
    if (spaceAvailable >= offset)
    {
        sizeLeftKb = (spaceAvailable - offset) / 1024;
    }
#else
    uintmax_t sizeLeftKb = fs::space(partitionPath, ec).available / 1024;
#endif
    uintmax_t reqSizeKb = SYSTEM_DUMP_MIN_SPACE_REQD;

    if (ec.value() != 0)
    {
        log<level::ERR>("Failed to check available space");
        elog<InternalFailure>();
    }

    if (sizeLeftKb < reqSizeKb)
    {
        log<level::ERR>(
            "Not enough space available to create system dump",
            entry("REQ_KB=%d", static_cast<unsigned int>(reqSizeKb)),
            entry("LEFT_KB=%d", static_cast<unsigned int>(sizeLeftKb)));
        using QuotaExceeded =
            sdbusplus::xyz::openbmc_project::Dump::Create::Error::QuotaExceeded;
        using Reason =
            xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }

    // Get Dump size.
    auto size = getAllowedSize();

    auto diagnosticTypeStr = std::get<std::string>(params["DiagnosticType"]);
    auto deviceID = std::get<std::string>(params["DeviceID"]);
    auto deviceType = std::get<std::string>(params["DeviceType"]);
    params.erase("DiagnosticType");
    params.erase("DeviceID");
    params.erase("DeviceType");

    auto diagnosticType = getDiagnosticType(diagnosticTypeStr);

    if (!diagnosticTypeStr.empty() && diagnosticType == DiagnosticType::Unknown)
    {
        log<level::ERR>("Unrecognized DiagnosticType option",
                        entry("DIAG_TYPE=%s", diagnosticTypeStr.c_str()));
        using INV_ARG =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
        using INV_VAL =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
        elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                              INV_VAL(diagnosticTypeStr.c_str()));
    }

#ifdef FAULTLOG_DUMP_EXTENSION
    if (diagnosticType == DiagnosticType::SelfTest)
    {
        log<level::ERR>("Unsupported DiagnosticType option",
                        entry("DIAG_TYPE=%s", diagnosticTypeStr.c_str()));
        using INV_ARG =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
        using INV_VAL =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
        elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                              INV_VAL(diagnosticTypeStr.c_str()));
    }
#endif

    log<level::INFO>(
        fmt::format("Capturing system dump of type ({})", diagnosticTypeStr)
            .c_str());

    if (diagnosticType == DiagnosticType::RetLTSSM)
    {
        retimerState.debugMode(true);
    }

    Manager::dumpInProgress.insert(diagnosticTypeStr);

    pid_t pid = fork();

    if (pid == 0)
    {
        fs::path dumpPath(dumpDir);
        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        // Construct additional arguments from params
        std::array<std::string, 3> addArgs;
        // Fix additional arguments order 'bf_ip', 'bf_username', 'bf_password'
        for (const auto& param : params)
        {
            auto kvPair = param.first + "=" +
                          std::get<std::string>(param.second);
            if (param.first == "bf_ip")
            {
                addArgs[0] = kvPair;
            }
            else if (param.first == "bf_username")
            {
                addArgs[1] = kvPair;
            }
            else if (param.first == "bf_password")
            {
                addArgs[2] = kvPair;
            }
            else
            {
                log<level::ERR>("System dump: Unknown additional arguments");
            }
        }

        switch (diagnosticType)
        {
            case DiagnosticType::Unknown:
                executeDreport("system", id, dumpPath, size, addArgs);
                break;
            case DiagnosticType::SelfTest:
                selfTest(id, dumpPath);
                break;
            case DiagnosticType::FPGA:
                fpgaRegDump(id, dumpPath);
                break;
            case DiagnosticType::SMA:
                smaRegDump(id, dumpPath);
                break;
            case DiagnosticType::EROT:
            case DiagnosticType::ROT:
                erotDump(id, dumpPath);
                break;
            case DiagnosticType::NVSwitch:
            case DiagnosticType::NVLinkManagementNIC:
            case DiagnosticType::GPU_SXM:
                if (deviceID.empty())
                {
                    log<level::ERR>("System dump: missing DeviceID parameter");
                    elog<InternalFailure>();
                }
                else
                {
                    diagnosticTypeStr =
                        diagnosticTypeStr.erase(0, 4) + "_" + deviceID;
                    nsmDump(id, dumpPath, NSM_DUMP_TEMP_PATH, diagnosticTypeStr,
                            "Network");
                }
                break;
            case DiagnosticType::NetIR:
                if (deviceType.empty())
                {
                    log<level::ERR>(
                        "System dump: missing DeviceType parameter");
                    elog<InternalFailure>();
                }
                else
                {
                    nsmDump(id, dumpPath, NSM_DUMP_TEMP_PATH, deviceType,
                            "Network");
                }
                break;
            case DiagnosticType::GPUDeviceDiagnostics:
                if (deviceType.empty())
                {
                    log<level::ERR>(
                        "System dump: missing DeviceType parameter");
                    elog<InternalFailure>();
                }
                else
                {
                    nsmDump(id, dumpPath, NSM_DUMP_TEMP_PATH, deviceType,
                            "Diagnostics");
                }
                break;
            case DiagnosticType::RetLTSSM:
                retimerLtssmDump(id, dumpPath, retimerState.getVendorId());
                break;
            case DiagnosticType::RetRegister:
            {
                std::string retimer_address =
                    std::get<std::string>(params["Address"]);
                retimerRegisterDump(id, dumpPath, retimer_address,
                                    retimerState.getVendorId());
                break;
            }
            case DiagnosticType::FirmwareAttributes:
                fwAttrsDump(id, dumpPath);
                break;
            case DiagnosticType::HardwareCheckout:
                hwCheckoutDump(id, dumpPath);
                break;
            default:
                log<level::ERR>("System dump: Invalid DiagnosticType");
                elog<InternalFailure>();
        }
    }
    else if (pid > 0)
    {
        auto entryId = lastEntryId + 1;
        Child::Callback callback = [this, pid, entryId, diagnosticTypeStr](
                                       Child&, const siginfo_t* si) {
            if (si->si_status != 0)
            {
                std::string msg =
                    "Dump process failed: (signo)" +
                    std::to_string(si->si_signo) + "; (code)" +
                    std::to_string(si->si_code) + "; (errno)" +
                    std::to_string(si->si_errno) + "; (pid)" +
                    std::to_string(si->si_pid) + "; (status)" +
                    std::to_string(si->si_status);
                log<level::ERR>(msg.c_str());
                this->createDumpFailed(static_cast<int>(entryId));

                // Disable retimer debug mode if RetLTSSM dump failed
                if (diagnosticTypeStr == "RetLTSSM")
                {
                    retimerState.debugMode(false);
                }
            }

            this->childPtrMap.erase(pid);
            // Remove dumpType from dumpInProgress when dump ends
            Manager::dumpInProgress.erase(diagnosticTypeStr);
        };

        try
        {
            childPtrMap.emplace(pid,
                                std::make_unique<Child>(eventLoop.get(), pid,
                                                        WEXITED | WSTOPPED,
                                                        std::move(callback)));
        }
        catch (const sdeventplus::SdEventError& ex)
        {
            // Failed to add to event loop
            log<level::ERR>(
                fmt::format(
                    "Error occurred during the sdeventplus::source::Child "
                    "creation ex({})",
                    ex.what())
                    .c_str());
            // Remove dumpType from dumpInProgress
            Manager::dumpInProgress.erase(diagnosticTypeStr);
            elog<InternalFailure>();
        }
    }
    else
    {
        auto error = errno;
        log<level::ERR>("System dump: Error occurred during fork",
                        entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }

    return ++lastEntryId;
}

void Manager::createEntry(const fs::path& file)
{
    // Dump File Name format obmcdump_ID_EPOCHTIME.EXT
    static constexpr auto ID_POS = 1;
    static constexpr auto EPOCHTIME_POS = 2;
    std::regex file_regex("obmcdump_([0-9]+)_([0-9]+)\\.([a-zA-Z0-9]+)");

    std::smatch match;
    std::string name = file.filename();

    if (!((std::regex_search(name, match, file_regex)) && (match.size() > 0)))
    {
        log<level::ERR>("System dump: Invalid Dump file name",
                        entry("FILENAME=%s", file.filename().c_str()));
        return;
    }

    auto idString = match[ID_POS];
    uint64_t timestamp = stoull(match[EPOCHTIME_POS]) * 1000 * 1000;

    auto id = stoul(idString);

    // If there is an existing entry update it and return.
    auto dumpEntry = entries.find(id);
    if (dumpEntry != entries.end())
    {
        auto entryPtr = dynamic_cast<phosphor::dump::system::Entry*>(
            dumpEntry->second.get());
        if (entryPtr != nullptr)
        {
            entryPtr->update(timestamp, fs::file_size(file), file);
            auto dumpType = entryPtr->getDumpType();
            if (dumpType == "RetLTSSM")
            {
                retimerState.debugMode(false);
            }
            Manager::dumpInProgress.erase(dumpType);

            return;
        }
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        // Get the originator id and type from params
        std::string originatorId;
        originatorTypes originatorType;

        entries.insert(std::make_pair(
            id, std::make_unique<system::Entry>(
                    bus, objPath.c_str(), id, timestamp, fs::file_size(file),
                    file, phosphor::dump::OperationStatus::Completed,
                    originatorId, originatorType, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating system dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id), entry("TIMESTAMP=%ull", timestamp),
                        entry("SIZE=%d", fs::file_size(file)),
                        entry("FILENAME=%s", file.c_str()));
    }
}

void Manager::watchCallback(const UserMap& fileInfo)
{
    for (const auto& [path, event] : fileInfo)
    {
        // For any new dump file create dump entry object
        // and associated inotify watch.
        if (event == IN_CLOSE_WRITE)
        {
            if (!std::filesystem::is_directory(path))
            {
                // Don't require filename to be passed, as the path
                // of dump directory is stored in the childWatchMap
                removeWatch(path.parent_path());
                // dump file is written now create D-Bus entry
                createEntry(path);
            }
            else
            {
                removeWatch(path);
            }
        }
        // Start inotify watch on newly created directory.
        else if (event == IN_CREATE && fs::is_directory(path))
        {
            auto watchObj = std::make_unique<Watch>(
                eventLoop, IN_NONBLOCK, IN_CLOSE_WRITE, EPOLLIN, path,
                std::bind(std::mem_fn(&Manager::watchCallback), this,
                          std::placeholders::_1));

            childWatchMap.emplace(path, std::move(watchObj));
        }
    }
}

void Manager::removeWatch(const fs::path& path)
{
    // Delete Watch entry from map.
    childWatchMap.erase(path);
}

void Manager::restore()
{
    fs::path dir(dumpDir);
    if (!fs::exists(dir) || fs::is_empty(dir))
    {
        return;
    }

    // Dump file path: <DUMP_PATH>/<id>/<filename>
    for (const auto& p : fs::directory_iterator(dir))
    {
        auto idStr = p.path().filename().string();

        // Consider only directory's with dump id as name.
        // Note: As per design one file per directory.
        if (fs::is_directory(p.path()) &&
            std::all_of(idStr.begin(), idStr.end(), ::isdigit))
        {
            lastEntryId =
                std::max(lastEntryId, static_cast<uint32_t>(std::stoul(idStr)));
            auto fileIt = fs::directory_iterator(p.path());
            // Create dump entry d-bus object.
            if (fileIt != fs::end(fileIt))
            {
                createEntry(fileIt->path());
            }
        }
    }
}

size_t Manager::getAllowedSize()
{
    using namespace sdbusplus::xyz::openbmc_project::Dump::Create::Error;
    using Reason = xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;

    size_t size = 0;

    // Get current size of the dump directory.
    for (const auto& p : fs::recursive_directory_iterator(dumpDir))
    {
        if (!fs::is_directory(p))
        {
            size += fs::file_size(p);
        }
    }

    // Convert size into KB
    size = size / 1024;

    // Set the Dump size to Maximum  if the free space is greater than
    // Dump max size otherwise return the available size.

    size = (size > SYSTEM_DUMP_TOTAL_SIZE ? 0 : SYSTEM_DUMP_TOTAL_SIZE - size);

    if (size < SYSTEM_DUMP_MIN_SPACE_REQD)
    {
        // Reached to maximum limit
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }

    return std::min(size, static_cast<size_t>(SYSTEM_DUMP_MAX_SIZE));
}

} // namespace system
} // namespace dump
} // namespace phosphor
