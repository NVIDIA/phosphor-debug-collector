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

#include "dump_manager_fdr.hpp"

#include "dump_utils.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/Dump/Create/error.hpp"

#include <fmt/core.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <sdeventplus/clock.hpp>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/base.hpp>
#include <sdeventplus/source/time.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <ctime>
#include <regex>
#include <sstream>
#include <string>

namespace phosphor
{
namespace dump
{
namespace FDR
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;


// Single-flight progress key — all code paths use this constant.
static constexpr auto fdrProgressKey = "FDR";

void Manager::limitDumpEntries()
{
    // Delete dumps only when FDR dump max limit is configured
#if FDR_DUMP_MAX_LIMIT == 0
    // Do nothing - FDR dump max limit is not configured
    return;
#else  // #if FDR_DUMP_MAX_LIMIT == 0
    // Delete dumps on reaching allowed entries
    auto totalDumps = entries.size();
    if (totalDumps < FDR_DUMP_MAX_LIMIT)
    {
        // Do nothing - Its within allowed entries
        return;
    }
    // Get the oldest dumps
    size_t excessDumps = totalDumps - (FDR_DUMP_MAX_LIMIT - 1);
    // Delete the oldest dumps
    auto d = entries.begin();
    while (d != entries.end() && excessDumps != 0U)
    {
        auto& entry = d->second;
        // Defensive guard: never evict an active InProgress dump.
        // Belt-and-braces protection against silent under-deletion if a
        // stale InProgress entry ever appears (e.g. unexpected state after
        // an unmanaged PDC restart). Concurrent requests are rejected by the
        // busy check in createDump() before reaching this function.
        if (entry->status() ==
            phosphor::dump::OperationStatus::InProgress)
        {
            ++d;
            continue;
        }
        d++;
        entry->delete_();
        --excessDumps;
    }

    return;
#endif // #if FDR_DUMP_MAX_LIMIT == 0
}

sdbusplus::message::object_path Manager::createDump(
    phosphor::dump::DumpCreateParams params)
{
    // Default action is to collect the dump
    if (auto search = params.find("Action"); search == params.end())
    {
        params["Action"] = "Collect";
    }

    // Busy check applies to ALL actions — Clean/GenBirthCert share the same
    // global FDR key and would cause permanent lockout if allowed to run while
    // a Collect is in progress (they also race with Collect over shared scratch
    // files). Check before the action-type branch so no action can bypass it.
    const std::string progressKey{fdrProgressKey};
    if (dumpInProgress.contains(progressKey))
    {
        log<level::WARNING>("FDR dump: another dump already in progress");
        elog<Unavailable>();
    }

    // Handle other actions like clear log and generate certificates
    auto dumpAction = std::get<std::string>(params["Action"]);
    if (dumpAction != "Collect")
    {
        triggerFDRDumpScript(params);
        return fs::path(baseEntryPath).string();
    }

    // For Collect: limitDumpEntries() must run after the busy check above
    // (already done). With FDR_DUMP_MAX_LIMIT == 1, running it before the
    // check would evict the active InProgress entry.

    // Safe to apply retention now — no active InProgress entry will be evicted.
    limitDumpEntries();
    auto id = triggerFDRDumpScript(params);

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
            id, std::make_unique<FDR::Entry>(
                    bus, objPath.c_str(), id, timeStamp, 0, std::string(),
                    phosphor::dump::OperationStatus::InProgress, originatorId,
                    originatorType, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating FDR dump entry",
                        entry("OBJECTPATH=%s", objPath.c_str()),
                        entry("ID=%d", id));
        elog<InternalFailure>();
    }

