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

// nv-collector-glacier-i2c-log - I2C log download from the Glacier device.

#include "collectors/common/collector_main.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace
{

void writeErrorJson(const std::string& outDir, const std::string& deviceName,
                    const std::string& reason)
{
    std::string path = outDir + "/" + deviceName + ".error.json";
    std::ofstream f(path);
    if (!f)
    {
        lg2::error("Failed to open '{PATH}' for error.json", "PATH", path);
        return;
    }
    f << "{\n"
      << R"(  "device": ")" << deviceName << "\",\n"
      << R"(  "reason": ")" << reason << "\"\n"
      << "}\n";
}

int validateManifest(const std::string& outDir,
                     const std::vector<std::string>& expectedFiles)
{
    namespace fs = std::filesystem;
    if (expectedFiles.empty())
    {
        return 0;
    }
    std::string statusPath = outDir + "/collection_status.json";
    std::ofstream out(statusPath);
    if (!out)
    {
        lg2::error("manifest_validator: cannot open '{PATH}'", "PATH",
                   statusPath);
        return 1;
    }
    int missing = 0;
    out << "{\n  \"files\": [\n";
    bool first = true;
    for (const auto& name : expectedFiles)
    {
        if (name == "collection_status.json")
        {
            continue;
        }
        fs::path p = fs::path(outDir) / name;
        std::error_code ec;
        bool exists = fs::exists(p, ec) && !fs::is_directory(p, ec);
        uintmax_t size = exists ? static_cast<uintmax_t>(fs::file_size(p, ec))
                                : 0;
        bool ok = exists && size > 0;
        if (!ok)
        {
            ++missing;
        }
        if (!first)
        {
            out << ",\n";
        }
        first = false;
        out << R"(    {"name": ")" << name << R"(", "present": )"
            << (exists ? "true" : "false") << ", \"size\": " << size
            << ", \"ok\": " << (ok ? "true" : "false") << "}";
    }
    out << "\n  ],\n  \"status\": \"" << (missing == 0 ? "complete" : "partial")
        << "\"\n}\n";
    if (missing != 0)
    {
        lg2::warning(
            "manifest_validator: {N} expected file(s) missing or empty under '{DIR}'",
            "N", missing, "DIR", outDir);
        return 1;
    }
    return 0;
}
/** @brief Typed int64 accessor for EmObject. Returns 0 when absent or
 *  non-integer.
 */
int64_t emInt64(const phosphor::dump::collectors::EmObject& obj,
                const std::string& key)
{
    auto it = obj.properties.find(key);
    if (it == obj.properties.end())
    {
        return 0;
    }
    if (auto p = std::get_if<int64_t>(&it->second))
    {
        return *p;
    }
    // entity-manager publishes JSON integers as D-Bus `t` (uint64) by default.
    if (auto p = std::get_if<uint64_t>(&it->second))
    {
        return static_cast<int64_t>(*p);
    }
    if (auto p = std::get_if<uint8_t>(&it->second))
    {
        return static_cast<int64_t>(*p);
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    using namespace phosphor::dump::collectors;
    auto cli = parseCli(argc, argv);
    if (cli.outDir.empty() || cli.entryId.empty())
    {
        lg2::error(
            "nv-collector-glacier-i2c-log: missing --out-dir/--entry-id");
        return EXIT_FAILURE;
    }
    if (!ensureOutDir(cli.outDir))
    {
        return EXIT_FAILURE;
    }

    EmCollectorArgs em;
    std::vector<std::string> expected;
    if (auto rec = fetchEmRecord(cli.emConfigPath); rec)
    {
        expected = std::move(rec->first);
        em = std::move(rec->second);
    }
    if (!em.i2cBus)
    {
        if (auto v = cli.extra.find("I2CBus"); v != cli.extra.end())
        {
            try
            {
                em.i2cBus = std::stoll(v->second);
            }
            catch (...)
            {
                lg2::debug(
                    "nv-collector-glacier-i2c-log: unparsable I2CBus '{VAL}'",
                    "VAL", v->second);
            }
        }
    }
    if (!em.i2cAddress)
    {
        if (auto v = cli.extra.find("I2CAddress"); v != cli.extra.end())
        {
            em.i2cAddress = v->second;
        }
    }

    // Fallback: with no --em-config-path and no CLI I2CBus/I2CAddress, walk
    // Configuration.Dump records with SupportedDumps=["GlacierI2cLog"] for the
    // I2C target.
    if (cli.emConfigPath.empty() && (!em.i2cBus || !em.i2cAddress))
    {
        for (const auto& obj :
             enumerateEmObjects({"xyz.openbmc_project.Configuration.Dump"}))
        {
            auto supported = emStringVec(obj, "SupportedDumps");
            if (std::find(supported.begin(), supported.end(),
                          "GlacierI2cLog") == supported.end())
            {
                continue;
            }
            if (!em.i2cBus)
            {
                auto bus = emInt64(obj, "I2CBus");
                if (bus != 0)
                {
                    em.i2cBus = bus;
                }
            }
            if (!em.i2cAddress)
            {
                auto addr = emString(obj, "I2CAddress");
                if (!addr.empty())
                {
                    em.i2cAddress = addr;
                }
            }
            lg2::info(
                "nv-collector-glacier-i2c-log: discovered I2C target from EM "
                "(path={PATH})",
                "PATH", obj.path);
            break;
        }
    }

    auto probe = probeI2c(em);
    if (probe == ProbeResult::Unreachable)
    {
        writeErrorJson(cli.outDir, "Glacier",
                       "I2C device unreachable at probe time");
        return 2;
    }

    // Single-quote the daemon-supplied path/id (defense-in-depth; not user
    // input) so they can't be interpreted as shell tokens.
    std::string cmd =
        std::format("/usr/bin/glacier_i2c_log_dl.sh -p '{}' -i '{}'",
                    cli.outDir, cli.entryId);
    int rc = runExternal(cmd);
    if (rc != 0)
    {
        lg2::warning("nv-collector-glacier-i2c-log: helper exited {RC}", "RC",
                     rc);
    }

    int mfr = validateManifest(cli.outDir, expected);
    if (rc == 0 && mfr == 0)
    {
        return EXIT_SUCCESS;
    }
    return (mfr == 0) ? rc : 1;
}
