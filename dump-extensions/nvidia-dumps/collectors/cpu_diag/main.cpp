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

// nv-collector-cpu-diag - receives --device-type <CPU_0|CPU_1> and wraps
// cpu-diagnostic-dump (or dreport as a fallback when the native tool is
// absent).

#include "collectors/common/collector_helpers.hpp"
#include "collectors/common/collector_main.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace
{
constexpr auto CPU_DIAG_BIN = "/usr/bin/cpu-diagnostic-dump";
constexpr auto CPU_DIAG_TEMP = "/tmp/cpu_diagnostic_dump";
constexpr auto DREPORT_BIN = "/usr/bin/dreport";
} // namespace

int main(int argc, char** argv)
{
    using namespace phosphor::dump::collectors;
    auto cli = parseCli(argc, argv);
    if (cli.outDir.empty() || cli.entryId.empty())
    {
        lg2::error("nv-collector-cpu-diag: missing --out-dir/--entry-id");
        return EXIT_FAILURE;
    }
    if (!ensureOutDir(cli.outDir))
    {
        return EXIT_FAILURE;
    }

    std::string deviceType;
    if (auto it = cli.extra.find("device-type"); it != cli.extra.end())
    {
        deviceType = it->second;
    }
    else if (auto it = cli.extra.find("DeviceType"); it != cli.extra.end())
    {
        deviceType = it->second;
    }
    if (deviceType.empty())
    {
        lg2::error("nv-collector-cpu-diag: missing --device-type");
        return EXIT_FAILURE;
    }

    // The wrapped tools produce the obmcdump archive themselves, so this
    // collector does not stage/tar.
    std::error_code ec;
    bool haveCpuDiagBin = std::filesystem::exists(CPU_DIAG_BIN, ec);

    // Single-quote args: deviceType is a (Redfish-validated) user-facing value.
    std::string cmd;
    if (haveCpuDiagBin)
    {
        cmd = std::format("{} -p '{}' -i '{}' -t {} -d '{}'", CPU_DIAG_BIN,
                          cli.outDir, cli.entryId, CPU_DIAG_TEMP, deviceType);
    }
    else
    {
        cmd = std::format("{} -d '{}' -i '{}' -q -v -t system -a "
                          "DeviceType='{}'",
                          DREPORT_BIN, cli.outDir, cli.entryId, deviceType);
    }
    int rc = runExternal(cmd);

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
