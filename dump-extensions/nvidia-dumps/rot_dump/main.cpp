/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * ROT dump collector. Invoked by phosphor-dump-manager for
 * DiagnosticType ROT. Collects discovery-based IROT/VROT entries
 * (NSM via rot_dump_nsm_eid_raw).
 */

#include <unistd.h>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

// Paths and constants
constexpr const char* NSM_EID_RAW_TOOL = "/usr/bin/rot_dump_nsm_eid_raw";
constexpr const char* LEGACY_EROT_DUMP_TOOL = "/usr/bin/erot_dump.sh";
constexpr const char* TMP_DIR = "/tmp";

constexpr uint8_t NSM_DEVICE_EROT_ID = 4;
constexpr uint8_t NSM_MESSAGE_TYPE_DIAG = 4;
constexpr uint8_t NSM_CMD_GET_DEVICE_DIAGNOSTICS = 64;
constexpr uint8_t NSM_MESSAGE_TYPE_DCD = 0;
constexpr uint8_t NSM_CMD_QUERY_DEVICE_IDENTIFICATION = 9;
constexpr uint8_t NSM_MESSAGE_TYPE_FW = 6;
constexpr uint8_t NSM_CMD_GET_ROT_STATE_INFO = 1;
constexpr uint8_t NSM_CMD_QUERY_FW_COMP_ID = 7;
constexpr uint16_t NSM_FW_COMPONENT_CLASS = 10;
constexpr uint8_t PCI_VDM_MSG_TYPE = 0x7E;

// Shorter timeout for probe/bruteforce to avoid long blocks (MR feedback)
constexpr int PROBE_TIMEOUT_MS = 1000;

static int runCommand(const std::string& cmd)
{
    // NOLINTNEXTLINE(cert-env33-c): existing design invokes helper shell commands.
    int ret = std::system(cmd.c_str());
    if (ret >= 0 && WIFEXITED(ret))
    {
        return WEXITSTATUS(ret);
    }
    return -1;
}

static std::vector<uint8_t> readBinaryFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return {};
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool writeFile(const fs::path& path, const std::string& content)
{
    std::ofstream f(path);
    if (!f)
    {
        return false;
    }
    f << content;
    return f.good();
}

static bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

static fs::path findLegacyArchive(const fs::path& dir, const std::string& dumpId)
{
    const std::string prefix = "obmcdump_" + dumpId + "_";
    const std::string suffix = ".tar.xz";
    fs::path best;
    std::filesystem::file_time_type bestTime{};

    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!startsWith(name, prefix) ||
            name.size() < suffix.size() ||
            name.substr(name.size() - suffix.size()) != suffix)
        {
            continue;
        }

        auto t = entry.last_write_time();
        if (best.empty() || t > bestTime)
        {
            best = entry.path();
            bestTime = t;
        }
    }
    return best;
}

static bool mergeLegacyErotIntoTmp(const fs::path& tmpDir, const std::string& dumpId)
{
    if (!fs::exists(LEGACY_EROT_DUMP_TOOL) ||
        !fs::is_regular_file(LEGACY_EROT_DUMP_TOOL))
    {
        return false;
    }

    fs::path legacyOutDir =
        tmpDir.parent_path() / (tmpDir.filename().string() + "_legacy");
    fs::remove_all(legacyOutDir);
    fs::create_directories(legacyOutDir);

    std::string cmd = std::string(LEGACY_EROT_DUMP_TOOL) + " -p \"" +
                      legacyOutDir.string() + "\" -i " + dumpId;
    if (runCommand(cmd) != 0)
    {
        fs::remove_all(legacyOutDir);
        return false;
    }

    fs::path legacyArchive = findLegacyArchive(legacyOutDir, dumpId);
    if (legacyArchive.empty())
    {
        fs::remove_all(legacyOutDir);
        return false;
    }

    // legacy archive has top-level obmcdump_<id>_<epoch>/..., strip it
    std::string extractCmd = "tar -Jxf \"" + legacyArchive.string() +
                             "\" -C \"" + tmpDir.string() +
                             "\" --strip-components=1";
    bool ok = runCommand(extractCmd) == 0;
    fs::remove_all(legacyOutDir);
    return ok;
}

