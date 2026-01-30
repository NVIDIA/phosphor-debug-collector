/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tool to collect PLDM OEM diagnostic events from SatMC:
 * - CPER Error Counters
 * - PCIe Root Port Static Data
 * - PCIe Root Port Performance Data
 */

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
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
#include <set>
#include <unordered_map>
#include <vector>

#define VERSION "1.0"
#define SLEEP_DURING_WAIT_SECONDS 1
#define DEFAULT_TIMEOUT_SECONDS 90
#define CMET_CHANNEL_COUNT 64
#define MAX_HOST_BRIDGES 8
#define MAX_ROOT_PORTS_PER_HB 16

using json = nlohmann::json;
using namespace phosphor::logging;
namespace fs = std::filesystem;

// Event staging directory base path
constexpr auto EVENT_STAGING_BASE = "/var/lib/pldm_events";

// Event file names (written by pldmd for OEM event classes)
// Files in /var/lib/pldm_events/<terminus>/
constexpr auto CPER_ERROR_COUNT_EVENT_FILE = "CPERErrorCount_0_0.bin";
constexpr auto PCIE_LTSSM_EVENT_FILE = "PCIeLTSSM_0_0.bin";
constexpr auto PCIE_TELEMETRY_EVENT_FILE = "PCIeTelemetry_0_0.bin";

// D-Bus effecter paths
constexpr auto PLDM_SERVICE = "xyz.openbmc_project.PLDM";
constexpr auto CONTROL_TRIGGER_INTERFACE =
    "xyz.openbmc_project.Control.Trigger";

// Effecter name suffixes (appended to ProcessorModule_X_)
constexpr auto EFFECTER_CPER_ERROR_COUNT = "CPERErrorCount_0_0";
constexpr auto EFFECTER_PCIE_LTSSM = "PCIeLTSSM_0_0";
constexpr auto EFFECTER_PCIE_TELEMETRY = "PCIeTelemetry_0_0";

// Tool state
std::string tempPath;
std::string targetDevice;
std::string dumpPath;
std::string dumpID;
int timeoutSeconds = DEFAULT_TIMEOUT_SECONDS;

// Event reception timestamps
std::string cperErrorCountReceivedTime;
std::string pcieLtssmReceivedTime;
std::string pcieTelemetryReceivedTime;

// Link speed names
const std::map<uint8_t, std::string> linkSpeedNames = {
    {1, "Gen1"}, {2, "Gen2"}, {3, "Gen3"},
    {4, "Gen4"}, {5, "Gen5"}, {6, "Gen6"}};

// Payload structures (packed)
#pragma pack(push, 1)

struct ErrorCounterPayload
{
    uint32_t cpuCorrectedErrors;
    uint32_t uncoreCorrectedErrors;
    uint32_t cacheCorrectedErrors;
    uint32_t dramCorrectedErrors;
    uint32_t dramUncorrectedErrors;
    uint32_t pagesRetired;
    uint32_t otherSocCorrectedErrors;
    uint32_t cmetCount[CMET_CHANNEL_COUNT];
    uint32_t cmetStatus[CMET_CHANNEL_COUNT];
};

// LTSSM History data structure (per root port)
// Based on spec: hb_num(1) + rp_num(1) + ltssm_history(512) + padding(2) = 516
// bytes
constexpr size_t LTSSM_HISTORY_SIZE = 128; // 128 uint32_t entries
struct PcieLtssmData
{
    uint8_t hbNum;                             // Host bridge number
    uint8_t rpNum;                             // Root port number
    uint32_t ltssmHistory[LTSSM_HISTORY_SIZE]; // LTSSM state history array
    uint16_t reserved;                         // Padding to align
};

