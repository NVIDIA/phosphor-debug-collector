/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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

// nv-collector-erot - ERoT dump collector. Enumerates Configuration.Dump
// records with SupportedDumps=["RoT"] and runs mctp-vdm-util download_log per
// EID. HMC adds query_boot_status/selftest plus an OCP-Recovery sweep; BMC adds
// IROT/VROT via rot_dump.

#include "collectors/common/collector_helpers.hpp"
#include "collectors/common/collector_main.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr auto MCTP_VDM_UTIL = "/usr/bin/mctp-vdm-util";
// mctp-vdm-util writes the download_log binary payload to this fixed file.
constexpr auto MCTP_LOG_FILE = "/var/mctp-vdm-output.bin";
// OCP-Recovery sweep tooling (CMS/OOBHUB); runs when OCPRecovery records exist.
constexpr auto OCP_RECOVERY_TOOL = "/usr/bin/ocp-recovery-tool";
constexpr auto I2C_TRANSFER = "i2ctransfer";
constexpr auto CMS_LOG_FILE = "/var/cms2_log.bin";
constexpr auto OCP_RECOVERY_IFACE =
    "xyz.openbmc_project.Configuration.OCPRecovery";
// IROT/VROT collector; runs when /usr/bin/rot_dump is installed.
constexpr auto ROT_DUMP_TOOL = "/usr/bin/rot_dump";

// Append a message to the Execution_Report.txt in the staging dir.
void report(const std::string& reportFile, const std::string& msg)
{
    std::ofstream f(reportFile, std::ios::app);
    if (f)
    {
        f << msg << "\n";
    }
}

// Numeric EM property as string; handles uint64/int64 ints and quoted hex
// strings (e.g. "0x48").
std::string emNumStr(const phosphor::dump::collectors::EmObject& obj,
                     const std::string& key)
{
    auto it = obj.properties.find(key);
    if (it == obj.properties.end())
    {
        return {};
    }
    if (auto p = std::get_if<uint64_t>(&it->second))
    {
        return std::to_string(*p);
    }
    if (auto p = std::get_if<int64_t>(&it->second))
    {
        return std::to_string(*p);
    }
    if (auto p = std::get_if<std::string>(&it->second))
    {
        return *p;
    }
    return {};
}

