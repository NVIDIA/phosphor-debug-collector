#include "config.h"

#include "dump_manager_bmc.hpp"

#include "bmc_dump_entry.hpp"
#include "dump-extensions/nvidia-dumps/oem_dump_utils.hpp"
#include "dump_types.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdeventplus/exception.hpp>
#include <sdeventplus/source/child.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace bmc
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace phosphor::logging;

namespace
{

constexpr auto containerRuntimeMetricsDumpBin =
    "/usr/bin/container_runtime_metrics_dump.sh";

std::string handlerPathFor(const std::string& diagnosticType)
{
    if (diagnosticType == "ContainerRuntimeMetrics")
    {
        return containerRuntimeMetricsDumpBin;
    }
    return {};
}

bool isTypeAllowed(const std::string& diagnosticType)
{
    const std::string prefix = "DiagnosticType=" + diagnosticType + ";";
    for (const auto& value : bmcOemAllowableValues())
    {
        if (value.starts_with(prefix) ||
            value == "DiagnosticType=" + diagnosticType)
        {
            return true;
        }
    }
    return false;
}

bool isCombinationAllowed(const std::string& diagnosticType,
                          const std::string& duration,
                          const std::string& samplingRate)
{
    const std::string requested =
        "DiagnosticType=" + diagnosticType + ";Duration=" + duration +
        ";SamplingRate=" + samplingRate;
    const auto& values = bmcOemAllowableValues();
    return std::find(values.begin(), values.end(), requested) != values.end();
}

bool isPositiveIntInRange(const std::string& s, long min, long max)
{
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    try
    {
        long v = std::stol(s);
        return v >= min && v <= max;
    }
    catch (...)
    {
        return false;
    }
}

using BmcEntryServer = sdbusplus::xyz::openbmc_project::Dump::Entry::server::BMC;
using AdditionalType = BmcEntryServer::AdditionalType;

AdditionalType additionalTypeFor(const std::string& diagnosticType)
{
    if (diagnosticType == "ContainerRuntimeMetrics")
    {
        return AdditionalType::ContainerRuntimeMetrics;
    }
    return AdditionalType::None;
}

} // namespace