// PCIe Telemetry payload header
// Based on spec: timestamp(8) + vendor_id(2) + device_id(2) + ssid(2) +
// ssvid(2) = 16 bytes
struct PcieTelemetryHeader
{
    uint64_t timestamp; // Timestamp when telemetry was collected (nanoseconds)
    uint16_t vendorId;  // PCI Vendor ID (e.g., 0x10de for NVIDIA)
    uint16_t deviceId;  // PCI Device ID
    uint16_t ssid;      // Subsystem ID
    uint16_t ssvid;     // Subsystem Vendor ID
};
constexpr size_t PCIE_TELEMETRY_HEADER_SIZE = sizeof(PcieTelemetryHeader);

// PCIe Root Port Telemetry Data structure (per root port)
// Based on spec: is_enabled + rp_num + sbdf + link info + eq_values + perf_data
// Note: eq_values and perf_data are TBD in spec, using observed size
struct PcieRpTelemetryData
{
    uint8_t isEnabled;    // Whether root port is enabled
    uint8_t rpNum;        // Root port number (link number)
    uint16_t reserved1;   // Padding
    uint32_t sbdf;        // Segment/Bus/Device/Function address
    uint8_t linkWidth;    // Current negotiated link width (lanes)
    uint8_t linkSpeed;    // Current negotiated link speed (GT/s)
    uint8_t maxLinkSpeed; // Maximum supported link speed
    uint8_t maxLinkWidth; // Maximum supported link width
    // Additional fields based on observed data size
    uint32_t txThroughput;
    uint32_t rxThroughput;
    uint32_t correctedErrors;
    uint32_t uncorrectedErrors;
};

#pragma pack(pop)

