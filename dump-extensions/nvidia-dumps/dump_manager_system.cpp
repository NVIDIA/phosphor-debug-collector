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
#include "config.h"

#include "dump_manager_system.hpp"

#include "dump_utils.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <iostream>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <regex>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>

namespace phosphor
{
namespace dump
{
namespace system
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

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
    int excessDumps = totalDumps - (SYSTEM_DUMP_MAX_LIMIT - 1);
    // Delete the oldest dumps
    for (auto d = entries.begin(); d != entries.end() && excessDumps; d++)
    {
        auto& entry = d->second;
        entry->delete_();
        --excessDumps;
    }

    return;
#endif // #if SYSTEM_DUMP_MAX_LIMIT == 0
}

sdbusplus::message::object_path
    Manager::createDump(phosphor::dump::DumpCreateParams params)
{
    // Limit dumps to max allowed entries
    limitDumpEntries();
    // Check whether there is same dump already running
    // Also ensure RetLTSSM and RetRegister will not run at the same time
    auto dumpType = std::get<std::string>(params["DiagnosticType"]);
    if ((Manager::dumpInProgress.find(dumpType) !=
         Manager::dumpInProgress.end()) ||
        (Manager::dumpInProgress.find("RetLTSSM") !=
             Manager::dumpInProgress.end() &&
         dumpType == "RetRegister") ||
        (Manager::dumpInProgress.find("RetRegister") !=
             Manager::dumpInProgress.end() &&
         dumpType == "RetLTSSM"))
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
                    originatorType, *this,
                    std::get<std::string>(params["DiagnosticType"]))));
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
uint32_t executeDreport(const std::string& dumpType, const std::string& dumpId,
                        const std::string& dumpPath, const size_t size,
                        const std::array<std::string, 3>& addArgs)
{
    // Construct dreport arguments
    std::vector<char*> arg_v;
    std::string fPath = "/usr/bin/dreport";
    arg_v.push_back(&fPath[0]);
    std::string dOption = "-d";
    arg_v.push_back(&dOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string sOption = "-s";
    arg_v.push_back(&sOption[0]);
    arg_v.push_back(const_cast<char*>(std::to_string(size).c_str()));
    std::string qOption = "-q";
    arg_v.push_back(&qOption[0]);
    std::string vOption = "-v";
    arg_v.push_back(&vOption[0]);
    std::string tOption = "-t";
    arg_v.push_back(&tOption[0]);
    arg_v.push_back(const_cast<char*>(dumpType.c_str()));
    // Add additional arguments
    std::string aOption = "-a";
    for (int i = 0; i < (int)addArgs.size(); i++)
    {
        arg_v.push_back(&aOption[0]);
        arg_v.push_back(const_cast<char*>((addArgs[i]).c_str()));
    }
    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // dreport script execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during dreport function execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t selfTest(const std::string& dumpId, const std::string& dumpPath)
{
    // Construct selftest dump arguments
    std::vector<char*> arg_v;
    std::string fPath = SELFTEST_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string dOption = "-v";
    arg_v.push_back(&dOption[0]);

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // self test execution is failed.
    auto error = errno;
    log<level::ERR>("System dump: Error occurred during self test execution",
                    entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t fpgaRegDump(const std::string& dumpId, const std::string& dumpPath)
{
    // Construct fpga dump arguments
    std::vector<char*> arg_v;
    std::string fPath = FPGA_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // FPGA register dump execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during FPGA register dump execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t erotDump(const std::string& dumpId, const std::string& dumpPath)
{
    // Construct erot dump arguments
    std::vector<char*> arg_v;
    std::string fPath = EROT_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));

    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // dreport script execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during dreport function execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t retimerLtssmDump(const std::string& dumpId,
                          const std::string& dumpPath,
                          const std::string& vendorId)
{
    // Construct Ltssm dump arguments
    std::vector<char*> arg_v;
    std::string fPath = RETIMER_LTSSM_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    if (!vendorId.empty())
    {
        std::string vOption = "-v";
        arg_v.push_back(&vOption[0]);
        arg_v.push_back(const_cast<char*>(vendorId.c_str()));
    }

    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // Retimer LTSSM Dump execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during retimerLtssmDump function execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t retimerRegisterDump(const std::string& dumpId,
                             const std::string& dumpPath,
                             const std::string& retimer_address,
                             const std::string& vendorId)
{
    // Construct Register dump arguments
    std::vector<char*> arg_v;
    std::string fPath = RETIMER_REGISTER_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    if (!retimer_address.empty())
    {
        std::string aOption = "-a";
        arg_v.push_back(&aOption[0]);
        arg_v.push_back(const_cast<char*>(retimer_address.c_str()));
    }
    if (!vendorId.empty())
    {
        std::string vOption = "-v";
        arg_v.push_back(&vOption[0]);
        arg_v.push_back(const_cast<char*>(vendorId.c_str()));
    }

    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // Retimer Register Dump execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during retimerRegisterDump function execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t fwAttrsDump(const std::string& dumpId, const std::string& dumpPath)
{
    // Construct firmware attributes dump arguments
    std::vector<char*> arg_v;
    std::string fPath = FWATTRS_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string dOption = "-v";
    arg_v.push_back(&dOption[0]);

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // firmware attributes dump execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during firmware attributes dump execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

uint32_t hwCheckoutDump(const std::string& dumpId, const std::string& dumpPath)
{
    // Construct hardware checkout dump arguments
    std::vector<char*> arg_v;
    std::string fPath = HWCHECKOUT_DUMP_BIN_PATH;
    arg_v.push_back(&fPath[0]);
    std::string pOption = "-p";
    arg_v.push_back(&pOption[0]);
    arg_v.push_back(const_cast<char*>(dumpPath.c_str()));
    std::string iOption = "-i";
    arg_v.push_back(&iOption[0]);
    arg_v.push_back(const_cast<char*>(dumpId.c_str()));
    std::string dOption = "-v";
    arg_v.push_back(&dOption[0]);

    arg_v.push_back(nullptr);
    execv(arg_v[0], &arg_v[0]);

    // hardware checkout dump execution is failed.
    auto error = errno;
    log<level::ERR>(
        "System dump: Error occurred during hardware checkout dump execution",
        entry("ERRNO=%d", error));
    elog<InternalFailure>();
}

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

    // Validate request argument
    const std::string typeSelftest = "SelfTest";
    const std::string typeFPGA = "FPGA";
    const std::string typeEROT = "EROT";
    const std::string typeROT = "ROT";
    const std::string typeLTSSM = "RetLTSSM";
    const std::string typeRetimerRegister = "RetRegister";
    const std::string typeFwAtts = "FirmwareAttributes";
    const std::string typeHwCheckout = "HardwareCheckout";
    auto diagnosticType = std::get<std::string>(params["DiagnosticType"]);
    params.erase("DiagnosticType");
    if (!diagnosticType.empty())
    {
        if (diagnosticType != typeSelftest && diagnosticType != typeFPGA &&
            diagnosticType != typeEROT && diagnosticType != typeROT &&
            diagnosticType != typeLTSSM &&
            diagnosticType != typeRetimerRegister &&
            diagnosticType != typeFwAtts && diagnosticType != typeHwCheckout)
        {
            log<level::ERR>("Unrecognized DiagnosticType option",
                            entry("DIAG_TYPE=%s", diagnosticType.c_str()));
            using INV_ARG =
                xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
            using INV_VAL =
                xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
            elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                                  INV_VAL(diagnosticType.c_str()));
        }
#ifdef FAULTLOG_DUMP_EXTENSION
        if (diagnosticType == typeSelftest)
        {
            log<level::ERR>("Unsupported DiagnosticType option",
                            entry("DIAG_TYPE=%s", diagnosticType.c_str()));
            using INV_ARG =
                xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
            using INV_VAL =
                xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
            elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                                  INV_VAL(diagnosticType.c_str()));
        }
#endif
    }

    log<level::INFO>(
        fmt::format("Capturing system dump of type ({})", diagnosticType)
            .c_str());

    if (diagnosticType == typeLTSSM)
    {
        retimerState.debugMode(true);
    }

    Manager::dumpInProgress.insert(diagnosticType);

    pid_t pid = fork();

    if (pid == 0)
    {
        fs::path dumpPath(dumpDir);
        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        std::string dumpType = "system";

        // Construct additional arguments from params
        std::array<std::string, 3> addArgs;
        // Fix additional arguments order 'bf_ip', 'bf_username', 'bf_password'
        // std::map<std::string, std::string>::iterator itr;
        for (auto itr = params.begin(); itr != params.end(); ++itr)
        {
            auto kvPair = itr->first + "=" + std::get<std::string>(itr->second);
            if (itr->first == "bf_ip")
            {
                addArgs[0] = kvPair;
            }
            else if (itr->first == "bf_username")
            {
                addArgs[1] = kvPair;
            }
            else if (itr->first == "bf_password")
            {
                addArgs[2] = kvPair;
            }
            else
            {
                log<level::ERR>("System dump: Unknown additional arguments");
            }
        }

        if (diagnosticType.empty())
        {
            executeDreport(dumpType, id, dumpPath, size, addArgs);
        }
        else if (diagnosticType == typeSelftest)
        {
            selfTest(id, dumpPath);
        }
        else if (diagnosticType == typeFPGA)
        {
            fpgaRegDump(id, dumpPath);
        }
        else if (diagnosticType == typeEROT || diagnosticType == typeROT)
        {
            erotDump(id, dumpPath);
        }
        else if (diagnosticType == typeLTSSM)
        {
            retimerLtssmDump(id, dumpPath, retimerState.getVendorId());
        }
        else if (diagnosticType == typeRetimerRegister)
        {
            std::string retimer_address =
                std::get<std::string>(params["Address"]);
            retimerRegisterDump(id, dumpPath, retimer_address,
                                retimerState.getVendorId());
        }
        else if (diagnosticType == typeFwAtts)
        {
            fwAttrsDump(id, dumpPath);
        }
        else if (diagnosticType == typeHwCheckout)
        {
            hwCheckoutDump(id, dumpPath);
        }
        else
        {
            log<level::ERR>("System dump: Invalid DiagnosticType");
            elog<InternalFailure>();
        }
    }
    else if (pid > 0)
    {
        auto entryId = lastEntryId + 1;
        Child::Callback callback =
            [this, pid, entryId, diagnosticType](Child&, const siginfo_t* si) {
            if (si->si_status != 0)
            {
                std::string msg = "Dump process failed: (signo)" +
                                  std::to_string(si->si_signo) + "; (code)" +
                                  std::to_string(si->si_code) + "; (errno)" +
                                  std::to_string(si->si_errno) + "; (pid)" +
                                  std::to_string(si->si_pid) + "; (status)" +
                                  std::to_string(si->si_status);
                log<level::ERR>(msg.c_str());
                this->createDumpFailed(entryId);
            }

            this->childPtrMap.erase(pid);
            // Remove dumpType from dumpInProgress when dump ends
            Manager::dumpInProgress.erase(diagnosticType);
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
            Manager::dumpInProgress.erase(diagnosticType);
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
    std::regex file_regex("obmcdump_([0-9]+)_([0-9]+).([a-zA-Z0-9]+)");

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
        if (entryPtr)
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
        return;
    }
}

void Manager::watchCallback(const UserMap& fileInfo)
{
    for (const auto& i : fileInfo)
    {
        // For any new dump file create dump entry object
        // and associated inotify watch.
        if (IN_CLOSE_WRITE == i.second)
        {
            if (!std::filesystem::is_directory(i.first))
            {
                // Don't require filename to be passed, as the path
                // of dump directory is stored in the childWatchMap
                removeWatch(i.first.parent_path());
                // dump file is written now create D-Bus entry
                createEntry(i.first);
            }
            else
            {
                removeWatch(i.first);
            }
        }
        // Start inotify watch on newly created directory.
        else if ((IN_CREATE == i.second) && fs::is_directory(i.first))
        {
            auto watchObj = std::make_unique<Watch>(
                eventLoop, IN_NONBLOCK, IN_CLOSE_WRITE, EPOLLIN, i.first,
                std::bind(std::mem_fn(
                              &phosphor::dump::system::Manager::watchCallback),
                          this, std::placeholders::_1));

            childWatchMap.emplace(i.first, std::move(watchObj));
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
        if ((fs::is_directory(p.path())) &&
            std::all_of(idStr.begin(), idStr.end(), ::isdigit))
        {
            lastEntryId = std::max(lastEntryId,
                                   static_cast<uint32_t>(std::stoul(idStr)));
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

    auto size = 0;

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
    if (size > SYSTEM_DUMP_MAX_SIZE)
    {
        size = SYSTEM_DUMP_MAX_SIZE;
    }

    return size;
}

} // namespace system
} // namespace dump
} // namespace phosphor
