/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tool to collect PLDM OEM diagnostic events from SatMC.
 *
 * Legacy mode (default) collects a fixed bundle and decodes it into JSON:
 * - CPER Error Counters
 * - PCIe Root Port Static Data
 * - PCIe Root Port Performance Data
 *
 * PCore mode (-m) collects raw per-PCore dumps of one CPU package. It owns the
 * multi-PCore loop: one Collect call per selector, one staged payload copied
 * out per selector, and one archive holding every payload that arrived. The
 * payload is never decoded.
 */

#include "config.h"

#include "../dump-extensions/nvidia-dumps/tar_compress_lock.hpp"
#include "dump-extensions/nvidia-dumps/pcore_selectors.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define VERSION "1.1"
#define SLEEP_DURING_WAIT_SECONDS 1
#define DEFAULT_TIMEOUT_SECONDS 90
#define CMET_CHANNEL_COUNT 64
// Sanity cap only; the real host bridge count is derived from the payload size.
#define MAX_HOST_BRIDGES 8
#define MAX_ROOT_PORTS_PER_HB 8
#define PCIE_MAX_LANES_PER_RP 16

using json = nlohmann::json;
using namespace phosphor::logging;
namespace fs = std::filesystem;
namespace pcore = phosphor::dump::pcore;

// Event staging directory base path
constexpr auto EVENT_STAGING_BASE = "/var/lib/pldm_events";
constexpr auto PLDM_STATIC_CONFIG_PATH =
    "/usr/share/pldm/pldm_static_configuration.json";
constexpr int EXIT_CODE_TAR_WARNING = 1;

// Event file names (written by pldmd for OEM event classes)
// Files in /var/lib/pldm_events/<terminus>/
constexpr auto CPER_ERROR_COUNT_EVENT_FILE = "CPERErrorCount_0_0.bin";
// LTSSM event collection disabled - backend not ready
// constexpr auto PCIE_LTSSM_EVENT_FILE = "PCIeLTSSM_0_0.bin";
constexpr auto PCIE_TELEMETRY_EVENT_FILE = "PCIeTelemetry_0_0.bin";

#ifdef PCORE_DUMP
// PCore payloads are staged under a single fixed name, overwritten by every
// collection. Correlation to a request is by serialization plus the name this
// tool gives the copy, never by anything in the staged file itself.
constexpr auto PCORE_DUMP_EVENT_FILE = "PCoreDump_0_0.bin";
#endif // PCORE_DUMP

// D-Bus effecter paths
constexpr auto PLDM_SERVICE = "xyz.openbmc_project.PLDM";
constexpr auto CONTROL_TRIGGER_INTERFACE =
    "xyz.openbmc_project.Control.Trigger";
constexpr auto PROPERTIES_INTERFACE = "org.freedesktop.DBus.Properties";
constexpr auto MAPPER_SERVICE = "xyz.openbmc_project.ObjectMapper";
constexpr auto MAPPER_PATH = "/xyz/openbmc_project/object_mapper";
constexpr auto MAPPER_INTERFACE = "xyz.openbmc_project.ObjectMapper";

// Effecter name suffixes (appended to ProcessorModule_X_)
constexpr auto EFFECTER_CPER_ERROR_COUNT = "CPERErrorCount_0_0";
// LTSSM effecter disabled - backend not ready
// constexpr auto EFFECTER_PCIE_LTSSM = "PCIeLTSSM_0_0";
constexpr auto EFFECTER_PCIE_TELEMETRY = "PCIeTelemetry_0_0";

// Per-request working directories, one per mode, so a PCore collection and a
// legacy collection can never clean up each other's staging.
constexpr auto LEGACY_TEMP_SUBDIR = "CPUDiagnosticDump";
#ifdef PCORE_DUMP
constexpr auto PCORE_TEMP_SUBDIR = "PCoreDump";
#endif

#ifdef PCORE_DUMP
// Per-terminus lock serialising direct invocations against each other; the
// dump manager already serialises requests that come through it.
constexpr auto LOCK_DIR = "/run/lock";
#endif

// Exit codes. The dump manager fails the entry on any nonzero status; the
// distinct values exist so the journal names the failure.
enum ExitCode : int
{
    exitSuccess = 0,
    exitUsage = 1,
    exitNoEvents = 2,           // legacy mode: nothing collected
    exitAllSelectorsFailed = 3, // PCore mode: every selector was rejected
    exitAllSelectorsTimedOut = 4,
    exitArchiveFailed = 5,
};

struct PldmTarget
{
    std::string terminus;
    int eid = -1;
};

// Tool state
std::string tempPath;
std::string targetDevice;
std::string dumpPath;
std::string dumpID;
int timeoutSeconds = DEFAULT_TIMEOUT_SECONDS;
#ifdef PCORE_DUMP
bool pcoreMode = false;
std::string pcoreSelectorArg = pcore::allSelectorsToken;
#endif

// Mode-dependent values. Kept as functions so the PCore branch appears in
// exactly one place each on a build with the feature compiled out.
const char* modeTempSubdir()
{
#ifdef PCORE_DUMP
    return pcoreMode ? PCORE_TEMP_SUBDIR : LEGACY_TEMP_SUBDIR;
#else
    return LEGACY_TEMP_SUBDIR;
#endif
}

const char* modeName()
{
#ifdef PCORE_DUMP
    return pcoreMode ? "PCore" : "CPU diagnostic";
#else
    return "CPU diagnostic";
#endif
}

int modeNothingCollectedExit()
{
#ifdef PCORE_DUMP
    return pcoreMode ? exitAllSelectorsFailed : exitNoEvents;
#else
    return exitNoEvents;
#endif
}

// Event reception timestamps
std::string cperErrorCountReceivedTime;
// LTSSM reception timestamp disabled - backend not ready
// std::string pcieLtssmReceivedTime;
std::string pcieTelemetryReceivedTime;

// Link speed names
const std::map<uint8_t, std::string> linkSpeedNames = {
    {1, "Gen1"}, {2, "Gen2"}, {3, "Gen3"},
    {4, "Gen4"}, {5, "Gen5"}, {6, "Gen6"}};

// Payload structures (packed)
#pragma pack(push, 1)

// Per-CMET-channel info. The device emits these as an array of
// {count, status} pairs (cmet_info_channel[i]), NOT as two separate
// count[64]/status[64] arrays.
struct CmetChannelInfo
{
    uint32_t count;  // Errors found on this channel
    uint32_t status; // Channel status bitfield, see parseErrorCounterPayload()
};

struct ErrorCounterPayload
{
    uint32_t cpuCorrectedErrors;
    uint32_t uncoreCorrectedErrors;
    uint32_t cacheCorrectedErrors;
    uint32_t dramCorrectedErrors;
    uint32_t dramUncorrectedErrors;
    uint32_t pagesRetired;
    uint32_t otherSocCorrectedErrors;
    CmetChannelInfo cmetInfo[CMET_CHANNEL_COUNT];
    uint32_t cmetSpareCount;
};
static_assert(sizeof(ErrorCounterPayload) == 544);

#if 0
// LTSSM History data structure disabled - backend not ready
// LTSSM History data structure (per root port)
// hb_num(1) + rp_num(1) + ltssm_history(128 * 4 = 512) = 514 bytes
constexpr size_t LTSSM_HISTORY_SIZE = 128; // 128 uint32_t entries
struct PcieLtssmData
{
    uint8_t hbNum;                             // Host bridge number
    uint8_t rpNum;                             // Root port number
    uint32_t ltssmHistory[LTSSM_HISTORY_SIZE]; // LTSSM state history array
};
#endif

#pragma pack(pop)

// PCIe telemetry payload structures.
//
// Unlike the CPER error counters, the SatMC pcie_generic_telemetry_data payload
// is emitted with the device compiler's natural alignment, NOT packed. The
// explicit padding members below reproduce that layout byte for byte, and the
// static_asserts pin the resulting sizes to what the device actually sends.

