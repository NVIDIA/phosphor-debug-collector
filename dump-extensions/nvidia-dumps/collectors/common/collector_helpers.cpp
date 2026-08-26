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

#include "collector_helpers.hpp"

#include "tar_compress_lock.hpp"

#include <phosphor-logging/lg2.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>

namespace phosphor
{
namespace dump
{
namespace collectors
{

std::string obmcdumpStem(const std::string& entryId)
{
    auto t =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return "obmcdump_" + entryId + "_" +
           std::to_string(static_cast<long long>(t));
}

bool makeTarball(const std::string& outDir, const std::string& stageDir)
{
    namespace fs = std::filesystem;
    fs::path stage(stageDir);
    std::string stem = stage.filename().string();
    std::string parent = stage.parent_path().string();
    fs::path archive = fs::path(outDir) / (stem + ".tar.xz");

    // tar+xz the staging dir as a single top-level "<stem>/" entry.
    int rc = phosphor::dump::compression::runShellWithLock(
        phosphor::dump::compression::lockPath,
        {"tar", "-C", parent, "-Jcf", archive.string(), stem});

    std::error_code ec;
    fs::remove_all(stage, ec); // best-effort staging cleanup

    if (rc != 0)
    {
        lg2::error("makeTarball: tar failed (rc={RC}) for archive '{ARCHIVE}'",
                   "RC", rc, "ARCHIVE", archive.string());
        return false;
    }
    // Completion marker matching the legacy dreport/BMC-dump phrase so the
    // system-dump flow logs the same searchable string.
    lg2::info("Report is available in {DIR}", "DIR", outDir);
    return true;
}

void cleanupStage(const std::string& stageDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(stageDir, ec);
    if (ec)
    {
        lg2::debug("cleanupStage: could not remove staging dir '{DIR}': {ERR}",
                   "DIR", stageDir, "ERR", ec.message());
    }
}

} // namespace collectors
} // namespace dump
} // namespace phosphor