    return objPath.string();
}

// NOLINTBEGIN
uint32_t fdrDump(phosphor::dump::DumpCreateParams params)
{
    // Construct FDR dump arguments
    std::vector<char*> arg_v;
    std::string fPath = FDR_DUMP_BIN_PATH;
    std::string time_start, time_end, max_dump_size, extended_source;

    arg_v.push_back(&fPath[0]);

    arg_v.push_back(const_cast<char*>("-p"));
    auto dump_path = std::get<std::string>(params["DumpPath"]);
    arg_v.push_back(const_cast<char*>(dump_path.c_str()));

    arg_v.push_back(const_cast<char*>("-i"));
    auto dump_id = std::get<std::string>(params["DumpID"]);
    arg_v.push_back(const_cast<char*>(dump_id.c_str()));

    arg_v.push_back(const_cast<char*>("-a"));
    auto dump_action = std::get<std::string>(params["Action"]);
    std::transform(dump_action.begin(), dump_action.end(), dump_action.begin(),
                   ::tolower);
    arg_v.push_back(const_cast<char*>(dump_action.c_str()));

    if (auto search = params.find("TimeRangeStart"); search != params.end())
    {
        if (std::holds_alternative<std::string>(search->second))
        {
            arg_v.push_back(const_cast<char*>("-s"));
            time_start = std::get<std::string>(params["TimeRangeStart"]);
            arg_v.push_back(const_cast<char*>(time_start.c_str()));
        }
    }

    if (auto search = params.find("TimeRangeEnd"); search != params.end())
    {
        if (std::holds_alternative<std::string>(search->second))
        {
            arg_v.push_back(const_cast<char*>("-e"));
            time_end = std::get<std::string>(params["TimeRangeEnd"]);
            arg_v.push_back(const_cast<char*>(time_end.c_str()));
        }
    }

    if (auto search = params.find("MaxDumpSize"); search != params.end())
    {
        if (std::holds_alternative<std::string>(search->second))
        {
            arg_v.push_back(const_cast<char*>("-m"));
            max_dump_size = std::get<std::string>(params["MaxDumpSize"]);
            arg_v.push_back(const_cast<char*>(max_dump_size.c_str()));
        }
    }

    if (auto search = params.find("ExtendedSource"); search != params.end())
    {
        if (std::holds_alternative<std::string>(search->second))
        {
            extended_source = std::get<std::string>(params["ExtendedSource"]);
        }
    }

    // Append validated DataFilter to ExtendedSource for fdr_dump.sh via -S.
    // Validation is done in triggerFDRDumpScript() (parent, before fork)
    // so InvalidArgument propagates as a D-Bus error to bmcweb.
    if (auto search = params.find("DataFilter"); search != params.end())
    {
        if (std::holds_alternative<std::string>(search->second))
        {
            auto dataFilter = std::get<std::string>(params["DataFilter"]);
            if (!extended_source.empty())
                extended_source += ";";
            extended_source += "DataFilter=" + dataFilter;
        }
    }

    if (!extended_source.empty())
    {
        arg_v.push_back(const_cast<char*>("-S"));
        arg_v.push_back(const_cast<char*>(extended_source.c_str()));
    }

    arg_v.push_back(nullptr);

    execv(arg_v[0], &arg_v[0]);

    // execv() returned — execution failed.  This runs in the child process;
    // use _exit() to avoid running PDC destructors which would close D-Bus
    // file descriptors shared with the parent.
    auto error = errno;
    log<level::ERR>(
        "FDR dump: exec failed in child (before _exit)",
        entry("ERRNO=%d", error));
    _exit(EXIT_FAILURE);
}
// NOLINTEND

void Manager::killAndCleanup(pid_t pid, pid_t pgid, uint32_t entryId,
                              const std::string& progressKey)
{
    kill(-pgid, SIGKILL);
    // Register a minimal cleanup Child source for async reap so we do not
    // block the event-loop thread with waitpid().
    try
    {
        // WEXITED only — not WSTOPPED. A cleanup source must not fire on
        // CLD_STOPPED; the child has not exited and erasing childPtrMap
        // would leave it without a watcher to reap it later.
        childPtrMap.emplace(
            pid, std::make_unique<Child>(
                     eventLoop.get(), pid, WEXITED,
                     [this, pid, entryId](Child&, const siginfo_t*) {
                         this->childPtrMap.erase(pid);
                         this->entryCompletionMap.erase(entryId);
                     }));
    }
    catch (...)
    {
        log<level::ERR>("FDR dump: cleanup Child source failed — brief zombie possible");
    }
    entryCompletionMap.erase(entryId);
    dumpInProgress.erase(progressKey);
}

uint32_t Manager::triggerFDRDumpScript(phosphor::dump::DumpCreateParams params)
{
    // check if minimum required space is available on destination partition
    std::error_code ec{};
    fs::path partitionPath(dumpDir);

    auto dumpAction = std::get<std::string>(params["Action"]);
    if (dumpAction == "Collect")
    {
#if (JFFS_SPACE_CALC_INACCURACY_OFFSET_WORKAROUND_PERCENT > 0)
        /* jffs2 space available problem is worked around by substracting 2%
           of capacity from currently available space, eg. 200M - 4M = 196M
           it solves problem of failed dump when user request it close to space
           limit so instead if silently failing the task user receives
           appropriate message. Test it yourself - fill up the partition until
           'no space left' message appears, check `df -T` for available space,
           if there seems to be at least 1% space available then you just
           reproduced the issue*/
        uintmax_t offset =
            (fs::space(partitionPath, ec).capacity *
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
        uintmax_t reqSizeKb = FDR_DUMP_MIN_SPACE_REQD;

        if (ec.value() != 0)
        {
            log<level::ERR>("Failed to check available space");
            elog<InternalFailure>();
        }

        if (sizeLeftKb < reqSizeKb)
        {
            log<level::ERR>(
                "Not enough space available to create FDR dump",
                entry("REQ_KB=%d", static_cast<unsigned int>(reqSizeKb)),
                entry("LEFT_KB=%d", static_cast<unsigned int>(sizeLeftKb)));
            using QuotaExceeded = sdbusplus::xyz::openbmc_project::Dump::
                Create::Error::QuotaExceeded;
            using Reason =
                xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
            elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
        }
    }

    // Validate request argument
    const std::string typeFDR = "FDR";
    auto diagnosticType = std::get<std::string>(params["DiagnosticType"]);
    params.erase("DiagnosticType");
    if (!diagnosticType.empty())
    {
        if (diagnosticType != typeFDR)
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
    }
    else
    {
        log<level::ERR>("Empty DiagnosticType option",
                        entry("DIAG_TYPE=%s", diagnosticType.c_str()));
        using INV_ARG =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME;
        using INV_VAL =
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE;
        elog<InvalidArgument>(INV_ARG("DiagnosticType"),
                              INV_VAL(diagnosticType.c_str()));
    }

    // Validate DataFilter before fork() so InvalidArgument propagates as a
    // D-Bus error to bmcweb (HTTP 400) rather than causing an async task
    // failure after 202 is already sent. Follows same pattern as the
    // DiagnosticType validation above.
    if (auto search = params.find("DataFilter"); search != params.end())
    {
        if (std::holds_alternative<std::string>(search->second))
        {
            auto dataFilter = std::get<std::string>(search->second);
            std::vector<std::string> validFilters;
            std::istringstream ss(
                std::string(FDR_DUMP_OEM_DIAGNOSTIC_ALLOWABLE_TYPE));
            std::string tok;
            while (std::getline(ss, tok, ','))
            {
                auto pos = tok.find("DataFilter=");
                if (pos != std::string::npos)
                    validFilters.push_back(tok.substr(pos + 11));
            }
            if (dataFilter.empty() ||
                std::find(validFilters.begin(), validFilters.end(),
                          dataFilter) == validFilters.end())
            {
                log<level::ERR>("Invalid or unsupported DataFilter value",
                                entry("DATAFILTER=%s", dataFilter.c_str()));
                using INV_ARG = xyz::openbmc_project::Common::
                    InvalidArgument::ARGUMENT_NAME;
                using INV_VAL = xyz::openbmc_project::Common::
                    InvalidArgument::ARGUMENT_VALUE;
                elog<InvalidArgument>(INV_ARG("DataFilter"),
                                      INV_VAL(dataFilter.c_str()));
            }
        }
    }

    log<level::INFO>(
        fmt::format("Capturing FDR dump of type ({})", diagnosticType).c_str());

    // Insert key here — after all validation — so that pre-fork validation
    // throws (QuotaExceeded, InvalidArgument) cannot leave a stale key.
    // Erased on all error paths below and in Child::Callback on completion.
    const std::string progressKey{fdrProgressKey};
    dumpInProgress.insert(progressKey);
    log<level::INFO>(
        fmt::format("FDR dump: dumpInProgress inserted key={}", progressKey).c_str());

    pid_t pid = fork();

    if (pid == 0)
    {
        // Child: establish process group race-safely.
        // EPERM = parent called setpgid first (group exists, OK).
        if (setpgid(0, 0) < 0 && errno != EPERM)
        {
            log<level::ERR>("FDR dump: setpgid failed in child");
            _exit(EXIT_FAILURE);
        }

        fs::path dumpPath(dumpDir);
        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        if (diagnosticType == typeFDR)
        {
            params["DumpID"] = id;
            params["DumpPath"] = dumpPath;
            fdrDump(params);
        }
        else
        {
            log<level::ERR>("FDR dump: Invalid DiagnosticType");
            _exit(EXIT_FAILURE);
        }
        // fdrDump() calls execv() and only returns on failure; _exit in fdrDump
        _exit(EXIT_FAILURE);
    }
    else if (pid > 0)
    {
        // Parent: establish process group race-safely.
        // EPERM/EACCES/ESRCH are noted but acceptance is gated on PGID
        // verification below — not on setpgid() return value alone.
        if (setpgid(pid, pid) < 0)
        {
            int e = errno;
            if (e != EPERM && e != EACCES && e != ESRCH)
            {
                log<level::ERR>(
                    fmt::format("FDR dump: parent setpgid failed errno={}", e).c_str());
                // PGID not verified — kill only the direct child, not the group.
                // Group kill is forbidden until getpgid(pid)==pid is confirmed.
                // kill(pid) is called here directly; a WEXITED-only Child source
                // is registered below to reap the child asynchronously.
                kill(pid, SIGKILL);
                try
                {
                    childPtrMap.emplace(
                        pid,
                        std::make_unique<Child>(
                            eventLoop.get(), pid, WEXITED,
                            [this, pid, entryId = lastEntryId + 1u](
                                Child&, const siginfo_t*) {
                                this->childPtrMap.erase(pid);
                                this->entryCompletionMap.erase(entryId);
                            }));
                }
                catch (...)
                {
                    log<level::ERR>(
                        "FDR dump: cleanup Child source failed after setpgid error");
                }
                dumpInProgress.erase(progressKey);
                elog<InternalFailure>();
            }
            log<level::DEBUG>(
                fmt::format("FDR dump: parent setpgid errno={} observed; PGID verification follows", e).c_str());
        }

        // Verify PGID: EPERM/EACCES/ESRCH from setpgid() are only accepted
        // as benign after getpgid(pid)==pid confirms correct group assignment.
        // If verification fails, kill the child rather than use a bad pgid.
        pid_t pgid = pid;
        pid_t actualPgid = getpgid(pid);
        if (actualPgid < 0 || actualPgid != pid)
        {
            log<level::WARNING>(
                fmt::format(
                    "FDR dump: pgid mismatch expected={} got={} — setup error, killing child",
                    pid, actualPgid)
                    .c_str());
            kill(pid, SIGKILL);
            // Register WEXITED-only cleanup watcher to reap the child
            // asynchronously — same pattern as the setpgid error path.
            try
            {
                childPtrMap.emplace(
                    pid,
                    std::make_unique<Child>(
                        eventLoop.get(), pid, WEXITED,
                        [this, pid,
                         entryId = lastEntryId + 1u](Child&,
                                                     const siginfo_t*) {
                            this->childPtrMap.erase(pid);
                            this->entryCompletionMap.erase(entryId);
                        }));
            }
            catch (...)
            {
                log<level::ERR>(
                    "FDR dump: cleanup Child after pgid mismatch failed"
                    " — brief zombie possible");
            }
            dumpInProgress.erase(progressKey);
            elog<InternalFailure>();
        }
        log<level::DEBUG>(
            fmt::format("FDR dump: pgid verified pgid={} — setpgid race outcome accepted", pgid).c_str());

        auto entryId = lastEntryId + 1;
        entryCompletionMap[entryId] = EntryCompletionState{};
        // Only Collect expects an archive; Clean/GenBirthCert clear the key
        // on terminal child exit without waiting for IN_CLOSE_WRITE.
        entryCompletionMap[entryId].expectsArchive = (dumpAction == "Collect");

        // One-shot deadline timer: sdeventplus monotonic-clock source.
        // Shared with Child::Callback so either can cancel the other.
        using TimerType =
            sdeventplus::source::Time<sdeventplus::ClockId::Monotonic>;
        std::shared_ptr<TimerType> timer;
        try
        {
            timer = std::make_shared<TimerType>(
                eventLoop.get(),
                sdeventplus::Clock<sdeventplus::ClockId::Monotonic>(
                    eventLoop.get())
                        .now() +
                    std::chrono::seconds(FDR_DUMP_TIMEOUT_SECONDS),
                std::chrono::seconds(1),
                [this, pgid, progressKey](TimerType& source,
                                          TimerType::TimePoint) {
                    // One-shot guard: disable self before kill to prevent
                    // repeated SIGKILL on recycled PIDs.
                    source.set_enabled(sdeventplus::source::Enabled::Off);
                    log<level::WARNING>(
                        fmt::format(
                            "FDR dump: timeout fired, killing process "
                            "group pgid={}",
                            pgid)
                            .c_str());
                    if (kill(-pgid, SIGKILL) < 0)
                    {
                        if (errno == ESRCH)
                        {
                            log<level::DEBUG>(
                                fmt::format(
                                    "FDR dump: kill ESRCH - process group "
                                    "already gone (race with natural exit) pgid={}",
                                    pgid)
                                    .c_str());
                        }
                        else
                        {
                            log<level::ERR>(
                                fmt::format("FDR dump: kill failed errno={}",
                                            errno)
                                    .c_str());
                        }
                    }
                    log<level::ERR>(
                        fmt::format(
                            "FDR dump: process group killed by timeout pgid={}",
                            pgid)
                            .c_str());
                    // Entry stays in Terminating state; dumpInProgress key
                    // remains set. Child::Callback erases both on SIGCHLD.
                });
            log<level::INFO>(
                fmt::format(
                    "FDR dump: deadline timer armed timeout={}s pid={}",
                    static_cast<uint64_t>(FDR_DUMP_TIMEOUT_SECONDS), pid)
                    .c_str());
        }
        catch (const std::exception& ex)
        {
            log<level::ERR>(fmt::format("FDR dump: timer creation failed: {}", ex.what()).c_str());
            killAndCleanup(pid, pgid, entryId, progressKey);
            elog<InternalFailure>();
        }

        Child::Callback callback =
            [this, pid, pgid, entryId, progressKey,
             timer](Child& source, const siginfo_t* si) {
                // Filter non-terminal signals — do not clean up.
                if (si->si_code == CLD_STOPPED ||
                    si->si_code == CLD_CONTINUED)
                {
                    log<level::INFO>(
                        "FDR dump: child stopped (SIGSTOP) - not terminal, "
                        "timer remains armed");
                    // sdeventplus::source::Child defaults to OneShot.
                    // Rearm so the terminal SIGCHLD (from timeout kill or
                    // natural exit) is not silently dropped.
                    source.set_enabled(
                        sdeventplus::source::Enabled::OneShot);
                    return;
                }

                // Cancel timer FIRST (PID reuse protection) on every
                // terminal path.
                if (timer)
                {
                    timer->set_enabled(sdeventplus::source::Enabled::Off);
                }

                if (si->si_status == 0)
                {
                    // Use find() not operator[] — operator[] silently creates
                    // a default EntryCompletionState (expectsArchive=false)
                    // which would route a stray Collect callback through the
                    // non-Collect branch and erase another dump's key.
                    auto stateIt = entryCompletionMap.find(entryId);
                    if (stateIt == entryCompletionMap.end())
                    {
                        this->childPtrMap.erase(pid);
                        return;
                    }
                    auto& state = stateIt->second;

                    if (state.archiveError)
                    {
                        // createEntry() already marked entry Failed due to
                        // file_size error. Remove the entry directory so
                        // restore() cannot recreate it as Completed after
                        // PDC restart and quota is reclaimed.
                        std::error_code rmEc;
                        fs::path entryDir =
                            fs::path(dumpDir) / std::to_string(entryId);
                        removeWatch(entryDir);
                        fs::remove_all(entryDir, rmEc);
                        if (rmEc)
                        {
                            log<level::ERR>(
                                fmt::format(
                                    "FDR dump: failed to remove entry dir {}: {}",
                                    entryDir.string(), rmEc.message())
                                    .c_str());
                        }
                        entryCompletionMap.erase(entryId);
                        this->childPtrMap.erase(pid);
                        dumpInProgress.erase(progressKey);
                        log<level::INFO>(
                            fmt::format("FDR dump: terminal cleanup after archiveError pid={}", pid).c_str());
                        return;
                    }

                    if (!state.expectsArchive)
                    {
                        // Non-Collect (Clean/GenBirthCert): no archive is
                        // produced; release key immediately on clean exit.
                        entryCompletionMap.erase(entryId);
                        this->childPtrMap.erase(pid);
                        dumpInProgress.erase(progressKey);
                        log<level::INFO>(
                            fmt::format("FDR dump: non-Collect action completed pid={}", pid).c_str());
                    }
                    else if (state.archiveReady)
                    {
                        // Both signals present: mark Completed.
                        // Use archiveTimestamp from filename (microseconds)
                        // so result is independent of callback dispatch order.
                        auto dumpEntry = entries.find(entryId);
                        if (dumpEntry != entries.end())
                        {
                            dynamic_cast<phosphor::dump::FDR::Entry*>(
                                dumpEntry->second.get())
                                ->update(state.archiveTimestamp,
                                         state.archiveSize, state.archivePath);
                        }
                        entryCompletionMap.erase(entryId);
                        this->childPtrMap.erase(pid);
                        dumpInProgress.erase(progressKey);
                        log<level::INFO>(
                            fmt::format("FDR dump: Completed (archiveReady+exit0) pid={}", pid).c_str());
                    }
                    else
                    {
                        // IN_CLOSE_WRITE has not arrived yet. fdr_dump.sh
                        // runs tar synchronously so the archive must be on
                        // disk already. Scan <dumpDir>/<entryId>/ rather
                        // than waiting indefinitely (which would hold the
                        // busy key forever if the inotify event is missed).
                        this->childPtrMap.erase(pid);
                        fs::path scanPath(dumpDir);
                        scanPath /= std::to_string(entryId);
                        bool archiveFound = false;

                        // Cleanup runs unconditionally — even if the scan
                        // throws (e.g. I/O error, permission denied), we
                        // must not leave dumpInProgress set permanently.
                        try
                        {
                            // Match only the expected archive filename:
                            // obmcdump_<entryId>_<timestamp>.tar
                            static std::regex archiveRe(
                                "^obmcdump_([0-9]+)_([0-9]+)\\.tar$");
                            if (fs::exists(scanPath))
                            {
                                for (const auto& p :
                                     fs::directory_iterator(scanPath))
                                {
                                    if (fs::is_directory(p))
                                        continue;
                                    std::string fname =
                                        p.path().filename().string();
                                    std::smatch m;
                                    if (std::regex_match(fname, m,
                                                         archiveRe) &&
                                        stoull(m[1]) == entryId)
                                    {
                                        state.archivePath = p.path();
                                        state.archiveSize =
                                            fs::file_size(p);
                                        uint64_t archiveTs =
                                            stoull(m[2]) * 1000 * 1000;
                                        auto dumpEntry =
                                            entries.find(entryId);
                                        if (dumpEntry != entries.end())
                                        {
                                            dynamic_cast<
                                                phosphor::dump::FDR::Entry*>(
                                                dumpEntry->second.get())
                                                ->update(archiveTs,
                                                         state.archiveSize,
                                                         state.archivePath);
                                        }
                                        archiveFound = true;
                                        break;
                                    }
                                }
                            }
                        }
                        catch (const std::exception& ex)
                        {
                            log<level::ERR>(
                                fmt::format(
                                    "FDR dump: filesystem scan failed: {}"
                                    " — marking Failed",
                                    ex.what())
                                    .c_str());
                        }

                        if (archiveFound)
                        {
                            log<level::INFO>(
                                fmt::format(
                                    "FDR dump: Completed (filesystem scan) pid={}",
                                    pid)
                                    .c_str());
                        }
                        else
                        {
                            log<level::ERR>(
                                fmt::format(
                                    "FDR dump: exit(0) but no archive in {}"
                                    " — marking Failed",
                                    scanPath.string())
                                    .c_str());
                            this->createDumpFailed(static_cast<int>(entryId));
                            std::error_code rmEc;
                            removeWatch(scanPath);
                            fs::remove_all(scanPath, rmEc);
                            if (rmEc)
                            {
                                log<level::ERR>(
                                    fmt::format(
                                        "FDR dump: failed to remove entry dir {}: {}",
                                        scanPath.string(), rmEc.message())
                                        .c_str());
                            }
                        }
                        // Unconditional cleanup — key must be released
                        // even when scan threw an exception.
                        entryCompletionMap.erase(entryId);
                        dumpInProgress.erase(progressKey);
                    }
                }
                else
                {
                    // Non-zero exit or killed by signal (including timeout
                    // SIGKILL): Failed, no archive. Remove the entry directory
                    // so restore() cannot recreate a partial tar as Completed
                    // after PDC restart and quota is reclaimed.
                    std::string msg =
                        "Dump process failed: (signo)" +
                        std::to_string(si->si_signo) + "; (code)" +
                        std::to_string(si->si_code) + "; (errno)" +
                        std::to_string(si->si_errno) + "; (pid)" +
                        std::to_string(si->si_pid) + "; (status)" +
                        std::to_string(si->si_status);
                    log<level::ERR>(msg.c_str());
                    this->createDumpFailed(static_cast<int>(entryId));
                    std::error_code rmEc;
                    fs::path entryDir =
                        fs::path(dumpDir) / std::to_string(entryId);
                    removeWatch(entryDir);
                    fs::remove_all(entryDir, rmEc);
                    if (rmEc)
                    {
                        log<level::ERR>(
                            fmt::format(
                                "FDR dump: failed to remove entry dir {}: {}",
                                entryDir.string(), rmEc.message())
                                .c_str());
                    }
                    entryCompletionMap.erase(entryId);
                    this->childPtrMap.erase(pid);
                    dumpInProgress.erase(progressKey);
                    log<level::INFO>(
                        fmt::format(
                            "FDR dump: dumpInProgress removed key={} "
                            "reason=non-zero-exit-or-killed",
                            progressKey)
                            .c_str());
                }
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
            log<level::ERR>(
                fmt::format("Error occurred during the sdeventplus::source::Child creation ex({})", ex.what()).c_str());
            killAndCleanup(pid, pgid, entryId, progressKey);
            elog<InternalFailure>();
        }
    }
    else
    {
        auto error = errno;
        log<level::ERR>("FDR dump: Error occurred during fork",
                        entry("ERRNO=%d", error));
        dumpInProgress.erase(progressKey);
        elog<InternalFailure>();
    }

    return ++lastEntryId;
}

void Manager::createEntry(const fs::path& file)
{
    // Dump File Name format obmcdump_ID_EPOCHTIME.EXT
    static constexpr auto ID_POS = 1;
    static constexpr auto EPOCHTIME_POS = 2;
    static std::regex file_regex("obmcdump_([0-9]+)_([0-9]+)\\.([a-zA-Z0-9]+)");

    std::smatch match;
    std::string name = file.filename();

    if (!((std::regex_search(name, match, file_regex)) && (match.size() > 0)))
    {
        log<level::ERR>("FDR dump: Invalid Dump file name",
                        entry("FILENAME=%s", file.filename().c_str()));
        return;
    }

    auto idString = match[ID_POS];
    uint64_t timestamp = stoull(match[EPOCHTIME_POS]) * 1000 * 1000;

    auto id = stoul(idString);

    // inotify-first path: set archiveReady and return. Child::Callback will
    // find archiveReady=true and mark Completed.
    // exit-first path: Child::Callback finds archiveReady=false and performs
    // a filesystem scan to locate the archive directly.
    auto dumpEntry = entries.find(id);
    if (dumpEntry != entries.end())
    {
        auto stateIt = entryCompletionMap.find(id);
        if (stateIt != entryCompletionMap.end())
        {
            // Active in-flight dump: record archive metadata and signal
            // Child::Callback that the archive is ready.
            auto& state = stateIt->second;
            std::error_code sizeEc;
            auto archiveSize = fs::file_size(file, sizeEc);
            if (sizeEc)
            {
                log<level::ERR>(
                    fmt::format(
                        "FDR dump: file_size failed for {} ec={} — marking Failed",
                        file.string(), sizeEc.message())
                        .c_str());
                // Mark entry Failed but keep key and completion state alive
                // until the terminal Child::Callback fires. Erasing the key
                // here would allow a new request to start while the child
                // process and timer are still active.
                createDumpFailed(static_cast<int>(id));
                stateIt->second.archiveError = true;
                return;
            }
            state.archivePath = file;
            state.archiveSize = archiveSize;
            state.archiveTimestamp = timestamp;  // microseconds from filename
            state.archiveReady = true;

            // Child::Callback will complete the entry when it fires.
            log<level::INFO>(
                fmt::format(
                    "FDR dump: archiveReady set, waiting for "
                    "Child::Callback entry={}",
                    id)
                    .c_str());
        }
        else
        {
            // No coordination state means the terminal Child::Callback has
            // already resolved this entry as Completed or Failed and erased
            // entryCompletionMap. This IN_CLOSE_WRITE is a late or duplicate
            // event arriving after the dump concluded. Never call update()
            // here: doing so would silently override a Failed result with
            // Completed + archive, violating the SADD guarantee that timeout
            // and non-zero exit remain Failed with no archive offered.
            // restore() does not reach this branch — it creates entries from
            // scratch via the entries.insert() path below.
            log<level::INFO>(
                fmt::format(
                    "FDR dump: late or duplicate IN_CLOSE_WRITE for already-resolved entry={} — ignored",
                    id)
                    .c_str());
        }
        return;
    }

    // Entry Object path.
    auto objPath = fs::path(baseEntryPath) / std::to_string(id);

    try
    {
        // Get the originator id and type from params
        std::string originatorId;
        originatorTypes originatorType;
        entries.insert(std::make_pair(
            id, std::make_unique<FDR::Entry>(
                    bus, objPath.c_str(), id, timestamp, fs::file_size(file),
                    file, phosphor::dump::OperationStatus::Completed,
                    originatorId, originatorType, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(e.what());
        log<level::ERR>("Error in creating FDR dump entry",
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
                std::bind(
                    std::mem_fn(&phosphor::dump::FDR::Manager::watchCallback),
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

    uintmax_t size = 0;

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

    size = (size > FDR_DUMP_TOTAL_SIZE ? 0 : FDR_DUMP_TOTAL_SIZE - size);

    if (size < FDR_DUMP_MIN_SPACE_REQD)
    {
        // Reached to maximum limit
        elog<QuotaExceeded>(Reason("Not enough space: Delete old dumps"));
    }
    if (size > FDR_DUMP_MAX_SIZE)
    {
        size = FDR_DUMP_MAX_SIZE;
    }

    return static_cast<size_t>(size);
}

} // namespace FDR
} // namespace dump
} // namespace phosphor