// PCIe Telemetry payload header
struct PcieTelemetryHeader
{
    uint64_t timestamp; // Timestamp when telemetry was collected (nanoseconds)
    uint16_t vendorId;  // PCI Vendor ID
    uint16_t deviceId;  // PCI Device ID
    uint16_t ssid;      // Subsystem ID
    uint16_t ssvid;     // Subsystem Vendor ID
};
constexpr size_t PCIE_TELEMETRY_HEADER_SIZE = sizeof(PcieTelemetryHeader);
static_assert(PCIE_TELEMETRY_HEADER_SIZE == 16);

// PCIe Root Port EQ telemetry values (pcie_rp_eq_values).
// eomStatus/txStatus are pcie_eq_status_t enums on the device, i.e. 4-byte
// ints, so numLanes is followed by three bytes of padding.
struct PcieRpEqValues
{
    uint16_t laneEom[PCIE_MAX_LANES_PER_RP];     // Per-lane EOM (SLRG)
    uint8_t laneTxPreset[PCIE_MAX_LANES_PER_RP]; // Per-lane TX EQ preset
    uint8_t numLanes;                            // Number of valid lane entries
    uint8_t reserved[3];
    uint32_t eomStatus; // pcie_eq_status_t: EOM (SLRG) collection status
    uint32_t txStatus;  // pcie_eq_status_t: TX preset (SLTP) collection status
};
static_assert(sizeof(PcieRpEqValues) == 60);

// PCIe Root Port Telemetry Data structure (per root port)
struct PcieRpTelemetryData
{
    uint8_t isEnabled;    // Whether root port is enabled (bool on the device)
    uint8_t rpNum;        // Root port number (link number)
    uint8_t reserved0[2]; // Padding ahead of the 4-byte aligned sbdf
    uint32_t sbdf;        // Segment/Bus/Device/Function address
    uint8_t linkWidth;    // Current negotiated link width (lanes)
    uint8_t linkSpeed;    // Current negotiated link speed (GT/s)
    uint8_t maxLinkSpeed; // Maximum supported link speed
    uint8_t maxLinkWidth; // Maximum supported link width
    PcieRpEqValues eqValues;
    // Per-RP error counts since boot
    uint32_t ceCount;
    uint32_t ueFatalCount;
    uint32_t ueFatalDlpCount;      // AER UE bit 4
    uint32_t ueFatalSdeCount;      // AER UE bit 5
    uint32_t ueFatalFcpCount;      // AER UE bit 13
    uint32_t ueFatalRcvrOvflCount; // AER UE bit 17
    uint32_t ueFatalMalfTlpCount;  // AER UE bit 18
    uint32_t ueFatalUieCount;      // AER UE bit 22
    uint32_t ueFatalIdeCount;      // AER UE bit 28
    uint32_t ueNonfatalCount;
    uint32_t urCount;
};
static_assert(sizeof(PcieRpTelemetryData) == 116);

// PCIe Host Bridge Telemetry Data structure (per host bridge)
struct PcieHbTelemetryData
{
    uint8_t isEnabled;    // Whether host bridge is enabled (bool on the device)
    uint8_t reserved0[3]; // Padding ahead of the 4-byte aligned hbNum
    uint32_t hbNum;       // Host bridge number
    uint64_t egressBw;    // Egress bandwidth (bytes/s)
    uint64_t ingressBw;   // Ingress bandwidth (bytes/s)
    PcieRpTelemetryData rootPorts[MAX_ROOT_PORTS_PER_HB];
};
static_assert(sizeof(PcieHbTelemetryData) == 952);

void logMsg(const std::string& msg)
{
    // stdout first and unconditionally: the dump manager's journal is the only
    // record left when the report cannot be written or the archive is dropped.
    std::cout << msg << std::endl;

    std::fstream logFile;
    logFile.open(tempPath + "/Execution_Report.txt", std::ios::app);
    if (logFile)
    {
        logFile << msg << std::endl;
    }
    logFile.close();
}

std::string getCurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

/** @brief Timestamped Execution_Report.txt line. */
void reportMsg(const std::string& msg)
{
    logMsg(std::format("[{}] {}", getCurrentTimestamp(), msg));
}

/** @brief Archive base name: obmcdump_<id>_<epoch seconds>.
 *
 *  The dump manager parses the second token as epoch seconds when it builds
 *  the entry, so an MMDDHHMMSS token there produced nonsense Created and
 *  CompletedTime values on every dump this tool made (D5).
 */
std::string generateTempFolderName(const std::string& id)
{
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

    return std::format("obmcdump_{}_{}", id, epoch);
}

/** @brief Removes a directory tree when the enclosing scope exits.
 *
 *  The staging directory is created before the argument checks that can bail
 *  out, and those returns bypassed the explicit cleanup at the end of main.
 *  Nothing else sweeps the collector's temp root, so every such request leaked
 *  a directory until the next reboot (D4).
 */
class TempDirGuard
{
  public:
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    TempDirGuard(TempDirGuard&&) = delete;
    TempDirGuard& operator=(TempDirGuard&&) = delete;

    explicit TempDirGuard(std::string dir) : path(std::move(dir)) {}

    ~TempDirGuard()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

  private:
    std::string path;
};

#ifdef PCORE_DUMP
/** @brief Exclusive per-terminus lock held for the length of a collection.
 *
 *  The staging filename is fixed, so two collectors working the same terminus
 *  would overwrite each other's payloads. pldmd deliberately keeps no
 *  collection state and refuses nothing, so this lock is the only thing that
 *  serialises them. Requests routed through the dump manager are already
 *  serialised by its own gate; this covers direct invocations.
 */
class TerminusLock
{
  public:
    TerminusLock(const TerminusLock&) = delete;
    TerminusLock& operator=(const TerminusLock&) = delete;
    TerminusLock(TerminusLock&&) = delete;
    TerminusLock& operator=(TerminusLock&&) = delete;

    explicit TerminusLock(const std::string& terminus) :
        path(std::format("{}/cpu-diag-dump-{}.lock", LOCK_DIR, terminus))
    {}

