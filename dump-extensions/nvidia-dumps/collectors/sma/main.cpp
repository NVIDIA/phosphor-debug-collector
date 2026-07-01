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

// nv-collector-sma - SMA dump collector. Enumerates Configuration.Dump records
// with SupportedDumps=["SMA"] and runs mctp-vdm-util download_log per EID.

#include "collectors/common/collector_helpers.hpp"
#include "collectors/common/collector_main.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace
{
constexpr auto MCTP_VDM_UTIL = "/usr/bin/mctp-vdm-util";
// download_log writes its binary payload here, not to stdout.
constexpr auto MCTP_LOG_FILE = "/var/mctp-vdm-output.bin";
} // namespace

int main(int argc, char** argv)
{
    using namespace phosphor::dump::collectors;
    auto cli = parseCli(argc, argv);
    if (cli.outDir.empty() || cli.entryId.empty())
    {
        lg2::error("nv-collector-sma: missing --out-dir/--entry-id");
        return EXIT_FAILURE;
    }
    if (!ensureOutDir(cli.outDir))
    {
        return EXIT_FAILURE;
    }

    auto targets = enumerateDumpTargetsForKind("SMA");
    if (targets.empty())
    {
        // Fail fast so PDC marks the entry Failed rather than idling the
        // wall-clock timeout.
        lg2::error("nv-collector-sma: no Configuration.Dump records with "
                   "SupportedDumps=[\"SMA\"]; nothing to collect");
        return EXIT_FAILURE;
    }
    lg2::info("nv-collector-sma: dispatching for {COUNT} SMA target(s)",
              "COUNT", targets.size());

    // Stage under /tmp/<stem>, then tar into cli.outDir.
    std::string stem = obmcdumpStem(cli.entryId);
    std::string stage = "/tmp/" + stem;
    if (!ensureOutDir(stage))
    {
        return EXIT_FAILURE;
    }

    // Execution_Report.txt: a header plus one error line per failed EID;
    // baseline comparisons expect it alongside the *_sma_dump.bin artifacts.
    std::string reportPath = stage + "/Execution_Report.txt";
    std::ofstream report(reportPath, std::ios::app);
    if (report)
    {
        report << "Collecting SMA debug dump\n";
    }

    bool anyOk = false;
    for (const auto& tgt : targets)
    {
        std::string outPath = stage + "/" + tgt.name + "_sma_dump.bin";
        // Logged verbatim into the report on failure.
        std::string cmddump = std::string(MCTP_VDM_UTIL) +
                              " -c download_log -t " + std::to_string(tgt.eid);
        // Capture the binary log from MCTP_LOG_FILE; touch empty file on
        // failure.
        std::string cmd = std::format(
            "rm -f {0}; {1} >/dev/null 2>&1; rc=$?; "
            "if [ -f {0} ]; then mv -f {0} {2}; else touch {2}; fi; exit $rc",
            MCTP_LOG_FILE, cmddump, outPath);
        if (runExternal(cmd) == 0)
        {
            anyOk = true;
        }
        else if (report)
        {
            report << "An error occured while running " << cmddump << "\n";
        }
    }
    report.close();

    if (!anyOk)
    {
        cleanupStage(stage);
        return EXIT_FAILURE;
    }
    // Package into cli.outDir so PDC's inotify watch registers the entry.
    if (!makeTarball(cli.outDir, stage))
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