void logMsg(const std::string& msg)
{
    std::fstream logFile;
    logFile.open(tempPath + "/Execution_Report.txt", std::ios::app);
    if (logFile)
    {
        logFile << msg << std::endl;
        std::cout << msg << std::endl;
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

std::string generateTempFolderName(const std::string& id)
{
    auto now = std::chrono::system_clock::now();
    std::time_t timeNow = std::chrono::system_clock::to_time_t(now);

    struct tm timeInfo;
    char timeString[26];
    localtime_r(&timeNow, &timeInfo);

    sprintf(timeString, "%02d%02d%02d%02d%02d", timeInfo.tm_mon + 1,
            timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min,
            timeInfo.tm_sec);

    return std::format("obmcdump_{}_{}", id, timeString);
}

std::string findEffecterPath(const std::string& terminus,
                             const std::string& effecterSuffix)
{
    // Search for effecter path using ObjectMapper
    // Effecter paths are:
    // /xyz/openbmc_project/control/PLDM_Effecter_<id>_<tid>/<terminus>_<suffix>
    try
    {
        sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
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
        sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
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

void triggerAllEffecters(const std::string& terminus)
{
    // Trigger all 3 effecters
    auto cperPath = findEffecterPath(terminus, EFFECTER_CPER_ERROR_COUNT);
    if (!cperPath.empty())
    {
        triggerEffecter(cperPath);
    }

    auto ltssmPath = findEffecterPath(terminus, EFFECTER_PCIE_LTSSM);
    if (!ltssmPath.empty())
    {
        triggerEffecter(ltssmPath);
    }

    auto telemetryPath = findEffecterPath(terminus, EFFECTER_PCIE_TELEMETRY);
    if (!telemetryPath.empty())
    {
        triggerEffecter(telemetryPath);
    }
}

void clearStagingFiles(const std::string& eventDir)
{
    std::vector<std::string> files = {CPER_ERROR_COUNT_EVENT_FILE,
                                      PCIE_LTSSM_EVENT_FILE,
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

std::string getLinkSpeedName(uint8_t speed)
{
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

    json cmetChannels = json::array();
    for (int i = 0; i < CMET_CHANNEL_COUNT; i++)
    {
        json channel;
        channel["channel"] = i;
        channel["errors"] = payload->cmetCount[i];
        channel["status"] = std::format("0x{:02X}", payload->cmetStatus[i]);
        channel["enabled"] = (payload->cmetStatus[i] & 0x01) != 0;
        channel["spare"] = (payload->cmetStatus[i] & 0x02) != 0;
        channel["disabled"] = (payload->cmetStatus[i] & 0x04) != 0;
        cmetChannels.push_back(channel);
    }
    result["cmet_channels"] = cmetChannels;

    return result;
}

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

json parsePcieTelemetryPayload(const std::vector<uint8_t>& data)
{
    json result;
    result["event_received_timestamp"] = pcieTelemetryReceivedTime;
    result["data_valid"] = true;

    // Validate minimum size for header
    if (data.size() < PCIE_TELEMETRY_HEADER_SIZE)
    {
        result["data_valid"] = false;
        result["root_ports"] = json::array();
        return result;
    }

    // Parse header
    const auto* header =
        reinterpret_cast<const PcieTelemetryHeader*>(data.data());

    // Add header info to result
    result["timestamp_ns"] = header->timestamp;
    result["vendor_id"] = std::format("0x{:04X}", header->vendorId);
    result["device_id"] = std::format("0x{:04X}", header->deviceId);
    result["subsystem_id"] = std::format("0x{:04X}", header->ssid);
    result["subsystem_vendor_id"] = std::format("0x{:04X}", header->ssvid);

    json rootPorts = json::array();
    // Skip the 16-byte header
    size_t offset = PCIE_TELEMETRY_HEADER_SIZE;

    while (offset + sizeof(PcieRpTelemetryData) <= data.size())
    {
        const auto* rpData =
            reinterpret_cast<const PcieRpTelemetryData*>(data.data() + offset);

        // Check for end marker
        if (rpData->isEnabled == 0xFF && rpData->rpNum == 0xFF)
        {
            break;
        }

        json rp;
        rp["enabled"] = rpData->isEnabled != 0;
        rp["root_port"] = rpData->rpNum;
        rp["sbdf"] = formatSbdf(rpData->sbdf);
        rp["current_link_speed"] = getLinkSpeedName(rpData->linkSpeed);
        rp["current_link_width"] = std::format("x{}", rpData->linkWidth);
        rp["max_link_speed"] = getLinkSpeedName(rpData->maxLinkSpeed);
        rp["max_link_width"] = std::format("x{}", rpData->maxLinkWidth);
        rp["tx_throughput"] = rpData->txThroughput;
        rp["rx_throughput"] = rpData->rxThroughput;
        rp["corrected_errors"] = rpData->correctedErrors;
        rp["uncorrected_errors"] = rpData->uncorrectedErrors;

        rootPorts.push_back(rp);
        offset += sizeof(PcieRpTelemetryData);
    }

    result["root_ports"] = rootPorts;
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

    std::set<std::string> expectedEvents = {
        PCIE_LTSSM_EVENT_FILE, CPER_ERROR_COUNT_EVENT_FILE,
        PCIE_TELEMETRY_EVENT_FILE};

    // Check for existing files first
    for (const auto& file : expectedEvents)
    {
        auto path = eventDir + "/" + file;
        if (fs::exists(path))
        {
            receivedEvents.insert(file);
            if (file == PCIE_LTSSM_EVENT_FILE)
            {
                pcieLtssmReceivedTime = getCurrentTimestamp();
            }
            else if (file == CPER_ERROR_COUNT_EVENT_FILE)
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

    if (receivedEvents.size() >= 3)
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

    while (receivedEvents.size() < 3)
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
                    if (file == PCIE_LTSSM_EVENT_FILE)
                    {
                        pcieLtssmReceivedTime = timestamp;
                    }
                    else if (file == CPER_ERROR_COUNT_EVENT_FILE)
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

        if (receivedEvents.size() >= 3)
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
                    if (filename == PCIE_LTSSM_EVENT_FILE)
                    {
                        pcieLtssmReceivedTime = timestamp;
                    }
                    else if (filename == CPER_ERROR_COUNT_EVENT_FILE)
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
    metadata["events_expected"] = 3;
    metadata["events_received"] = receivedEvents.size();

    std::vector<std::string> missingEvents;
    if (receivedEvents.count(PCIE_LTSSM_EVENT_FILE) == 0)
    {
        missingEvents.push_back("pcie_ltssm_history");
    }
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

    // Parse PCIe LTSSM History
    if (receivedEvents.count(PCIE_LTSSM_EVENT_FILE) > 0)
    {
        auto data = readBinaryFile(eventDir + "/" + PCIE_LTSSM_EVENT_FILE);
        dump["pcie_ltssm_history"] = parsePcieLtssmPayload(data);
    }
    else
    {
        dump["pcie_ltssm_history"] = nullptr;
    }

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
           "<temp_path> -d <device_type> [-T <timeout_secs>]\n");
    printf("\nOptions:\n");
    printf("  -p <dump_path>     Final dump output directory\n");
    printf("  -i <dump_id>       Unique dump identifier\n");
    printf("  -t <temp_path>     Temporary working directory\n");
    printf("  -d <device_type>   Target device (CPU_0 or CPU_1)\n");
    printf("  -T <timeout_secs>  Event reception timeout (default: 90s)\n");
}

int main(int argc, char** argv)
{
    int result = 0;

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "p:i:t:d:T:h")) != -1)
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
                timeoutSeconds = std::stoi(optarg);
                break;
            case 'h':
            default:
                printUsage();
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (dumpPath.empty() || dumpID.empty() || tempPath.empty() ||
        targetDevice.empty())
    {
        printUsage();
        return 1;
    }

    using std::chrono::duration_cast;
    using std::chrono::high_resolution_clock;
    using std::chrono::milliseconds;
    auto t1 = high_resolution_clock::now();

    std::string tempFolderName = generateTempFolderName(dumpID);
    std::string tempDir = tempPath + "/CPUDiagnosticDump/";
    tempPath = tempDir + tempFolderName;

    // Create directories
    if (!fs::exists(tempPath))
    {
        fs::create_directories(tempPath);
    }
    if (!fs::exists(dumpPath))
    {
        fs::create_directories(dumpPath);
    }

    // Validate targetDevice against allowed values and map to PLDM terminus
    static const std::unordered_map<std::string, std::string>
        deviceToTerminusMap = {{"CPU_0", "ProcessorModule_0"},
                               {"CPU_1", "ProcessorModule_1"}};

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
        return 1;
    }
    std::string pldmTerminus = it->second;

    std::string eventDir = std::string(EVENT_STAGING_BASE) + "/" + pldmTerminus;

    logMsg(std::format("Starting CPU diagnostic dump collection for {} ({})",
                       targetDevice, pldmTerminus));

    try
    {
        // Step 1: Clear existing staging files
        clearStagingFiles(eventDir);

        // Step 2: Trigger all PLDM effecters
        logMsg("Triggering PLDM effecters...");
        triggerAllEffecters(pldmTerminus);

        // Step 3: Wait for event files
        std::set<std::string> receivedEvents;
        int eventCount = waitForEvents(eventDir, receivedEvents);
        logMsg(std::format("Received {} of 3 expected events", eventCount));

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

        std::string command = "tar -Jcf " + dumpPath + "/" + tempFolderName +
                              ".tar.xz -C " + tempDir + " " + tempFolderName;

        logMsg(std::format("Compressing dump to `{}`",
                           dumpPath + "/" + tempFolderName + ".tar.xz"));
        // NOLINTBEGIN
        result = system(command.c_str());
        // NOLINTEND

        if (result != 0)
        {
            logMsg(
                std::format("Compression failed with error code: {}", result));
        }

        // Cleanup temp directory
        fs::remove_all(tempDir);

        // Set exit code based on collection status
        if (eventCount == 0)
        {
            result = 2; // Complete failure
        }
        else if (eventCount < 3)
        {
            result = 1; // Partial success
        }
        else
        {
            result = 0; // Full success
        }
    }
    catch (const std::exception& e)
    {
        logMsg(std::format("Error: {}", e.what()));
        log<level::ERR>(e.what());
        result = 2;
    }

    return result;
}