static bool endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

static std::string toHexByte(uint8_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(2)
       << static_cast<unsigned>(value);
    return os.str();
}

static std::string makeRotStatePayloadHex(uint16_t componentClass,
                                          uint16_t componentId,
                                          uint8_t componentIndex)
{
    std::ostringstream payload;
    payload << toHexByte(static_cast<uint8_t>(componentClass & 0xFF))
            << toHexByte(static_cast<uint8_t>((componentClass >> 8) & 0xFF))
            << toHexByte(static_cast<uint8_t>(componentId & 0xFF))
            << toHexByte(static_cast<uint8_t>((componentId >> 8) & 0xFF))
            << toHexByte(componentIndex);
    return payload.str();
}

static void pruneIntermediateArtifacts(const fs::path& tmpDir)
{
    for (const auto& entry : fs::directory_iterator(tmpDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string name = entry.path().filename().string();

        // Drop raw transport/probe files that are mainly implementation detail.
        if (endsWith(name, "_dcd_probe.bin") || endsWith(name, "_dcd_probe.err") ||
            endsWith(name, "_fw_comp_id_resp.bin") ||
            endsWith(name, "_fw_comp_id.err") ||
            endsWith(name, "_rot_eid_resp.bin") ||
            endsWith(name, "_rot_query_boot_status_resp.bin"))
        {
            fs::remove(entry.path());
            continue;
        }

        // Keep error logs only when non-empty to reduce archive clutter.
        if ((endsWith(name, ".err") || endsWith(name, "_rot_error.log")) &&
            entry.file_size() == 0)
        {
            fs::remove(entry.path());
        }
    }
}

static void writeBootStatusTextLog(const fs::path& outFile, uint32_t eid,
                                   const std::string& componentSource,
                                   uint16_t componentClass, uint16_t componentId,
                                   uint8_t componentIndex,
                                   const std::vector<uint8_t>& resp)
{
    std::ostringstream os;
    size_t byteCount = resp.size();
    unsigned cc = byteCount >= 1 ? resp[0] : 0;
    unsigned dataSize = byteCount >= 5
                           ? (resp[3] + (static_cast<unsigned>(resp[4]) << 8))
                           : 0;
    size_t payloadAvailable = byteCount > 5 ? byteCount - 5 : 0;

    os << "source=nsm_get_rot_state_info\n";
    os << "eid=" << eid << "\n";
    os << "component_source=" << componentSource << "\n";
    os << "component_class=0x" << std::hex << std::setfill('0') << std::setw(4)
       << componentClass << " component_id=0x" << std::setw(4) << componentId
       << " component_index=0x" << std::setw(2)
       << static_cast<unsigned>(componentIndex) << std::dec << "\n";
    os << "completion_code=" << cc << "\n";
    os << "response_size_bytes=" << byteCount << "\n";
    os << "data_size_field=" << dataSize << "\n";
    os << "payload_bytes_available=" << payloadAvailable << "\n";
    os << "payload_bytes_expected=" << dataSize << "\n";

    std::string rawHex;
    std::string payloadHex;
    for (size_t i = 0; i < byteCount; ++i)
    {
        const std::string byteHex = toHexByte(resp[i]);
        rawHex += byteHex + " ";
        if (i >= 5)
        {
            payloadHex += byteHex + " ";
        }
    }
    if (!rawHex.empty())
    {
        rawHex.pop_back();
    }
    if (!payloadHex.empty())
    {
        payloadHex.pop_back();
    }
    os << "raw_response_hex=" << rawHex << "\n";
    os << "payload_hex=" << payloadHex << "\n";

    writeFile(outFile, os.str());
}

// Return component_class, component_id, component_index for first component
// with component_class == NSM_FW_COMPONENT_CLASS (MR: scan list, not just
// index 0).
static bool queryFwComponentForEid(uint32_t eid, const fs::path& tmpDir,
                                    uint16_t& outClass, uint16_t& outId,
                                    uint8_t& outIndex)
{
    std::string probePath = (tmpDir / ("eid_" + std::to_string(eid) +
                                       "_fw_comp_id_resp.bin"))
                                .string();
    std::string errPath =
        (tmpDir / ("eid_" + std::to_string(eid) + "_fw_comp_id.err")).string();

    std::string cmd = std::string(NSM_EID_RAW_TOOL) + " --eid " +
                      std::to_string(eid) + " --instance 0 --message-type " +
                      std::to_string(NSM_MESSAGE_TYPE_FW) +
                      " --command-code " + std::to_string(NSM_CMD_QUERY_FW_COMP_ID) +
                      " --payload-hex \"\" --timeout-ms " +
                      std::to_string(PROBE_TIMEOUT_MS) + " > \"" + probePath +
                      "\" 2>>\"" + errPath + "\"";
    if (runCommand(cmd) != 0)
    {
        return false;
    }

    std::vector<uint8_t> bytes = readBinaryFile(probePath);
    if (bytes.size() < 11)
    {
        return false;
    }
    if (bytes[0] != 0)
    {
        return false;
    }

    unsigned dataSize =
        bytes[3] + (static_cast<unsigned>(bytes[4]) << 8);
    if (dataSize < 6 || bytes.size() < 5 + dataSize)
    {
        return false;
    }

    unsigned componentCount = bytes[5];
    if (componentCount == 0)
    {
        return false;
    }

    // Scan for component with class == NSM_FW_COMPONENT_CLASS (per-component
    // record: class(2), id(2), index(1) = 5 bytes each)
    size_t offset = 6;
    for (unsigned i = 0; i < componentCount && offset + 5 <= bytes.size(); ++i)
    {
        uint16_t cclass = bytes[offset] + (static_cast<uint16_t>(bytes[offset + 1]) << 8);
        if (cclass == NSM_FW_COMPONENT_CLASS)
        {
            outClass = cclass;
            outId = bytes[offset + 2] + (static_cast<uint16_t>(bytes[offset + 3]) << 8);
            outIndex = bytes[offset + 4];
            return true;
        }
        offset += 5;
    }
    return false;
}

// Run NSM helper with given args; response written to outPath. Returns exit
// code. Optional timeoutMs (0 = default).
static int runNsmHelper(uint32_t eid, uint8_t msgType, uint8_t cmdCode,
                       const std::string& payloadHex, const fs::path& outPath,
                       const fs::path& errPath, int timeoutMs = PROBE_TIMEOUT_MS)
{
    std::string cmd = std::string(NSM_EID_RAW_TOOL) + " --eid " +
                      std::to_string(eid) + " --instance 0 --message-type " +
                      std::to_string(static_cast<unsigned>(msgType)) +
                      " --command-code " + std::to_string(static_cast<unsigned>(cmdCode)) +
                      " --payload-hex \"" + payloadHex + "\" --timeout-ms " +
                      std::to_string(timeoutMs) + " > \"" + outPath.string() +
                      "\" 2>>\"" + errPath.string() + "\"";
    return runCommand(cmd);
}

static bool isRotDeviceEid(uint32_t eid, const fs::path& tmpDir)
{
    fs::path probeFile = tmpDir / ("eid_" + std::to_string(eid) + "_dcd_probe.bin");
    fs::path errFile = tmpDir / ("eid_" + std::to_string(eid) + "_dcd_probe.err");
    writeFile(errFile, "");

    if (runNsmHelper(eid, NSM_MESSAGE_TYPE_DCD,
                    NSM_CMD_QUERY_DEVICE_IDENTIFICATION, "", probeFile, errFile) !=
        0)
    {
        return false;
    }

    std::vector<uint8_t> bytes = readBinaryFile(probeFile);
    if (bytes.size() < 6 || bytes[0] != 0)
    {
        return false;
    }
    unsigned dataSize = bytes[3] + (static_cast<unsigned>(bytes[4]) << 8);
    if (dataSize < 1)
    {
        return false;
    }
    return bytes[5] == NSM_DEVICE_EROT_ID;
}

// Discover ROT targets via D-Bus (MCTP endpoints with PCI VDM + DCD probe).
using EidNamePair = std::pair<uint32_t, std::string>;
static std::vector<EidNamePair> discoverRotTargets(sdbusplus::bus_t& bus,
                                                  const fs::path& tmpDir)
{
    std::vector<EidNamePair> targets;
    const char* mapperService = "xyz.openbmc_project.ObjectMapper";
    const char* mapperPath = "/xyz/openbmc_project/object_mapper";
    const char* mapperInterface = "xyz.openbmc_project.ObjectMapper";
    const char* endpointInterface = "xyz.openbmc_project.MCTP.Endpoint";

    auto method = bus.new_method_call(mapperService, mapperPath,
                                     mapperInterface, "GetSubTree");
    method.append("/", 0, std::array<const char*, 1>{endpointInterface});

    std::map<std::string, std::map<std::string, std::vector<std::string>>> result;
    try
    {
        auto reply = bus.call(method);
        reply.read(result);
    }
    catch (const std::exception&)
    {
        return targets;
    }

    for (const auto& entry : result)
    {
        const auto& path = entry.first;
        const auto& services = entry.second;
        if (services.empty())
        {
            continue;
        }
        std::string service = services.begin()->first;

        auto getProp = [&](const char* prop) -> std::variant<uint8_t, uint16_t, std::vector<uint8_t>> {
            auto m = bus.new_method_call(service.c_str(), path.c_str(),
                                        "org.freedesktop.DBus.Properties", "Get");
            m.append(endpointInterface, prop);
            try
            {
                auto r = bus.call(m);
                std::variant<uint8_t, uint16_t, std::vector<uint8_t>> v;
                r.read(v);
                return v;
            }
            catch (...)
            {
                return std::vector<uint8_t>{};
            }
        };

        auto eidVar = getProp("EID");
        uint32_t eid = 0;
        if (std::holds_alternative<uint8_t>(eidVar))
        {
            eid = std::get<uint8_t>(eidVar);
        }
        else if (std::holds_alternative<uint16_t>(eidVar))
        {
            eid = std::get<uint16_t>(eidVar);
        }
        else
        {
            continue;
        }

        auto typesVar = getProp("SupportedMessageTypes");
        bool hasPciVdm = false;
        if (std::holds_alternative<std::vector<uint8_t>>(typesVar))
        {
            const auto& arr = std::get<std::vector<uint8_t>>(typesVar);
            hasPciVdm = std::find(arr.begin(), arr.end(), PCI_VDM_MSG_TYPE) != arr.end();
        }
        if (!hasPciVdm)
        {
            continue;
        }

        if (!isRotDeviceEid(eid, tmpDir))
        {
            continue;
        }

        std::string name = "EID_" + std::to_string(eid);
        targets.emplace_back(eid, name);
    }

    std::sort(targets.begin(), targets.end(),
              [](const EidNamePair& a, const EidNamePair& b) { return a.first < b.first; });
    return targets;
}

static bool nsmDiagDumpEid(const std::string& name, uint32_t eid,
                           const fs::path& tmpDir)
{
    fs::path outputFile = tmpDir / (name + "_rot_dump.bin");
    fs::path respFile = tmpDir / (name + "_rot_eid_resp.bin");
    fs::path errFile = tmpDir / (name + "_rot_error.log");
    writeFile(errFile, "");

    std::ofstream out(outputFile, std::ios::binary);
    if (!out)
    {
        return false;
    }

    uint8_t segmentId = 0;
    std::set<uint8_t> seen;
    const int timeoutMs = 5000; // full segment reads use default timeout

    for (int i = 0; i < 256; ++i)
    {
        if (seen.count(segmentId))
        {
            return true;
        }
        seen.insert(segmentId);

        const std::string payloadHex = toHexByte(segmentId);

        if (runNsmHelper(eid, NSM_MESSAGE_TYPE_DIAG,
                        NSM_CMD_GET_DEVICE_DIAGNOSTICS, payloadHex, respFile,
                        errFile, timeoutMs) != 0)
        {
            std::ofstream err(errFile, std::ios::app);
            err << name << ": rot_dump_nsm_eid_raw failed for segment "
                << static_cast<unsigned>(segmentId) << "\n";
            return false;
        }

        std::vector<uint8_t> bytes = readBinaryFile(respFile);
        if (bytes.size() < 6 || bytes[0] != 0)
        {
            std::ofstream err(errFile, std::ios::app);
            err << name << ": short or error response\n";
            return false;
        }
        unsigned dataSize = bytes[3] + (static_cast<unsigned>(bytes[4]) << 8);
        if (dataSize < 1 || bytes.size() < 5 + dataSize)
        {
            std::ofstream err(errFile, std::ios::app);
            err << name << ": invalid diagnostics response\n";
            return false;
        }
        uint8_t nextSegmentId = bytes[5];
        size_t segmentLen = dataSize - 1;
        if (segmentLen > 0)
        {
            out.write(reinterpret_cast<const char*>(bytes.data() + 6),
                      segmentLen);
        }

        if (nextSegmentId == 255 || nextSegmentId == segmentId)
        {
            return true;
        }
        segmentId = nextSegmentId;
    }

    std::ofstream err(errFile, std::ios::app);
    err << name << ": reached max diagnostics segments\n";
    return false;
}

static void collectBootStatusForEid(const std::string& name, uint32_t eid,
                                    const fs::path& tmpDir)
{
    fs::path outFile = tmpDir / (name + "_rot_query_boot_status.log");
    fs::path respFile = tmpDir / (name + "_rot_query_boot_status_resp.bin");
    fs::path errFile = tmpDir / (name + "_rot_query_boot_status.err");
    writeFile(errFile, "");

    if (!fs::exists(NSM_EID_RAW_TOOL) || !fs::is_regular_file(NSM_EID_RAW_TOOL))
    {
        writeFile(outFile,
                  "source=nsm_get_rot_state_info\neid=" + std::to_string(eid) +
                      "\nstatus=failed\nreason=missing_raw_tool\n");
        return;
    }

    uint16_t componentClass = NSM_FW_COMPONENT_CLASS;
    uint16_t componentId = 0;
    uint8_t componentIndex = 0;
    bool triedDiscovery = false;
    bool success = false;

    if (queryFwComponentForEid(eid, tmpDir, componentClass, componentId,
                               componentIndex))
    {
        triedDiscovery = true;
        const std::string payloadHex =
            makeRotStatePayloadHex(componentClass, componentId, componentIndex);

        if (runNsmHelper(eid, NSM_MESSAGE_TYPE_FW, NSM_CMD_GET_ROT_STATE_INFO,
                        payloadHex, respFile, errFile, 5000) == 0)
        {
            std::vector<uint8_t> resp = readBinaryFile(respFile);
            if (resp.size() >= 1 && resp[0] == 0)
            {
                writeBootStatusTextLog(outFile, eid, "query_fw_comp_id",
                                       componentClass, componentId,
                                       componentIndex, resp);
                success = true;
            }
        }
    }

    if (!success)
    {
        for (int cid = 0; cid <= 255; ++cid)
        {
            const std::string payloadHex = makeRotStatePayloadHex(
                NSM_FW_COMPONENT_CLASS, static_cast<uint16_t>(cid), 0);

            if (runNsmHelper(eid, NSM_MESSAGE_TYPE_FW, NSM_CMD_GET_ROT_STATE_INFO,
                            payloadHex, respFile, errFile, PROBE_TIMEOUT_MS) != 0)
            {
                continue;
            }

            std::vector<uint8_t> resp = readBinaryFile(respFile);
            if (resp.size() < 1 || resp[0] != 0)
            {
                continue;
            }

            writeBootStatusTextLog(outFile, eid, "bruteforce",
                                   NSM_FW_COMPONENT_CLASS,
                                   static_cast<uint16_t>(cid), 0, resp);
            success = true;
            break;
        }
    }

    if (!success)
    {
        std::string msg =
            "source=nsm_get_rot_state_info\neid=" + std::to_string(eid) +
            "\nstatus=failed\nattempt=" +
            (triedDiscovery ? "query_fw_comp_id_then_bruteforce"
                            : "bruteforce_only") +
            "\nreason=no_successful_completion_code\n";
        writeFile(outFile, msg);
    }
}

int main(int argc, char* argv[])
{
    std::string dumpPathArg;
    std::string dumpIdArg = "00000000";

    int c;
    while ((c = getopt(argc, argv, "hp:i:D")) != -1)
    {
        switch (c)
        {
            case 'h':
                std::cout << "Usage: rot_dump [-h] -p <file_path> -i <dump_id>\n"
                             "  -p  (required) path to put compressed dump to\n"
                             "  -i  dump id, default 00000000\n"
                             "  -D  debug (preserve temp)\n";
                return 0;
            case 'p':
                dumpPathArg = optarg;
                break;
            case 'i':
                dumpIdArg = optarg;
                break;
            case 'D':
                setenv("ROT_DUMP_KEEP_TMP", "1", 1);
                break;
            default:
                return 1;
        }
    }

    if (dumpPathArg.empty())
    {
        std::cerr << "argument -p is required" << std::endl;
        return 1;
    }

    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch())
                    .count();
    std::string templateName =
        "obmcdump_" + dumpIdArg + "_" + std::to_string(epoch);
    fs::path tmpDirPath = fs::path(TMP_DIR) / templateName;
    fs::path archivePath = fs::path(TMP_DIR) / (templateName + ".tar.xz");

    fs::create_directories(dumpPathArg);
    fs::create_directories(tmpDirPath);

    bool anyOutput = false;

    // Use a distinct legacy dump-id so erot_dump.sh does not reuse/remove
    // this process's tmp workspace when both run in the same second.
    const std::string legacyDumpId = dumpIdArg + "_legacy";
    if (mergeLegacyErotIntoTmp(tmpDirPath, legacyDumpId))
    {
        anyOutput = true;
    }

    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    std::vector<EidNamePair> rotTargets = discoverRotTargets(bus, tmpDirPath);

    if (fs::exists(NSM_EID_RAW_TOOL) && fs::is_regular_file(NSM_EID_RAW_TOOL))
    {
        for (const auto& [eid, name] : rotTargets)
        {
            if (!nsmDiagDumpEid(name, eid, tmpDirPath))
            {
                fs::path touchFile = tmpDirPath / (name + "_rot_dump.bin");
                std::ofstream(touchFile).flush();
            }
            collectBootStatusForEid(name, eid, tmpDirPath);
            anyOutput = true;
        }
    }
    else if (!rotTargets.empty())
    {
        std::cerr << "Error: " << NSM_EID_RAW_TOOL << " not found" << std::endl;
    }

    if (!anyOutput)
    {
        std::cerr << "No ROT data collected from legacy EROT or NSM discovery"
                  << std::endl;
        return 1;
    }

    pruneIntermediateArtifacts(tmpDirPath);

    std::string tarCmd = "tar -Jcf \"" + archivePath.string() + "\" -C \"" +
                         tmpDirPath.parent_path().string() + "\" " +
                         tmpDirPath.filename().string();
    if (runCommand(tarCmd) != 0)
    {
        std::cerr << "Compression failed: " << archivePath << std::endl;
        return 1;
    }

    fs::path destArchive = fs::path(dumpPathArg) / archivePath.filename();
    if (archivePath != destArchive)
    {
        fs::copy(archivePath, destArchive, fs::copy_options::overwrite_existing);
    }

    if (std::getenv("ROT_DUMP_KEEP_TMP") == nullptr)
    {
        fs::remove_all(tmpDirPath);
        fs::remove(archivePath);
    }

    return 0;
}