// Find the i2c bus number by locating i2c-* dirs under
// /sys/bus/usb/devices/<port>/.
std::optional<std::string> getI2cBusFromUsbPort(const std::string& usbPort)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = "/sys/bus/usb/devices/" + usbPort;
    for (const auto& entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_directory(ec))
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.starts_with("i2c-"))
        {
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(name.back())) == 0)
        {
            continue;
        }
        // Trailing digit sequence is the bus number.
        auto pos = name.find_last_not_of("0123456789");
        return (pos == std::string::npos) ? name : name.substr(pos + 1);
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace phosphor::dump::collectors;
    auto cli = parseCli(argc, argv);
    if (cli.outDir.empty() || cli.entryId.empty())
    {
        lg2::error("nv-collector-erot: missing --out-dir/--entry-id");
        return EXIT_FAILURE;
    }
    if (!ensureOutDir(cli.outDir))
    {
        return EXIT_FAILURE;
    }

    auto targets = enumerateDumpTargetsForKind("RoT");
    if (targets.empty())
    {
        lg2::error("nv-collector-erot: no Configuration.Dump records with "
                   "SupportedDumps=[\"RoT\"]; nothing to collect");
        return EXIT_FAILURE;
    }
    lg2::info("nv-collector-erot: dispatching for {COUNT} RoT target(s)",
              "COUNT", targets.size());

    std::string stem = obmcdumpStem(cli.entryId);
    std::string stage = "/tmp/" + stem;
    if (!ensureOutDir(stage))
    {
        return EXIT_FAILURE;
    }

    std::string reportFile = stage + "/Execution_Report.txt";
    report(reportFile, "Collecting ROT debug dump");

    bool anyOk = false;

    // HMC role (Configuration.Dump records named HGX_*) adds
    // query_boot_status/selftest to the per-EID MCTP phase.
    const bool hmc = isHmcFromEm("RoT");

    // EIDs (decimal) whose MCTP download_log succeeded; used below to skip the
    // contending CP2112/I2C path for already-covered devices.
    std::set<std::string> mctpDlOkEids;

    // MCTP phase
    for (const auto& tgt : targets)
    {
        std::string outBase = stage + "/" + tgt.name;
        std::string eid = std::to_string(tgt.eid);

        // download_log writes binary to MCTP_LOG_FILE; move on success, touch a
        // placeholder on failure for a deterministic file set.
        const std::string dlDest = outBase + "_dump.bin";
        std::string dlCmd = std::format(
            "rm -f {0}; {1} -c download_log -t {2} >/dev/null 2>&1; rc=$?; "
            "if [ -f {0} ]; then mv -f {0} {3}; else touch {3}; fi; exit $rc",
            MCTP_LOG_FILE, MCTP_VDM_UTIL, eid, dlDest);
        if (runExternal(dlCmd) == 0)
        {
            anyOk = true;
            mctpDlOkEids.insert(eid);
        }
        else
        {
            report(reportFile,
                   "An error occurred while running download_log for EID " +
                       eid);
        }

        if (hmc)
        {
            // query_boot_status (HMC-only)
            const std::string bootLog = outBase + "_query_boot_status.log";
            if (runExternal(std::format(
                    "{} -c query_boot_status -t {} -m -j >> {} 2>&1",
                    MCTP_VDM_UTIL, eid, bootLog)) == 0)
            {
                anyOk = true;
            }
            else
            {
                runExternal("touch " + bootLog);
                report(reportFile,
                       "An error occurred while running query_boot_status for "
                       "EID " +
                           eid);
            }

            // selftest (HMC-only): archive log on success, touch placeholder on
            // failure.
            const std::string selftestTmp =
                std::format("/tmp/{}_rot_selftest.log", tgt.name);
            const std::string selftestDest = outBase + "_rot_selftest.log";
            if (runExternal(
                    std::format("{} -c selftest 8 0 0 0 -t {} >> {} 2>&1",
                                MCTP_VDM_UTIL, eid, selftestTmp)) == 0)
            {
                runExternal(
                    std::format("mv -f {} {}", selftestTmp, selftestDest));
                anyOk = true;
            }
            else
            {
                runExternal("rm -f " + selftestTmp);
                runExternal("touch " + selftestDest);
                report(reportFile,
                       "An error occurred while running selftest for EID " +
                           eid);
            }
        } // if (hmc)
    }

    // OCP-Recovery sweep: enumerate Configuration.OCPRecovery objects (empty on
    // platforms without them) and run ocp-recovery-tool + i2ctransfer per dev.
    auto ocpObjs = enumerateEmObjects({OCP_RECOVERY_IFACE});
    // Append a shell-echo of msg into the per-device log.
    auto echoLog = [](const std::string& file, const std::string& msg) {
        runExternal(std::format("echo '{}' >> {} 2>&1", msg, file));
    };
    for (const auto& obj : ocpObjs)
    {
        const std::string devName = emString(obj, "Name");
        const std::string i2cAddr = emNumStr(obj, "I2CAddress");
        const std::string usbPort = emString(obj, "USBPort");
        // Some platforms publish I2CBus directly instead of USBPort.
        const std::string i2cBusDirect = emNumStr(obj, "I2CBus");

        if (devName.empty() || i2cAddr.empty() ||
            (usbPort.empty() && i2cBusDirect.empty()))
        {
            lg2::warning(
                "nv-collector-erot: OCPRecovery object '{PATH}' missing "
                "Name/I2CAddress/USBPort; skipping",
                "PATH", obj.path);
            continue;
        }

        // If MCTP download_log already succeeded for this device (matched via
        // MctpEID), skip the contending CP2112 path to avoid IBI timing
        // conflicts on the shared bridge. No-op when the record has no MctpEID.
        const std::string mctpEid = emNumStr(obj, "MctpEID");
        if (!mctpEid.empty() && mctpDlOkEids.contains(mctpEid))
        {
            report(reportFile,
                   std::format("MCTP download_log succeeded for {} (EID {}); "
                               "skipping CP2112 ocp-recovery-tool and "
                               "i2ctransfer for i2c address {}",
                               devName, mctpEid, i2cAddr));
            continue;
        }

        const std::string devLog = std::format("/tmp/{}.log", devName);

        // GetDeviceStatus
        std::string header =
            std::format("ocp-recovery-tool GetDeviceStatus for {} on usb port "
                        "{} and i2c address {}",
                        devName, usbPort, i2cAddr);
        echoLog(devLog, header);
        std::string devStatusCmd =
            !usbPort.empty()
                ? std::format("{} GetDeviceStatus -p {} -s {} >> {} 2>&1",
                              OCP_RECOVERY_TOOL, usbPort, i2cAddr, devLog)
                : std::format("{} GetDeviceStatus -b {} -s {} >> {} 2>&1",
                              OCP_RECOVERY_TOOL, i2cBusDirect, i2cAddr, devLog);
        if (runExternal(devStatusCmd) != 0)
        {
            const std::string err =
                "ocp-recovery-tool GetDeviceStatus failed for " + devName;
            echoLog(devLog, err);
            report(reportFile, err);
        }

        // GetRecoveryStatus
        header =
            std::format("ocp-recovery-tool GetRecoveryStatus for {} on usb "
                        "port {} and i2c address {}",
                        devName, usbPort, i2cAddr);
        echoLog(devLog, header);
        std::string recStatusCmd =
            !usbPort.empty()
                ? std::format("{} GetRecoveryStatus -p {} -s {} >> {} 2>&1",
                              OCP_RECOVERY_TOOL, usbPort, i2cAddr, devLog)
                : std::format("{} GetRecoveryStatus -b {} -s {} >> {} 2>&1",
                              OCP_RECOVERY_TOOL, i2cBusDirect, i2cAddr, devLog);
        if (runExternal(recStatusCmd) != 0)
        {
            const std::string err =
                "ocp-recovery-tool GetRecoveryStatus failed for " + devName;
            echoLog(devLog, err);
            report(reportFile, err);
        }

        // GetCMSLogs writes binary output to CMS_LOG_FILE.
        runExternal("rm -f " + std::string(CMS_LOG_FILE));
        header = std::format("ocp-recovery-tool GetCMSLogs for {} on usb port "
                             "{} and i2c address {}",
                             devName, usbPort, i2cAddr);
        echoLog(devLog, header);
        std::string cmsCmd =
            !usbPort.empty()
                ? std::format("{} GetCMSLogs -p {} -s {} -w 2 >> {} 2>&1",
                              OCP_RECOVERY_TOOL, usbPort, i2cAddr, devLog)
                : std::format("{} GetCMSLogs -b {} -s {} -w 2 >> {} 2>&1",
                              OCP_RECOVERY_TOOL, i2cBusDirect, i2cAddr, devLog);
        if (runExternal(cmsCmd) != 0)
        {
            const std::string err =
                "ocp-recovery-tool GetCMSLogs failed for " + devName;
            echoLog(devLog, err);
            report(reportFile, err);
        }

        // OOBHUB: resolve i2c bus (from USBPort or I2CBus), then i2ctransfer.
        const std::string cmsDest =
            std::format("{}/{}_CMS.bin", stage, devName);
        const std::string oobhubDest =
            std::format("{}/{}_OOBHUB.bin", stage, devName);
        std::string i2cBus = i2cBusDirect;
        if (i2cBus.empty() && !usbPort.empty())
        {
            auto discovered = getI2cBusFromUsbPort(usbPort);
            if (discovered)
            {
                i2cBus = *discovered;
            }
            else
            {
                const std::string err =
                    "No i2c bus found for USB port " + usbPort;
                echoLog(devLog, err);
                report(reportFile, err);
            }
        }
        if (!i2cBus.empty())
        {
            header = std::format(
                "OOBHUB logs for {} on usb port {} and i2c address {}", devName,
                usbPort, i2cAddr);
            echoLog(devLog, header);
            const std::string oobhubTmp = "/tmp/oobhub_logs.bin";
            if (runExternal(
                    std::format("{} -y {} w1@{} 0x2c r256 >> {} 2>&1",
                                I2C_TRANSFER, i2cBus, i2cAddr, oobhubTmp)) == 0)
            {
                runExternal(std::format("mv -f {} {}", oobhubTmp, oobhubDest));
            }
            else
            {
                const std::string err = std::format(
                    "i2ctransfer for OOBHUB logs failed for {} on i2c bus {}",
                    devName, i2cBus);
                echoLog(devLog, err);
                report(reportFile, err);
                runExternal("touch " + oobhubDest);
            }
        }

        // Move per-device log and CMS binary to staging dir.
        runExternal(std::format("mv -f {} {}/", devLog, stage));
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::exists(CMS_LOG_FILE, ec) && !ec)
        {
            runExternal(std::format("mv -f {} {}", CMS_LOG_FILE, cmsDest));
        }
        else
        {
            report(reportFile,
                   "ocp-recovery-tool GetCMSLogs produced no output for " +
                       devName);
            runExternal("touch " + cmsDest);
        }

        anyOk = true;
    }

    // IROT/VROT collection: run rot_dump into the staging dir when the helper
    // is installed; a no-op otherwise (e.g. HMC platforms).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::exists(ROT_DUMP_TOOL, ec) && !ec)
        {
            if (runExternal(std::string(ROT_DUMP_TOOL) + " -o " + stage) != 0)
            {
                report(reportFile,
                       "IROT/VROT collection failed or no devices found");
            }
        }
    }

    if (!anyOk)
    {
        cleanupStage(stage);
        return EXIT_FAILURE;
    }
    if (!makeTarball(cli.outDir, stage))
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