std::optional<sdbusplus::message::object_path> Manager::createVhmcOemDump(
    phosphor::dump::DumpCreateParams& params, const std::string& originatorId,
    originatorTypes originatorType)
{
    std::string diagnosticType = lookupCreateParam(params, "DiagnosticType");
    if (diagnosticType.empty())
    {
        return std::nullopt;
    }

    if (!isTypeAllowed(diagnosticType))
    {
        lg2::error("Unsupported BMC dump DiagnosticType: {TYPE}", "TYPE",
                   diagnosticType);
        std::string invalidValue = "DiagnosticType=" + diagnosticType;
        elog<InvalidArgument>(
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME(
                "OEMDiagnosticDataType"),
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE(
                invalidValue.c_str()));
    }

    std::string handlerPath = handlerPathFor(diagnosticType);
    if (handlerPath.empty())
    {
        lg2::error("BMC dump DiagnosticType {TYPE} is allowlisted but has no "
                   "mapped handler",
                   "TYPE", diagnosticType);
        elog<InternalFailure>();
    }

    std::string duration = lookupCreateParam(params, "Duration");
    std::string samplingRate = lookupCreateParam(params, "SamplingRate");
    if (!isPositiveIntInRange(duration, 1, bmcDumpMaxTimeLimitInSec) ||
        !isPositiveIntInRange(samplingRate, 1, std::stol(duration)) ||
        !isCombinationAllowed(diagnosticType, duration, samplingRate))
    {
        lg2::error("Unsupported Duration(s)/SamplingRate(s) for BMC OEM dump: "
                   "{DUR}/{RATE}",
                   "DUR", duration, "RATE", samplingRate);
        std::string invalidValue = "DiagnosticType=" + diagnosticType +
                                   ";Duration=" + duration +
                                   ";SamplingRate=" + samplingRate;
        elog<InvalidArgument>(
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_NAME(
                "OEMDiagnosticDataType"),
            xyz::openbmc_project::Common::InvalidArgument::ARGUMENT_VALUE(
                invalidValue.c_str()));
    }

    if (vhmcOemDumpInProgress.contains(diagnosticType))
    {
        lg2::info("A {TYPE} collection is already in progress", "TYPE",
                  diagnosticType);
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    auto allowedSize = getAllowedSize();

    vhmcOemDumpInProgress.insert(diagnosticType);
    auto id = captureVhmcOemDump(handlerPath, diagnosticType, duration,
                                 samplingRate, allowedSize);

    auto objPath = std::filesystem::path(baseEntryPath) / std::to_string(id);
    try
    {
        uint64_t timeStamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        auto entry = std::make_unique<bmc::Entry>(
            bus, objPath.c_str(), id, timeStamp, 0, std::string(),
            phosphor::dump::OperationStatus::InProgress, originatorId,
            originatorType, *this);
        entry->additionalTypeName(additionalTypeFor(diagnosticType));
        entries.insert(std::make_pair(id, std::move(entry)));
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error("Error in creating dump entry, errormsg: {ERROR}, "
                   "OBJECTPATH: {OBJECT_PATH}, ID: {ID}",
                   "ERROR", e, "OBJECT_PATH", objPath, "ID", id);
        elog<InternalFailure>();
    }

    return sdbusplus::message::object_path(objPath.string());
}

uint32_t Manager::captureVhmcOemDump(
    const std::string& handlerPath, const std::string& diagnosticType,
    const std::string& duration, const std::string& samplingRate,
    size_t allowedSize)
{

    lg2::info("Capturing BMC OEM diagnostic dump: {TYPE}", "TYPE",
              diagnosticType);

    pid_t pid = fork();

    if (pid == 0)
    {
        std::filesystem::path dumpPath(dumpDir);
        auto id = std::to_string(lastEntryId + 1);
        dumpPath /= id;

        std::vector<std::string> args = {
            handlerPath, "-p",         dumpPath.string(),
            "-i",        id,           "-d",
            duration,    "-s",         samplingRate,
            "-S",        std::to_string(allowedSize)};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args)
        {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        execv(handlerPath.c_str(), argv.data());

        auto error = errno;
        lg2::error("Error exec'ing OEM diagnostic dump handler for {TYPE}, "
                   "errno: {ERRNO}",
                   "TYPE", diagnosticType, "ERRNO", error);
        _exit(EXIT_FAILURE);
    }
    else if (pid > 0)
    {
        auto entryId = lastEntryId + 1;
        Child::Callback callback = [this, pid, entryId,
                                    diagnosticType](Child&,
                                                    const siginfo_t* si) {
            // WSTOPPED also notifies on stop/continue; only exit is terminal.
            if (si->si_code != CLD_EXITED && si->si_code != CLD_KILLED &&
                si->si_code != CLD_DUMPED)
            {
                return;
            }
            this->vhmcOemDumpInProgress.erase(diagnosticType);
            if (si->si_status != 0)
            {
                lg2::error(
                    "OEM diagnostic dump process failed: status {STATUS}",
                    "STATUS", si->si_status);
                this->createDumpFailed(static_cast<int>(entryId));
            }
            this->childPtrMap.erase(pid);
        };
        try
        {
            childPtrMap.emplace(pid, std::make_unique<Child>(
                                         eventLoop.get(), pid,
                                         WEXITED | WSTOPPED,
                                         std::move(callback)));
        }
        catch (const sdeventplus::SdEventError& ex)
        {
            lg2::error(
                "Error adding OEM diagnostic dump child to event loop: {ERROR}",
                "ERROR", ex.what());
            // Untracked child: stop and reap it so it cannot outlive the dump.
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            vhmcOemDumpInProgress.erase(diagnosticType);
            elog<InternalFailure>();
        }
    }
    else
    {
        auto error = errno;
        lg2::error("OEM diagnostic dump: fork failed, errno: {ERRNO}", "ERRNO",
                   error);
        vhmcOemDumpInProgress.erase(diagnosticType);
        elog<InternalFailure>();
    }

    return ++lastEntryId;
}

void Entry::serialize()
{
    phosphor::dump::Entry::serialize();

    auto path = file.parent_path() / PRESERVE / SERIAL_FILE;
    try
    {
        nlohmann::json j;
        {
            std::ifstream is(path, std::ios::binary);
            if (!is.is_open())
            {
                return;
            }
            is >> j;
        }
        j["additionalTypeName"] = additionalTypeName();
        std::ofstream os(path, std::ios::binary);
        if (os.is_open())
        {
            os << j.dump(4);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to persist AdditionalTypeName: {PATH} {ERROR}",
                   "PATH", path, "ERROR", e);
    }
}

void Entry::deserialize(const std::filesystem::path& dumpPath)
{
    phosphor::dump::Entry::deserialize(dumpPath);

    auto path = dumpPath / PRESERVE / SERIAL_FILE;
    try
    {
        std::ifstream is(path, std::ios::binary);
        if (!is.is_open())
        {
            return;
        }
        nlohmann::json j;
        is >> j;
        auto it = j.find("additionalTypeName");
        if (it != j.end())
        {
            additionalTypeName(it->get<AdditionalType>());
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to restore AdditionalTypeName: {PATH} {ERROR}",
                   "PATH", path, "ERROR", e);
    }
}

} // namespace bmc
} // namespace dump
} // namespace phosphor
