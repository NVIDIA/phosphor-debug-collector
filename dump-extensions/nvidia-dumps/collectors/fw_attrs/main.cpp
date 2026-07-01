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

// nv-collector-fw-attrs - runs hw_checkout.sh firmware and copies out the
// checker log. HMC keeps hmc_checker.log as-is; BMC relabels HMC->BMC and
// copies it as bmc_checker.log.

#include "collectors/common/collector_helpers.hpp"
#include "collectors/common/collector_main.hpp"
#include "dump_kinds.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace
{
constexpr auto HW_CHECKOUT_SH = "/usr/bin/hw_checkout.sh";
} // namespace

int main(int argc, char** argv)
{
    using namespace phosphor::dump::collectors;
    auto cli = parseCli(argc, argv);
    if (cli.outDir.empty() || cli.entryId.empty())
    {
        lg2::error("nv-collector-fw-attrs: missing --out-dir/--entry-id");
        return EXIT_FAILURE;
    }
    if (!ensureOutDir(cli.outDir))
    {
        return EXIT_FAILURE;
    }

    // Stage under /tmp, then tar into cli.outDir; loose files in the watched
    // dir would trip PDC's "Invalid Dump file name" path.
    std::string stem = obmcdumpStem(cli.entryId);
    std::string stage = "/tmp/" + stem;
    if (!ensureOutDir(stage))
    {
        return EXIT_FAILURE;
    }
    std::string consoleLog = stage + "/console.log";

    // Role (hmc/bmc) from the EM record Name (HGX_* = HMC); command is fixed,
    // only the checker-log label and the BMC relabel differ.
    bool hmc = isHmcFromEm("FwAttrs");
    const auto& args = phosphor::dump::checkoutArgs().at("FwAttrs");
    std::string subsystems = hmc ? args.hmcArgs : args.bmcArgs;
    std::string label = hmc ? "hmc" : "bmc";
    std::string checkerLog = std::format("{}/{}_checker.log", stage, label);

    // hw_checkout.sh writes its detailed log to /tmp/hmc_checker.log.
    runExternal(
        std::format("{} {} > {} 2>&1", HW_CHECKOUT_SH, subsystems, consoleLog));
    if (!hmc)
    {
        // BMC relabel: console.log header and the checker log's "HMC UTC Time".
        runExternal(
            std::format("sed -i 's/## HMC Firmware Attributes ##/"
                        "## BMC Firmware Attributes ##/g' {} 2>/dev/null",
                        consoleLog));
        runExternal("sed -i 's/HMC UTC Time/BMC UTC Time/g' "
                    "/tmp/hmc_checker.log 2>/dev/null");
    }
    runExternal(
        std::format("cp /tmp/hmc_checker.log {} 2>/dev/null", checkerLog));

    // Always deliver the archive; only a packaging failure is fatal.
    if (!makeTarball(cli.outDir, stage))
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