    ~TerminusLock()
    {
        if (fd >= 0)
        {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }

    /** @brief Take the lock, giving up after timeoutSec seconds. */
    bool acquire(int timeoutSec)
    {
        std::error_code ec;
        fs::create_directories(LOCK_DIR, ec);

        fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            logMsg(std::format("Failed to open lock {}: {}", path,
                               strerror(errno)));
            return false;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
        for (;;)
        {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            {
                return true;
            }
            if (errno != EWOULDBLOCK && errno != EINTR)
            {
                logMsg(std::format("Failed to lock {}: {}", path,
                                   strerror(errno)));
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                logMsg(std::format(
                    "Timed out after {}s waiting for lock {}; another "
                    "collection holds this terminus",
                    timeoutSec, path));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        close(fd);
        fd = -1;
        return false;
    }

  private:
    std::string path;
    int fd = -1;
};
#endif // PCORE_DUMP

bool loadDeviceToTerminusMap(const std::string& configPath,
                             std::unordered_map<std::string, PldmTarget>& map)
{
    std::set<std::pair<std::string, int>> seenTargets;
    std::ifstream configFile(configPath);
    if (!configFile.is_open())
    {
        log<level::ERR>(
            std::format("Failed to open PLDM static configuration: {}",
                        configPath)
                .c_str());
        return false;
    }

    json config = json::parse(configFile, nullptr, false);
    if (config.is_discarded())
    {
        log<level::ERR>(
            std::format("Failed to parse PLDM static configuration: {}",
                        configPath)
                .c_str());
        return false;
    }

    constexpr auto terminiKey = "PLDMTermini";
    if (!config.contains(terminiKey) || !config[terminiKey].is_array())
    {
        log<level::ERR>(
            std::format("PLDM static configuration {} must contain array '{}'",
                        configPath, terminiKey)
                .c_str());
        return false;
    }

    for (const auto& terminusInfo : config[terminiKey])
    {
        if (!terminusInfo.is_object())
        {
            log<level::ERR>(
                std::format("PLDM static configuration {} contains an invalid "
                            "PLDMTermini entry",
                            configPath)
                    .c_str());
            return false;
        }

        if (terminusInfo.contains("Type") &&
            terminusInfo.value("Type", "") != "PLDMTerminus")
        {
            continue;
        }

        if (!terminusInfo.contains("CpuIndex"))
        {
            continue;
        }

        if (!terminusInfo["CpuIndex"].is_number_integer() ||
            terminusInfo["CpuIndex"].get<int>() < 0)
        {
            log<level::ERR>(std::format("PLDM static configuration {} has an "
                                        "invalid CpuIndex",
                                        configPath)
                                .c_str());
            return false;
        }

        if (!terminusInfo.contains("Instance") ||
            !terminusInfo["Instance"].is_number_integer() ||
            terminusInfo["Instance"].get<int>() < 0)
        {
            log<level::ERR>(std::format("PLDM static configuration {} has an "
                                        "invalid Instance",
                                        configPath)
                                .c_str());
            return false;
        }

        if (!terminusInfo.contains("TerminusName") ||
            !terminusInfo["TerminusName"].is_string() ||
            terminusInfo["TerminusName"].get<std::string>().empty())
        {
            log<level::ERR>(std::format("PLDM static configuration {} has an "
                                        "invalid TerminusName",
                                        configPath)
                                .c_str());
            return false;
        }

        if (!terminusInfo.contains("EID") ||
            !terminusInfo["EID"].is_number_integer() ||
            terminusInfo["EID"].get<int>() < 0)
        {
            log<level::ERR>(std::format("PLDM static configuration {} has an "
                                        "invalid EID",
                                        configPath)
                                .c_str());
            return false;
        }

        auto instanceNum = terminusInfo["Instance"].get<int>();
        auto device = std::format("CPU_{}", instanceNum);
        PldmTarget target{
            .terminus = terminusInfo["TerminusName"].get<std::string>(),
            .eid = terminusInfo["EID"].get<int>(),
        };

        if (!seenTargets.emplace(target.terminus, target.eid).second)
        {
            log<level::ERR>(
                std::format("PLDM static configuration {} contains duplicate "
                            "CPU target TerminusName '{}' with EID {}",
                            configPath, target.terminus, target.eid)
                    .c_str());
            return false;
        }

        if (!map.emplace(device, target).second)
        {
            log<level::ERR>(
                std::format("PLDM static configuration {} contains duplicate "
                            "CPU device '{}' (Instance {})",
                            configPath, device, instanceNum)
                    .c_str());
            return false;
        }
    }

    if (map.empty())
    {
        log<level::ERR>(
            std::format("PLDM static configuration {} has no CPU mappings",
                        configPath)
                .c_str());
        return false;
    }

    return true;
}

std::string findEffecterPath(const std::string& terminus,
                             const std::string& effecterSuffix, int eid)
{
    // Search for effecter path using ObjectMapper
    // Effecter paths are:
    // /xyz/openbmc_project/control/PLDM_Effecter_<id>_<eid>/<terminus>_<suffix>
    try
    {
        sdbusplus::bus_t bus = sdbusplus::bus::new_default();
        auto method = bus.new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");

        method.append("/xyz/openbmc_project/control");
        method.append(0); // depth
        method.append(std::vector<std::string>{CONTROL_TRIGGER_INTERFACE});

        auto reply = bus.call(method);
        std::vector<std::string> paths;
        reply.read(paths);

        // Find path ending with terminus_effecterSuffix
        std::string suffix = std::format("/{}_{}", terminus, effecterSuffix);
        for (const auto& path : paths)
        {
            if (path.ends_with(suffix))
            {
                if (eid >= 0)
                {
                    const auto leafPos = path.rfind('/');
                    const auto parentPath = leafPos == std::string::npos
                                                ? ""
                                                : path.substr(0, leafPos);
                    if (!parentPath.ends_with(std::format("_{}", eid)))
                    {
                        continue;
                    }
                }
                logMsg(std::format("Found effecter path: {}", path));
                return path;
            }
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        logMsg(std::format("ObjectMapper query failed: {}", e.what()));
    }

    logMsg(
        std::format("Effecter not found for {}_{}", terminus, effecterSuffix));
    return "";
}

bool triggerEffecter(const std::string& effecterPath)
{
    if (effecterPath.empty())
    {
        return false;
    }

    try
    {
        sdbusplus::bus_t bus = sdbusplus::bus::new_default();
        auto method =
            bus.new_method_call(PLDM_SERVICE, effecterPath.c_str(),
                                "org.freedesktop.DBus.Properties", "Set");
        method.append(CONTROL_TRIGGER_INTERFACE, "Refresh",
                      std::variant<bool>(true));
        bus.call_noreply(method);
        logMsg(std::format("Triggered effecter: {}", effecterPath));
        return true;
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        logMsg(std::format("Failed to trigger effecter {}: {}", effecterPath,
                           e.what()));
        return false;
    }
}

void triggerAllEffecters(const PldmTarget& target)
{
    // Trigger all effecters
    auto cperPath = findEffecterPath(target.terminus, EFFECTER_CPER_ERROR_COUNT,
                                     target.eid);
    if (!cperPath.empty())
    {
        triggerEffecter(cperPath);
    }

#if 0
    // LTSSM effecter trigger disabled - backend not ready
    auto ltssmPath =
        findEffecterPath(target.terminus, EFFECTER_PCIE_LTSSM, target.eid);
    if (!ltssmPath.empty())
    {
        triggerEffecter(ltssmPath);
    }
#endif

    auto telemetryPath =
        findEffecterPath(target.terminus, EFFECTER_PCIE_TELEMETRY, target.eid);
    if (!telemetryPath.empty())
    {
        triggerEffecter(telemetryPath);
    }
}

void clearStagingFiles(const std::string& eventDir)
{
    // LTSSM event file removed from staging list - backend not ready
    std::vector<std::string> files = {CPER_ERROR_COUNT_EVENT_FILE,
                                      // PCIE_LTSSM_EVENT_FILE,
                                      PCIE_TELEMETRY_EVENT_FILE};
    for (const auto& file : files)
    {
        auto path = eventDir + "/" + file;
        if (fs::exists(path))
        {
            fs::remove(path);
            logMsg(std::format("Cleared staging file: {}", path));
        }
    }
}

// ---------------------------------------------------------------------------
// PCore mode
// ---------------------------------------------------------------------------

#ifdef PCORE_DUMP
/** @brief Locate the PCore dump effecter object of one CPU package.
 *
 *  Discovery is by interface through ObjectMapper plus the terminus-prefixed
 *  object name, so no EID, effecter ID or object path is ever assumed.
 *
 *  @param[in] terminus - PLDM terminus name owning the CPU package.
 *  @param[in] eid - EID of that package, reported in the not-found log only.
 *
 *  @return The object path, or an empty string when the CPU exposes none.
 */
std::string findPCoreEffecterPath(const std::string& terminus, int eid)
{
    try
    {
        sdbusplus::bus_t bus = sdbusplus::bus::new_default();
        auto method = bus.new_method_call(MAPPER_SERVICE, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTreePaths");
        method.append(std::string(pcore::controlRoot));
        method.append(0); // depth
        method.append(
            std::vector<std::string>{std::string(pcore::pcoreDumpInterface)});

        std::vector<std::string> paths;
        bus.call(method).read(paths);

        const std::string prefix = terminus + "_";
        for (const auto& path : paths)
        {
            if (!fs::path(path).filename().string().starts_with(prefix))
            {
                continue;
            }

            // Matching stops at the terminus-prefixed name deliberately.
            // pldmd builds these paths flat as
            // /xyz/openbmc_project/control/<effecterName>, so there is no
            // parent component to carry an EID, and the fallback name for an
            // effecter with no PDR auxiliary name embeds the TID rather than
            // the EID. A platform that ever fronted both packages behind a
            // single terminus name could therefore not be separated here at
            // all; the mechanism for that is the per-CPU
            // cpu/pcore_dump_control association pldmd already publishes on
            // each effecter, not the object path.
            logMsg(std::format("Found PCore dump effecter: {}", path));
            return path;
        }

        logMsg(std::format(
            "No {} object named for terminus {} (EID {}) among {} candidate(s)",
            pcore::pcoreDumpInterface, terminus, eid, paths.size()));
    }
    catch (const std::exception& e)
    {
        logMsg(std::format("ObjectMapper query for {} failed: {}",
                           pcore::pcoreDumpInterface, e.what()));
    }

    return "";
}

/** @brief Read one of the selector bound properties off the effecter. */
std::optional<uint64_t> readPCoreBound(const std::string& effecterPath,
                                       const char* property)
{
    try
    {
        sdbusplus::bus_t bus = sdbusplus::bus::new_default();
        auto method = bus.new_method_call(PLDM_SERVICE, effecterPath.c_str(),
                                          PROPERTIES_INTERFACE, "Get");
        method.append(std::string(pcore::pcoreDumpInterface), property);

        std::variant<uint64_t> value;
        bus.call(method).read(value);
        return std::get<uint64_t>(value);
    }
    catch (const std::exception& e)
    {
        logMsg(std::format("Failed to read {} from {}: {}", property,
                           effecterPath, e.what()));
        return std::nullopt;
    }
}

/** @brief Dispatch a dump of one PCore.
 *
 *  A successful return means the set was handed to the device, never that a
 *  payload followed. Only pre-dispatch rejections throw.
 *
 *  @param[in] effecterPath - Object carrying com.nvidia.PCoreDump.
 *  @param[in] pcoreId - Selector to dump.
 *  @param[out] error - Reason text when the call was rejected.
 *
 *  @return True when the request was dispatched.
 */
bool collectPCore(const std::string& effecterPath, uint64_t pcoreId,
                  std::string& error)
{
    try
    {
        sdbusplus::bus_t bus = sdbusplus::bus::new_default();
        auto method = bus.new_method_call(PLDM_SERVICE, effecterPath.c_str(),
                                          pcore::pcoreDumpInterface,
                                          pcore::createDumpMethod);
        method.append(pcoreId);
        bus.call(method);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

/** @brief Discard any queued inotify events without acting on them. */
void drainInotify(int inotifyFd)
{
    std::array<char, 4096> buffer{};
    while (read(inotifyFd, buffer.data(), buffer.size()) > 0)
    {}
}

/** @brief Wait for pldmd to stage a payload at path.
 *
 *  pldmd publishes by rename, so the file either is not there or is complete;
 *  existence is therefore the decision and inotify only shortens the wait. The
 *  watch is armed by the caller before the trigger, and the periodic existence
 *  check covers an event delivered before the watch or lost in the queue.
 *
 *  @param[in] inotifyFd - Watch armed on the staging directory.
 *  @param[in] path - Staged payload to wait for.
 *  @param[in] timeoutSec - Seconds to wait before giving up.
 *
 *  @return True when the payload appeared within the timeout.
 */
bool waitForStagedFile(int inotifyFd, const std::string& path, int timeoutSec)
{
    constexpr int POLL_INTERVAL_MS = 2000;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);

    for (;;)
    {
        if (fs::exists(path))
        {
            return true;
        }

        const auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remainingMs <= 0)
        {
            return false;
        }

        struct pollfd pfd{inotifyFd, POLLIN, 0};
        const int waitMs =
            static_cast<int>(std::min<int64_t>(remainingMs, POLL_INTERVAL_MS));

        const int ret = poll(&pfd, 1, waitMs);
        if (ret > 0)
        {
            drainInotify(inotifyFd);
        }
        else if (ret < 0 && errno != EINTR)
        {
            logMsg(std::format("Poll error while waiting for {}: {}", path,
                               strerror(errno)));
            return fs::exists(path);
        }
    }
}

/** @brief Tally of one PCore collection request. */
struct PCoreOutcome
{
    size_t requested = 0;
    size_t collected = 0;
    size_t rejected = 0;   // Collect threw before any wire traffic
    size_t timedOut = 0;   // dispatched, but no payload within the timeout
    size_t copyFailed = 0; // payload arrived, staging it locally failed
};

/** @brief Run the per-selector collection loop for one CPU package.
 *
 *  Each iteration clears the staged payload of the previous selector,
 *  dispatches its own, waits for the replacement and copies it out under a
 *  name derived from the selector just requested. A selector that fails is
 *  recorded and skipped: one dead PCore must not discard the rest of a bulk
 *  collection.
 *
 *  @param[in] effecterPath - Object carrying com.nvidia.PCoreDump.
 *  @param[in] eventDir - pldmd staging directory of this terminus.
 *  @param[in] selectors - Selectors to collect, in order.
 *
 *  @return Counts of what was collected and how the rest failed.
 */
PCoreOutcome runPCoreLoop(const std::string& effecterPath,
                          const std::string& eventDir,
                          const std::vector<uint64_t>& selectors)
{
    PCoreOutcome outcome;
    outcome.requested = selectors.size();

    const std::string stagedPath =
        eventDir + "/" + std::string(PCORE_DUMP_EVENT_FILE);

    std::error_code ec;
    fs::create_directories(eventDir, ec);

    const int inotifyFd = inotify_init1(IN_NONBLOCK);
    if (inotifyFd < 0)
    {
        reportMsg(
            std::format("Failed to initialize inotify: {}", strerror(errno)));
        outcome.rejected = selectors.size();
        return outcome;
    }

    // Armed before the first trigger, and kept armed across the loop, so no
    // payload can land in the gap between triggering and watching. IN_MOVED_TO
    // is required: pldmd publishes with rename, which never reports
    // IN_CLOSE_WRITE.
    const int watchFd = inotify_add_watch(inotifyFd, eventDir.c_str(),
                                          IN_MOVED_TO | IN_CLOSE_WRITE);
    if (watchFd < 0)
    {
        reportMsg(
            std::format("Failed to watch {}: {}", eventDir, strerror(errno)));
        close(inotifyFd);
        outcome.rejected = selectors.size();
        return outcome;
    }

    for (const auto selector : selectors)
    {
        // Clear the previous payload so the wait below cannot be satisfied by
        // it, then drop the events that removal and its predecessor queued.
        if (fs::exists(stagedPath))
        {
            fs::remove(stagedPath, ec);
            if (ec)
            {
                reportMsg(std::format(
                    "PCore {}: failed to clear stale staged file {}: {}",
                    selector, stagedPath, ec.message()));
                outcome.rejected++;
                continue;
            }
        }
        drainInotify(inotifyFd);

        std::string error;
        if (!collectPCore(effecterPath, selector, error))
        {
            reportMsg(std::format("PCore {}: rejected before dispatch: {}",
                                  selector, error));
            outcome.rejected++;
            continue;
        }

        if (!waitForStagedFile(inotifyFd, stagedPath, timeoutSeconds))
        {
            // A firmware rejection after dispatch looks exactly like a payload
            // that never came; only the pldmd journal tells them apart.
            reportMsg(std::format(
                "PCore {}: no payload staged within {}s (see the pldmd journal "
                "for a completion code)",
                selector, timeoutSeconds));
            outcome.timedOut++;
            continue;
        }

        const auto outName =
            std::format("PCoreDump_{}_PCore_{}.bin", targetDevice, selector);
        const auto outPath = tempPath + "/" + outName;

        fs::copy_file(stagedPath, outPath, fs::copy_options::overwrite_existing,
                      ec);
        if (ec)
        {
            reportMsg(std::format("PCore {}: failed to copy {} to {}: {}",
                                  selector, stagedPath, outPath, ec.message()));
            // copy_file leaves whatever it managed to write behind, and the
            // archive step packs the whole directory, so a truncated payload
            // would ship looking exactly like a good one.
            std::error_code rmEc;
            fs::remove(outPath, rmEc);
            outcome.copyFailed++;
            continue;
        }

        const auto size = fs::file_size(outPath, ec);
        reportMsg(std::format("PCore {}: collected {} ({} bytes)", selector,
                              outName, ec ? 0 : size));
        outcome.collected++;
    }

    // The last payload belongs to no further request; leaving it staged would
    // let the next collection mistake it for its own.
    fs::remove(stagedPath, ec);

    inotify_rm_watch(inotifyFd, watchFd);
    close(inotifyFd);
    return outcome;
}
#endif // PCORE_DUMP

std::string getLinkSpeedName(uint8_t speed)
{
    // Speed 0 means the link never trained. Report that distinctly so it is
    // not confused with a speed encoding the tool does not recognise.
    if (speed == 0)
    {
        return "NotTrained";
    }
    auto it = linkSpeedNames.find(speed);
    return it != linkSpeedNames.end() ? it->second : "Unknown";
}

std::string formatSbdf(uint32_t sbdf)
{
    uint16_t segment = (sbdf >> 16) & 0xFFFF;
    uint8_t bus = (sbdf >> 8) & 0xFF;
    uint8_t device = (sbdf >> 3) & 0x1F;
    uint8_t function = sbdf & 0x07;
    return std::format("{:04x}:{:02x}:{:02x}.{}", segment, bus, device,
                       function);
}

json parseErrorCounterPayload(const std::vector<uint8_t>& data)
{
    json result;

    if (data.size() < sizeof(ErrorCounterPayload))
    {
        result["error"] = "Payload too small";
        result["data_valid"] = false;
        return result;
    }

    const auto* payload =
        reinterpret_cast<const ErrorCounterPayload*>(data.data());

    result["event_received_timestamp"] = cperErrorCountReceivedTime;
    result["data_valid"] = true;

    json coreErrors;
    coreErrors["cpu_corrected"] = payload->cpuCorrectedErrors;
    coreErrors["uncore_corrected"] = payload->uncoreCorrectedErrors;
    coreErrors["cache_corrected"] = payload->cacheCorrectedErrors;
    coreErrors["dram_corrected"] = payload->dramCorrectedErrors;
    coreErrors["dram_uncorrected"] = payload->dramUncorrectedErrors;
    coreErrors["pages_retired"] = payload->pagesRetired;
    coreErrors["other_soc_corrected"] = payload->otherSocCorrectedErrors;
    result["core_errors"] = coreErrors;

    static const std::array<const char*, 4> disableReasons = {
        "alias_checker", "training_at_por_frequency_failed",
        "training_at_boot_frequency_failed", "threshold_of_bad_pages_exceeded"};

    json cmetChannels = json::array();
    for (int i = 0; i < CMET_CHANNEL_COUNT; i++)
    {
        const uint32_t st = payload->cmetInfo[i].status;
        const bool disabled = (st & 0x04) != 0;
        json channel;
        channel["channel"] = i;
        channel["errors"] = payload->cmetInfo[i].count;
        channel["status"] = std::format("0x{:08X}", st);
        channel["enabled"] = (st & 0x01) != 0;
        channel["spare"] = (st & 0x02) != 0;
        channel["disabled"] = disabled;
        // Bits 3-4: disable reason, only meaningful when the channel is
        // permanently disabled.
        if (disabled)
        {
            channel["disable_reason"] = disableReasons[(st >> 3) & 0x03];
        }
        // Bits 5-7: SOCAMM module index [0-7]
        channel["socamm_module_index"] = (st >> 5) & 0x07;
        cmetChannels.push_back(channel);
    }
    result["cmet_channels"] = cmetChannels;
    result["cmet_spare_count"] = payload->cmetSpareCount;

    return result;
}

#if 0
// LTSSM payload parser disabled - backend not ready
json parsePcieLtssmPayload(const std::vector<uint8_t>& data)
{
    json result;
    result["event_received_timestamp"] = pcieLtssmReceivedTime;
    result["data_valid"] = true;

    if (data.size() < sizeof(PcieLtssmData))
    {
        result["data_valid"] = false;
        return result;
    }

    const auto* ltssmData = reinterpret_cast<const PcieLtssmData*>(data.data());

    result["host_bridge"] = ltssmData->hbNum;
    result["root_port"] = ltssmData->rpNum;

    // Extract LTSSM history - only include non-zero state transitions
    json ltssmHistory = json::array();
    int nonZeroCount = 0;
    for (size_t i = 0; i < LTSSM_HISTORY_SIZE; i++)
    {
        uint32_t state = ltssmData->ltssmHistory[i];
        if (state != 0)
        {
            nonZeroCount++;
            json entry;
            entry["index"] = i;
            entry["state"] = std::format("0x{:08X}", state);
            ltssmHistory.push_back(entry);
        }
    }

    result["ltssm_history"] = ltssmHistory;
    result["ltssm_history_count"] = nonZeroCount;

    return result;
}
#endif

json parsePcieTelemetryPayload(const std::vector<uint8_t>& data)
{
    json result;
    result["event_received_timestamp"] = pcieTelemetryReceivedTime;
    result["data_valid"] = true;

    if (data.size() < PCIE_TELEMETRY_HEADER_SIZE)
    {
        result["data_valid"] = false;
        result["host_bridges"] = json::array();
        return result;
    }

    const auto* header =
        reinterpret_cast<const PcieTelemetryHeader*>(data.data());

    result["timestamp_ns"] = header->timestamp;
    result["vendor_id"] = std::format("0x{:04X}", header->vendorId);
    result["device_id"] = std::format("0x{:04X}", header->deviceId);
    result["subsystem_id"] = std::format("0x{:04X}", header->ssid);
    result["subsystem_vendor_id"] = std::format("0x{:04X}", header->ssvid);

    static const std::array<const char*, 4> eqStatusNames = {
        "valid", "speed_too_low", "link_down", "mnoc_fail"};

    // The device sends one PcieHbTelemetryData per host bridge in the socket;
    // the count is implied by the payload size rather than carried in the
    // header, so derive it instead of assuming a fixed number.
    const size_t hbBytes = data.size() - PCIE_TELEMETRY_HEADER_SIZE;
    const size_t hbRecords = hbBytes / sizeof(PcieHbTelemetryData);
    const size_t hbCount = std::min<size_t>(hbRecords, MAX_HOST_BRIDGES);
    if (hbBytes % sizeof(PcieHbTelemetryData) != 0)
    {
        result["data_valid"] = false;
        result["error"] = std::format(
            "Unexpected PCIe telemetry payload size {} ({} trailing bytes "
            "after {} host bridge records of {} bytes)",
            data.size(), hbBytes % sizeof(PcieHbTelemetryData), hbRecords,
            sizeof(PcieHbTelemetryData));
    }
    else if (hbRecords > MAX_HOST_BRIDGES)
    {
        // Report the bridges that fit rather than dropping them silently: a
        // payload this size means the device layout has outgrown the tool.
        result["data_valid"] = false;
        result["error"] = std::format(
            "PCIe telemetry payload carries {} host bridge records, more than "
            "the {} this tool supports; only the first {} are reported",
            hbRecords, MAX_HOST_BRIDGES, hbCount);
    }
    result["host_bridge_count"] = hbCount;

    json hostBridges = json::array();
    size_t offset = PCIE_TELEMETRY_HEADER_SIZE;

    for (size_t h = 0; h < hbCount; h++)
    {
        if (offset + sizeof(PcieHbTelemetryData) > data.size())
        {
            break;
        }

        const auto* hbData =
            reinterpret_cast<const PcieHbTelemetryData*>(data.data() + offset);

        json hb;
        hb["enabled"] = hbData->isEnabled != 0;
        hb["host_bridge"] = hbData->hbNum;
        hb["egress_bw"] = hbData->egressBw;
        hb["ingress_bw"] = hbData->ingressBw;

        json rootPorts = json::array();
        for (int r = 0; r < MAX_ROOT_PORTS_PER_HB; r++)
        {
            const auto& rpData = hbData->rootPorts[r];

            json rp;
            rp["enabled"] = rpData.isEnabled != 0;
            rp["root_port"] = rpData.rpNum;
            rp["sbdf"] = formatSbdf(rpData.sbdf);
            rp["current_link_speed"] = getLinkSpeedName(rpData.linkSpeed);
            rp["current_link_width"] = std::format("x{}", rpData.linkWidth);
            rp["max_link_speed"] = getLinkSpeedName(rpData.maxLinkSpeed);
            rp["max_link_width"] = std::format("x{}", rpData.maxLinkWidth);

            // EQ values: only emit lanes in [0, numLanes)
            const auto& eqData = rpData.eqValues;
            const uint8_t nLanes = std::min(
                eqData.numLanes, static_cast<uint8_t>(PCIE_MAX_LANES_PER_RP));
            json laneEom = json::array();
            json laneTxPreset = json::array();
            for (int l = 0; l < nLanes; l++)
            {
                laneEom.push_back(eqData.laneEom[l]);
                laneTxPreset.push_back(eqData.laneTxPreset[l]);
            }
            json eq;
            eq["num_lanes"] = eqData.numLanes;
            eq["lane_eom"] = laneEom;
            eq["lane_tx_preset"] = laneTxPreset;
            eq["eom_status"] = eqData.eomStatus < eqStatusNames.size()
                                   ? eqStatusNames[eqData.eomStatus]
                                   : "unknown";
            eq["tx_status"] = eqData.txStatus < eqStatusNames.size()
                                  ? eqStatusNames[eqData.txStatus]
                                  : "unknown";
            rp["eq_values"] = eq;

            rp["ce_count"] = rpData.ceCount;
            rp["ue_fatal_count"] = rpData.ueFatalCount;
            rp["ue_fatal_dlp_count"] = rpData.ueFatalDlpCount;
            rp["ue_fatal_sde_count"] = rpData.ueFatalSdeCount;
            rp["ue_fatal_fcp_count"] = rpData.ueFatalFcpCount;
            rp["ue_fatal_rcvr_ovfl_count"] = rpData.ueFatalRcvrOvflCount;
            rp["ue_fatal_malf_tlp_count"] = rpData.ueFatalMalfTlpCount;
            rp["ue_fatal_uie_count"] = rpData.ueFatalUieCount;
            rp["ue_fatal_ide_count"] = rpData.ueFatalIdeCount;
            rp["ue_nonfatal_count"] = rpData.ueNonfatalCount;
            rp["ur_count"] = rpData.urCount;

            rootPorts.push_back(rp);
        }
        hb["root_ports"] = rootPorts;
        hostBridges.push_back(hb);
        offset += sizeof(PcieHbTelemetryData);
    }

    result["host_bridges"] = hostBridges;
    return result;
}

std::vector<uint8_t> readBinaryFile(const std::string& path)
{
    std::vector<uint8_t> data;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return data;
    }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    data.resize(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

int waitForEvents(const std::string& eventDir,
                  std::set<std::string>& receivedEvents)
{
    int inotifyFd = inotify_init1(IN_NONBLOCK);
    if (inotifyFd < 0)
    {
        logMsg("Failed to initialize inotify");
        return -1;
    }

    // Create event directory if it doesn't exist
    if (!fs::exists(eventDir))
    {
        fs::create_directories(eventDir);
    }

    int watchFd =
        inotify_add_watch(inotifyFd, eventDir.c_str(), IN_CLOSE_WRITE);
    if (watchFd < 0)
    {
        logMsg(std::format("Failed to add inotify watch on {}", eventDir));
        close(inotifyFd);
        return -1;
    }

    // LTSSM event file removed from expected events - backend not ready
    std::set<std::string> expectedEvents = {
        /* PCIE_LTSSM_EVENT_FILE, */ CPER_ERROR_COUNT_EVENT_FILE,
        PCIE_TELEMETRY_EVENT_FILE};

    // Check for existing files first
    for (const auto& file : expectedEvents)
    {
        auto path = eventDir + "/" + file;
        if (fs::exists(path))
        {
            receivedEvents.insert(file);
#if 0
            // LTSSM timestamp tracking disabled - backend not ready
            if (file == PCIE_LTSSM_EVENT_FILE)
            {
                pcieLtssmReceivedTime = getCurrentTimestamp();
            }
            else
#endif
            if (file == CPER_ERROR_COUNT_EVENT_FILE)
            {
                cperErrorCountReceivedTime = getCurrentTimestamp();
            }
            else if (file == PCIE_TELEMETRY_EVENT_FILE)
            {
                pcieTelemetryReceivedTime = getCurrentTimestamp();
            }
            logMsg(std::format("Found existing event file: {}", file));
        }
    }

    if (receivedEvents.size() >= 2)
    {
        inotify_rm_watch(inotifyFd, watchFd);
        close(inotifyFd);
        return 0;
    }

    // Poll for new events with periodic file existence check as fallback
    struct pollfd pfd;
    pfd.fd = inotifyFd;
    pfd.events = POLLIN;

    auto startTime = std::chrono::steady_clock::now();
    constexpr int POLL_INTERVAL_MS = 2000; // Check files every 2 seconds

    // Expecting 2 events (LTSSM disabled - backend not ready)
    while (receivedEvents.size() < 2)
    {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        auto elapsedSec =
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        if (elapsedSec >= timeoutSeconds)
        {
            logMsg(std::format("Timeout waiting for events ({} seconds)",
                               timeoutSeconds));
            break;
        }

        // Use shorter poll timeout to allow periodic file checks
        int ret = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (ret < 0)
        {
            logMsg("Poll error");
            break;
        }

        // Periodic fallback: Check for files that might have been written
        // before inotify watch was established or missed by inotify
        for (const auto& file : expectedEvents)
        {
            if (receivedEvents.count(file) == 0)
            {
                auto path = eventDir + "/" + file;
                if (fs::exists(path))
                {
                    receivedEvents.insert(file);
                    auto timestamp = getCurrentTimestamp();
#if 0
                    // LTSSM timestamp tracking disabled - backend not ready
                    if (file == PCIE_LTSSM_EVENT_FILE)
                    {
                        pcieLtssmReceivedTime = timestamp;
                    }
                    else
#endif
                    if (file == CPER_ERROR_COUNT_EVENT_FILE)
                    {
                        cperErrorCountReceivedTime = timestamp;
                    }
                    else if (file == PCIE_TELEMETRY_EVENT_FILE)
                    {
                        pcieTelemetryReceivedTime = timestamp;
                    }
                    logMsg(std::format("Found event file (polling): {} at {}",
                                       file, timestamp));
                }
            }
        }

        // Expecting 2 events (LTSSM disabled - backend not ready)
        if (receivedEvents.size() >= 2)
        {
            break;
        }

        if (ret == 0)
        {
            // Poll timeout - continue to next iteration for file check
            continue;
        }

        // Read inotify events
        char buffer[4096];
        ssize_t len = read(inotifyFd, buffer, sizeof(buffer));
        if (len <= 0)
        {
            continue;
        }

        for (char* ptr = buffer; ptr < buffer + len;)
        {
            auto* event = reinterpret_cast<struct inotify_event*>(ptr);
            if (event->len > 0)
            {
                std::string filename(event->name);
                if (expectedEvents.count(filename) > 0 &&
                    receivedEvents.count(filename) == 0)
                {
                    receivedEvents.insert(filename);
                    auto timestamp = getCurrentTimestamp();
#if 0
                    // LTSSM timestamp tracking disabled - backend not ready
                    if (filename == PCIE_LTSSM_EVENT_FILE)
                    {
                        pcieLtssmReceivedTime = timestamp;
                    }
                    else
#endif
                    if (filename == CPER_ERROR_COUNT_EVENT_FILE)
                    {
                        cperErrorCountReceivedTime = timestamp;
                    }
                    else if (filename == PCIE_TELEMETRY_EVENT_FILE)
                    {
                        pcieTelemetryReceivedTime = timestamp;
                    }
                    logMsg(
                        std::format("Received event file (inotify): {} at {}",
                                    filename, timestamp));
                }
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    inotify_rm_watch(inotifyFd, watchFd);
    close(inotifyFd);

    return receivedEvents.size();
}

json createCombinedDump(const std::string& eventDir,
                        const std::set<std::string>& receivedEvents)
{
    json dump;
    json metadata;

    metadata["dump_timestamp"] = getCurrentTimestamp();
    metadata["device"] = targetDevice;
    metadata["dump_id"] = std::stoul(dumpID);
    // LTSSM event disabled - backend not ready (was 3)
    metadata["events_expected"] = 2;
    metadata["events_received"] = receivedEvents.size();

    std::vector<std::string> missingEvents;
#if 0
    // LTSSM event collection disabled - backend not ready
    if (receivedEvents.count(PCIE_LTSSM_EVENT_FILE) == 0)
    {
        missingEvents.push_back("pcie_ltssm_history");
    }
#endif
    if (receivedEvents.count(CPER_ERROR_COUNT_EVENT_FILE) == 0)
    {
        missingEvents.push_back("cper_error_counters");
    }
    if (receivedEvents.count(PCIE_TELEMETRY_EVENT_FILE) == 0)
    {
        missingEvents.push_back("pcie_telemetry");
    }

    if (missingEvents.empty())
    {
        metadata["collection_status"] = "complete";
    }
    else if (receivedEvents.empty())
    {
        metadata["collection_status"] = "failed";
        metadata["missing_events"] = missingEvents;
    }
    else
    {
        metadata["collection_status"] = "partial";
        metadata["missing_events"] = missingEvents;
    }

    dump["metadata"] = metadata;

    // Parse CPER Error Counters
    if (receivedEvents.count(CPER_ERROR_COUNT_EVENT_FILE) > 0)
    {
        auto data =
            readBinaryFile(eventDir + "/" + CPER_ERROR_COUNT_EVENT_FILE);
        dump["cper_error_counters"] = parseErrorCounterPayload(data);
    }
    else
    {
        dump["cper_error_counters"] = nullptr;
    }

#if 0
    // Parse PCIe LTSSM History - disabled, backend not ready
    if (receivedEvents.count(PCIE_LTSSM_EVENT_FILE) > 0)
    {
        auto data = readBinaryFile(eventDir + "/" + PCIE_LTSSM_EVENT_FILE);
        dump["pcie_ltssm_history"] = parsePcieLtssmPayload(data);
    }
    else
    {
        dump["pcie_ltssm_history"] = nullptr;
    }
#endif

    // Parse PCIe Telemetry Data
    if (receivedEvents.count(PCIE_TELEMETRY_EVENT_FILE) > 0)
    {
        auto data = readBinaryFile(eventDir + "/" + PCIE_TELEMETRY_EVENT_FILE);
        dump["pcie_telemetry"] = parsePcieTelemetryPayload(data);
    }
    else
    {
        dump["pcie_telemetry"] = nullptr;
    }

    json root;
    root["cpu_diagnostic_dump"] = dump;
    return root;
}

void printUsage()
{
    printf("cpu-diagnostic-dump version " VERSION "\n");
    printf("Usage: cpu-diagnostic-dump -p <file_path> -i <dump_id> -t "
           "<temp_path> -d <device_type> [-T <timeout_secs>]"
#ifdef PCORE_DUMP
           " [-m -c <pcore_ids>]"
#endif
           "\n");
    printf("\nOptions:\n");
    printf("  -p <dump_path>     Final dump output directory\n");
    printf("  -i <dump_id>       Unique dump identifier\n");
    printf("  -t <temp_path>     Temporary working directory\n");
    printf("  -d <device_type>   Target device from %s\n",
           PLDM_STATIC_CONFIG_PATH);
    printf("  -T <timeout_secs>  Payload reception timeout, per PCore in "
           "PCore mode (default: %d)\n",
           DEFAULT_TIMEOUT_SECONDS);
#ifdef PCORE_DUMP
    printf("  -m                 PCore mode: collect raw per-PCore dumps "
           "instead of the legacy bundle\n");
    printf("  -c <pcore_ids>     PCore mode selectors: \"%s\" or a "
           "comma-separated list, e.g. 1,3\n",
           pcore::allSelectorsToken);
#endif
}

/** @brief Parse the -T argument without throwing on junk (D8). */
bool parseTimeoutArg(const char* arg, int& out)
{
    const char* end = arg + strlen(arg);
    int value = 0;
    const auto [ptr, ec] = std::from_chars(arg, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0)
    {
        return false;
    }
    out = value;
    return true;
}

#ifdef PCORE_DUMP
/** @brief Collect raw per-PCore dumps for one CPU package.
 *
 *  @param[in] pldmTarget - Terminus of the CPU named on the command line.
 *  @param[in] eventDir - pldmd staging directory of that terminus.
 *  @param[out] collected - Number of payloads written into tempPath.
 *
 *  @return exitSuccess when at least one payload was collected, otherwise the
 *          exit code naming how the request failed.
 */
int runPCoreMode(const PldmTarget& pldmTarget, const std::string& eventDir,
                 size_t& collected)
{
    collected = 0;

    // Serialise against direct invocations before touching staging. Scoped to
    // the terminus and deliberately not to the EID: pldmd stages by terminus
    // name under a single fixed filename, so two packages sharing a name also
    // share the staged file and must not collect at the same time.
    TerminusLock lock(pldmTarget.terminus);
    if (!lock.acquire(timeoutSeconds))
    {
        return exitAllSelectorsFailed;
    }

    const auto effecterPath =
        findPCoreEffecterPath(pldmTarget.terminus, pldmTarget.eid);
    if (effecterPath.empty())
    {
        reportMsg(std::format(
            "{} ({}) exposes no PCore dump effecter; nothing to collect",
            targetDevice, pldmTarget.terminus));
        return exitAllSelectorsFailed;
    }

    const auto minId = readPCoreBound(effecterPath, "MinPCoreId");
    const auto maxId = readPCoreBound(effecterPath, "MaxPCoreId");
    if (!minId || !maxId || *minId > *maxId)
    {
        reportMsg(std::format("{} advertises no usable selector range",
                              effecterPath));
        return exitAllSelectorsFailed;
    }

    // The dump manager already validated this list; re-checking here keeps a
    // direct invocation from putting an out-of-range value on the wire.
    const auto parsed = pcore::parseSelectors(pcoreSelectorArg, *minId, *maxId);
    if (!parsed)
    {
        reportMsg(std::format("Selector '{}' is not within {}..{}",
                              *parsed.badToken, *minId, *maxId));
        return exitUsage;
    }

    const auto selectors = pcore::expandSelectors(parsed.ids, *minId, *maxId);
    reportMsg(std::format("Collecting PCore dump(s) [{}] for {} ({}), {}s per "
                          "selector",
                          pcore::formatSelectors(selectors), targetDevice,
                          pldmTarget.terminus, timeoutSeconds));

    const auto outcome = runPCoreLoop(effecterPath, eventDir, selectors);
    collected = outcome.collected;

    reportMsg(std::format(
        "Collected {} of {} requested PCore(s); {} rejected, {} timed out, "
        "{} failed to stage",
        outcome.collected, outcome.requested, outcome.rejected,
        outcome.timedOut, outcome.copyFailed));

    if (outcome.collected > 0)
    {
        return exitSuccess;
    }
    // Every selector failed. Name the failure the operator has to act on: a
    // request that never reached the device, one dispatched and answered by
    // silence, or payloads that arrived and could not be kept.
    if (outcome.rejected == 0 && outcome.timedOut == 0)
    {
        return exitArchiveFailed;
    }
    return outcome.rejected == 0 ? exitAllSelectorsTimedOut
                                 : exitAllSelectorsFailed;
}
#endif // PCORE_DUMP

int main(int argc, char** argv)
{
    int result = exitSuccess;

    // Parse command line arguments
    int opt;
#ifdef PCORE_DUMP
    constexpr auto optString = "p:i:t:d:T:mc:h";
#else
    constexpr auto optString = "p:i:t:d:T:h";
#endif
    while ((opt = getopt(argc, argv, optString)) != -1)
    {
        switch (opt)
        {
            case 'p':
                dumpPath = optarg;
                break;
            case 'i':
                dumpID = optarg;
                break;
            case 't':
                tempPath = optarg;
                break;
            case 'd':
                targetDevice = optarg;
                break;
            case 'T':
                if (!parseTimeoutArg(optarg, timeoutSeconds))
                {
                    fprintf(stderr,
                            "Error: -T needs a positive integer, got '%s'\n",
                            optarg);
                    return exitUsage;
                }
                break;
#ifdef PCORE_DUMP
            case 'm':
                pcoreMode = true;
                break;
            case 'c':
                pcoreSelectorArg = optarg;
                break;
#endif
            case 'h':
            default:
                printUsage();
                return (opt == 'h') ? exitSuccess : exitUsage;
        }
    }

    if (dumpPath.empty() || dumpID.empty() || tempPath.empty() ||
        targetDevice.empty())
    {
        printUsage();
        return exitUsage;
    }

    using std::chrono::duration_cast;
    using std::chrono::high_resolution_clock;
    using std::chrono::milliseconds;
    auto t1 = high_resolution_clock::now();

    std::string tempFolderName = generateTempFolderName(dumpID);
    std::string tempDir = tempPath + "/" + modeTempSubdir() + "/";
    tempPath = tempDir + tempFolderName;

    // Create directories. These run before the try below, so the throwing
    // overloads would have ended the process by std::terminate rather than a
    // usage exit if the filesystem were read-only or full.
    std::error_code dirEc;
    fs::create_directories(tempPath, dirEc);
    if (dirEc && !fs::is_directory(tempPath))
    {
        fprintf(stderr, "Error: cannot create staging directory '%s': %s\n",
                tempPath.c_str(), dirEc.message().c_str());
        return exitUsage;
    }

    // Removes tempPath however this function leaves, including the argument
    // checks below that return before the archive step.
    TempDirGuard tempGuard(tempPath);

    fs::create_directories(dumpPath, dirEc);
    if (dirEc && !fs::is_directory(dumpPath))
    {
        fprintf(stderr, "Error: cannot create dump directory '%s': %s\n",
                dumpPath.c_str(), dirEc.message().c_str());
        return exitUsage;
    }

    // Load targetDevice validation and PLDM terminus mapping from config
    std::unordered_map<std::string, PldmTarget> deviceToTerminusMap;
    if (!loadDeviceToTerminusMap(PLDM_STATIC_CONFIG_PATH, deviceToTerminusMap))
    {
        return exitUsage;
    }

    auto it = deviceToTerminusMap.find(targetDevice);
    if (it == deviceToTerminusMap.end())
    {
        fprintf(stderr, "Error: Invalid device type '%s'\n",
                targetDevice.c_str());
        fprintf(stderr, "Allowed values: ");
        bool first = true;
        for (const auto& [device, _] : deviceToTerminusMap)
        {
            if (!first)
                fprintf(stderr, ", ");
            fprintf(stderr, "%s", device.c_str());
            first = false;
        }
        fprintf(stderr, "\n");
        return exitUsage;
    }
    const auto& pldmTarget = it->second;
    std::string pldmTerminus = pldmTarget.terminus;

    std::string eventDir = std::string(EVENT_STAGING_BASE) + "/" + pldmTerminus;

    logMsg(std::format("Starting {} dump collection for {} ({})", modeName(),
                       targetDevice, pldmTerminus));

    try
    {
        // Number of artifacts staged for the archive. Zero means there is
        // nothing worth archiving.
        size_t produced = 0;

#ifdef PCORE_DUMP
        if (pcoreMode)
        {
            result = runPCoreMode(pldmTarget, eventDir, produced);
        }
        else
#endif
        {
            // Step 1: Clear existing staging files
            clearStagingFiles(eventDir);

            // Step 2: Trigger all PLDM effecters
            logMsg("Triggering PLDM effecters...");
            triggerAllEffecters(pldmTarget);

            // Step 3: Wait for event files
            std::set<std::string> receivedEvents;
            int eventCount = waitForEvents(eventDir, receivedEvents);
            // Expecting 2 events (LTSSM disabled - backend not ready)
            logMsg(std::format("Received {} of 2 expected events", eventCount));

            // Step 4: Create combined JSON dump
            json dumpJson = createCombinedDump(eventDir, receivedEvents);

            // Step 5: Write dump file to temp directory
            std::string outputFilename =
                std::format("cpu_diagnostic_dump_{}.json", dumpID);
            std::string outputPath = tempPath + "/" + outputFilename;
            std::ofstream outFile(outputPath);
            outFile << dumpJson.dump(2);
            outFile.close();
            logMsg(std::format("Created dump file: {}", outputPath));

            produced = eventCount > 0 ? static_cast<size_t>(eventCount) : 0;
            result = produced > 0 ? exitSuccess : exitNoEvents;
        }

        // Step 6: Compress and copy to final location
        auto t2 = high_resolution_clock::now();
        auto msInt = duration_cast<milliseconds>(t2 - t1);
        int msecs = static_cast<int>(msInt.count());
        int hours = msecs / (60 * 60 * 1000);
        msecs -= hours * (60 * 60 * 1000);
        int mins = msecs / (60 * 1000);
        msecs -= mins * (60 * 1000);
        int seconds = msecs / 1000;
        msecs -= (seconds * 1000);

        logMsg(std::format(
            "Execution time: {} hours, {} minutes, {} seconds, {} milliseconds",
            hours, mins, seconds, msecs));

        if (produced > 0)
        {
            const auto archivePath =
                dumpPath + "/" + tempFolderName + ".tar.xz";
            // tar writes under a dot-prefixed name in the same directory and
            // the archive is published by renaming it into place. The dump
            // manager watches this directory, so writing under the final name
            // meant a failed tar queued an IN_CLOSE_WRITE for an archive the
            // cleanup then deleted, and the manager stat()ed a path that had
            // gone. A same-directory rename is atomic and reports IN_MOVED_TO
            // exactly once; the failure path only ever removes a name the
            // manager has been told to ignore (D3).
            const auto stagingPath =
                dumpPath + "/." + tempFolderName + ".tar.xz.part";
            logMsg(std::format("Compressing dump to `{}`", archivePath));

            // Compression is serialized system-wide behind the shared tar
            // lock, and the command is an argv rather than a shell line, so
            // the D9 fix is preserved. tar exiting 1 is a warning (a file
            // changed while being read); the archive is still produced.

            std::error_code ec;
            std::vector<std::string> command = {"tar", "-Jcf",  stagingPath,
                                                "-C",  tempDir, tempFolderName};
            int tarRc = phosphor::dump::compression::runShellWithLock(
                phosphor::dump::compression::lockPath, std::move(command));
            if (tarRc != EXIT_SUCCESS && tarRc != EXIT_CODE_TAR_WARNING)
            {
                // Exiting 0 here left the entry InProgress while the only copy
                // of the data was deleted by the cleanup below (D3).
                logMsg(std::format("Compression failed with error code: {}",
                                   tarRc));
                fs::remove(stagingPath, ec);
                result = exitArchiveFailed;
            }
            else
            {
                fs::rename(stagingPath, archivePath, ec);
                if (ec)
                {
                    logMsg(std::format("Failed to publish {} as {}: {}",
                                       stagingPath, archivePath, ec.message()));
                    fs::remove(stagingPath, ec);
                    result = exitArchiveFailed;
                }
            }
        }
        else
        {
            logMsg("Nothing collected within timeout; no archive produced.");
        }

        // tempPath is cleaned up by TempDirGuard on every exit path. Removing
        // the shared parent here destroyed the staging of any collection
        // running alongside this one (D4).
    }
    catch (const std::exception& e)
    {
        logMsg(std::format("Error: {}", e.what()));
        log<level::ERR>(e.what());
        result = modeNothingCollectedExit();
    }

    return result;
}
