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

// nv-collector-fpga - C++ port of fpga_dump.sh. Reads the FPGA register-page
// table from the EM Configuration.Dump Pages<N> interfaces and issues one
// i2ctransfer per page, producing per-page .bin files; also runs the hardcoded
// APB controller dump.

#include "collectors/common/collector_helpers.hpp"
#include "collectors/common/collector_main.hpp"
#include "collectors/common/em_args.hpp"

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace
{
constexpr auto I2CTRANSFER = "/usr/sbin/i2ctransfer";
constexpr auto DUMP_IFACE = "xyz.openbmc_project.Configuration.Dump";
// EM names each exploded Pages[] element interface "<DUMP_IFACE>.Pages<N>".
constexpr auto PAGES_IFACE_PREFIX =
    "xyz.openbmc_project.Configuration.Dump.Pages";

// FPGA APB controller dump constants (port of fpga_dump.sh dump_apb_registers),
// hardware constants reproduced verbatim. APB_BUS maps module index -> I2C bus;
// APB registers are read at slave 0x11, 4 bytes each.
constexpr auto APB_I2C_ADDR = "0x11";
constexpr int APB_READ_SIZE = 4;
constexpr int APB_NUM_MOD = 2;
constexpr int APB_NUM_GPU = 2;
const std::array<int, 2> APB_BUS = {1, 2};

// (B0, B1) register-offset pairs, transcribed verbatim from fpga_dump.sh.
const std::vector<std::pair<std::string, std::string>> apbControllerOffsets = {
    {"0x00", "0x10"}, {"0x01", "0x10"}, {"0x02", "0x10"}, {"0x03", "0x10"},
    {"0x04", "0x10"}, {"0x07", "0x10"}, {"0x08", "0x10"}, {"0x09", "0x10"},
    {"0x0d", "0x10"}, {"0x0f", "0x10"}, {"0x10", "0x10"}, {"0x11", "0x10"},
    {"0x12", "0x10"}, {"0x13", "0x10"}, {"0x14", "0x10"}, {"0x15", "0x10"},
    {"0x17", "0x10"}, {"0x18", "0x10"}, {"0x1b", "0x10"}, {"0x2c", "0x10"},
    {"0x2d", "0x10"}, {"0x2e", "0x10"}, {"0x2f", "0x10"}, {"0x30", "0x10"},
    {"0x32", "0x10"}, {"0x33", "0x10"}, {"0x34", "0x10"}, {"0x35", "0x10"},
    {"0x38", "0x10"}, {"0x39", "0x10"}, {"0x3a", "0x10"}, {"0x80", "0x10"},
    {"0x81", "0x10"}, {"0x82", "0x10"}, {"0x83", "0x10"}, {"0x98", "0x10"},
    {"0x00", "0x14"}, {"0x01", "0x14"}, {"0x02", "0x14"}, {"0x03", "0x14"},
    {"0x04", "0x14"}, {"0x05", "0x14"}, {"0x06", "0x14"}, {"0x07", "0x14"},
    {"0x08", "0x14"}, {"0x09", "0x14"}, {"0x0a", "0x14"}, {"0x0b", "0x14"},
    {"0x0c", "0x14"}, {"0xc0", "0x14"}, {"0xc1", "0x14"}, {"0xc2", "0x14"},
    {"0xc3", "0x14"}, {"0xc4", "0x14"}, {"0xc5", "0x14"}, {"0xc6", "0x14"},
    {"0xc7", "0x14"}, {"0xc8", "0x14"}, {"0xc9", "0x14"}, {"0xca", "0x14"},
    {"0xcb", "0x14"}, {"0xcc", "0x14"}, {"0xc0", "0x15"}, {"0xc1", "0x15"},
    {"0xc2", "0x15"}, {"0xc3", "0x15"}, {"0xc4", "0x15"}, {"0xc5", "0x15"},
    {"0xc6", "0x15"}, {"0xc7", "0x15"}, {"0xc8", "0x15"}, {"0xc9", "0x15"},
    {"0xca", "0x15"}, {"0xcb", "0x15"}, {"0xcc", "0x15"}, {"0xcd", "0x15"},
    {"0xce", "0x15"}, {"0xcf", "0x15"}, {"0xd0", "0x15"}, {"0xd1", "0x15"},
    {"0xd2", "0x15"}, {"0xd3", "0x15"}, {"0xd4", "0x15"}, {"0xd5", "0x15"},
    {"0xd6", "0x15"}};

const std::vector<std::pair<std::string, std::string>> gpuControllerOffsets = {
    {"0x00", "0x50"}, {"0x12", "0x50"}, {"0x13", "0x50"},
    {"0x14", "0x50"}, {"0x15", "0x50"}, {"0x16", "0x50"},
    {"0x17", "0x50"}, {"0x18", "0x50"}, {"0x19", "0x50"}};

using phosphor::dump::collectors::em_args::EmCpldPage;

// Scalar property variant for a Pages<N> interface. EM publishes numeric
// scalars (incl. coerced hex strings) as uint64; only Idx is a genuine string.
using PageVariant =
    std::variant<std::string, uint64_t, int64_t, uint8_t, bool, double>;

std::optional<std::string> pageStr(
    const std::map<std::string, PageVariant>& props, const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    if (auto p = std::get_if<std::string>(&it->second))
    {
        return *p;
    }
    return std::nullopt;
}

std::optional<uint64_t> pageUint(
    const std::map<std::string, PageVariant>& props, const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    if (auto p = std::get_if<uint64_t>(&it->second))
    {
        return *p;
    }
    if (auto p = std::get_if<int64_t>(&it->second))
    {
        return static_cast<uint64_t>(*p);
    }
    if (auto p = std::get_if<uint8_t>(&it->second))
    {
        return static_cast<uint64_t>(*p);
    }
    if (auto p = std::get_if<std::string>(&it->second))
    {
        try
        {
            return static_cast<uint64_t>(std::stoul(*p, nullptr, 0));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// Format a byte value as a "0xNN" lowercase hex string.
std::string toHexByte(uint64_t v)
{
    return std::format("0x{:02x}", static_cast<unsigned>(v & 0xff));
}

// Read the FPGA register-page table from EM: find the Configuration.Dump record
// advertising "FPGA", GetAll its sibling Pages<N> interfaces in index order
// into EmCpldPage. Returns empty (logged) when no FPGA record / Pages exist.
std::vector<EmCpldPage> readFpgaPagesFromEm()
{
    using namespace phosphor::dump::collectors;
    std::vector<EmCpldPage> pages;

    // 1. Find every Configuration.Dump record advertising "FPGA"; step 2 picks
    //    the one carrying Pages (others advertise FPGA only for
    //    AllowableValues).
    std::vector<std::pair<std::string, std::string>> fpgaCandidates;
    for (const auto& obj : enumerateEmObjects({DUMP_IFACE}))
    {
        auto supported = emStringVec(obj, "SupportedDumps");
        if (std::find(supported.begin(), supported.end(), "FPGA") !=
            supported.end())
        {
            fpgaCandidates.emplace_back(obj.path, obj.service);
        }
    }
    if (fpgaCandidates.empty())
    {
        lg2::info(
            "nv-collector-fpga: no Configuration.Dump record advertises FPGA");
        return pages;
    }

    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);

    // 2. For each candidate, find its sibling Pages<N> interfaces; use the
    //    first that has them (advertisement-only records are skipped).
    std::string fpgaPath;
    std::string fpgaService;
    std::map<unsigned long, std::string> pageIfaces;
    const std::string prefix = PAGES_IFACE_PREFIX;
    for (const auto& [candPath, candService] : fpgaCandidates)
    {
        std::map<std::string, std::vector<std::string>> svcIfaces;
        try
        {
            auto getObj = conn->new_method_call(
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetObject");
            getObj.append(candPath, std::vector<std::string>{});
            auto reply = conn->call(getObj);
            reply.read(svcIfaces);
        }
        catch (const std::exception& e)
        {
            lg2::warning(
                "nv-collector-fpga: GetObject on '{PATH}' failed: {ERR}",
                "PATH", candPath, "ERR", e.what());
            continue;
        }

        // Collect (index -> interface) so Pages are read in array order.
        std::map<unsigned long, std::string> candPages;
        for (const auto& [svc, ifaces] : svcIfaces)
        {
            for (const auto& iface : ifaces)
            {
                if (!iface.starts_with(prefix))
                {
                    continue;
                }
                try
                {
                    candPages.emplace(std::stoul(iface.substr(prefix.size())),
                                      iface);
                }
                catch (...)
                {
                    lg2::warning(
                        "nv-collector-fpga: unparsable Pages interface "
                        "'{IFACE}'",
                        "IFACE", iface);
                }
            }
        }
        if (!candPages.empty())
        {
            fpgaPath = candPath;
            fpgaService = candService;
            pageIfaces = std::move(candPages);
            break;
        }
    }
    if (pageIfaces.empty())
    {
        lg2::error(
            "nv-collector-fpga: no FPGA Configuration.Dump record carries "
            "Pages<N> interfaces (checked {N} candidate(s))",
            "N", fpgaCandidates.size());
        return pages;
    }

    // 3. GetAll each Pages<N> interface in index order; map scalar props to
    //    EmCpldPage (Idx / Bus / SlaveAddr + optional RegAddr / Page / Size).
    for (const auto& [idx, iface] : pageIfaces)
    {
        std::map<std::string, PageVariant> props;
        try
        {
            auto getAll = conn->new_method_call(
                fpgaService.c_str(), fpgaPath.c_str(),
                "org.freedesktop.DBus.Properties", "GetAll");
            getAll.append(iface);
            auto reply = conn->call(getAll);
            reply.read(props);
        }
        catch (const std::exception& e)
        {
            lg2::warning("nv-collector-fpga: GetAll '{IFACE}' failed: {ERR}",
                         "IFACE", iface, "ERR", e.what());
            continue;
        }

        EmCpldPage p;
        if (auto v = pageStr(props, "Idx"); v)
        {
            p.idx = *v;
        }
        if (auto v = pageUint(props, "Bus"); v)
        {
            p.bus = static_cast<uint8_t>(*v);
        }
        // SlaveAddr / RegAddr / Page arrive as uint64; normalise to "0xNN".
        // RegAddr / Page are optional: set only when the key is present.
        if (auto v = pageUint(props, "SlaveAddr"); v)
        {
            p.slaveAddr = toHexByte(*v);
        }
        if (auto v = pageUint(props, "RegAddr"); v)
        {
            p.regAddr = toHexByte(*v);
        }
        if (auto v = pageUint(props, "Page"); v)
        {
            p.page = toHexByte(*v);
        }
        if (auto v = pageUint(props, "Size"); v)
        {
            p.size = static_cast<uint32_t>(*v);
        }

        if (p.slaveAddr.empty())
        {
            lg2::warning(
                "nv-collector-fpga: Pages[{IDX}] missing SlaveAddr; skipping",
                "IDX", idx);
            continue;
        }
        pages.push_back(std::move(p));
    }
    return pages;
}

std::string buildPageName(
    const phosphor::dump::collectors::em_args::EmCpldPage& p)
{
    std::string name = p.idx + "_" + std::to_string(p.bus) + "_" + p.slaveAddr;
    if (p.regAddr)
    {
        name += "_" + *p.regAddr;
    }
    if (p.page)
    {
        name += "_" + *p.page;
    }
    name += ".bin";
    return name;
}

// Convert i2ctransfer's ASCII-hex stdout into a raw binary file at outPath.
// Returns false (writing nothing) on an i2ctransfer error or zero parsed bytes.
bool hexFileToBinary(const std::string& rawPath, const std::string& outPath)
{
    std::ifstream in(rawPath);
    if (!in)
    {
        lg2::warning("nv-collector-fpga: cannot read raw capture '{PATH}'",
                     "PATH", rawPath);
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    // i2ctransfer diagnostics land in the captured stream; treat as a failure.
    if (content.find("Error") != std::string::npos)
    {
        lg2::warning("nv-collector-fpga: i2ctransfer error for '{PATH}': {MSG}",
                     "PATH", outPath, "MSG", content);
        return false;
    }

    std::vector<unsigned char> bytes;
    std::istringstream iss(content);
    std::string tok;
    while (iss >> tok)
    {
        try
        {
            bytes.push_back(
                static_cast<unsigned char>(std::stoul(tok, nullptr, 16)));
        }
        catch (...)
        {
            // Skip non-hex tokens rather than abort the page.
            continue;
        }
    }
    if (bytes.empty())
    {
        lg2::warning("nv-collector-fpga: no hex bytes parsed for '{PATH}'",
                     "PATH", outPath);
        return false;
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        lg2::warning("nv-collector-fpga: cannot write binary '{PATH}'", "PATH",
                     outPath);
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// Run one i2ctransfer and return its combined stdout+stderr as text. The APB
// dump keeps the ASCII-hex output verbatim (not converted to raw binary).
std::string runCapture(const std::string& cmd, const std::string& tmpPath)
{
    phosphor::dump::collectors::runExternal(cmd + " > " + tmpPath + " 2>&1");
    std::ifstream in(tmpPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    in.close();
    std::remove(tmpPath.c_str());
    return out;
}

// APB controller register dump (port of fpga_dump.sh dump_apb_registers). Per
// module writes dump_apb_mod_<mod>.txt with the per-GPU apb_controller and
// gpu_controller reads as command lines plus ASCII-hex results. Returns true if
// at least one read produced output.
bool dumpApbRegisters(const std::string& stage)
{
    using namespace phosphor::dump::collectors;
    bool any = false;
    const std::string tmp = stage + "/.apb.rawhex";

    for (int mod = 0; mod < APB_NUM_MOD; ++mod)
    {
        int bus = APB_BUS[static_cast<size_t>(mod)];
        std::string txtPath =
            stage + "/dump_apb_mod_" + std::to_string(mod) + ".txt";
        std::ofstream txt(txtPath, std::ios::trunc);
        if (!txt)
        {
            lg2::warning("nv-collector-fpga: cannot open APB file '{PATH}'",
                         "PATH", txtPath);
            continue;
        }
        txt << "APB Register Dump - Module " << mod << "\n";
        txt << "Module: " << mod << ", Bus: " << bus
            << ", GPUs: " << APB_NUM_GPU << "\n\n";

        for (int gpu = 0; gpu < APB_NUM_GPU; ++gpu)
        {
            txt << "==== Module " << mod << ", GPU " << gpu
                << " apb_controller regs ====\n";
            for (const auto& [b0, b1] : apbControllerOffsets)
            {
                unsigned b1dec = 0;
                try
                {
                    b1dec = static_cast<unsigned>(std::stoul(b1, nullptr, 16));
                }
                catch (...)
                {
                    continue;
                }
                unsigned b1new = (b1dec + 0x8U * static_cast<unsigned>(gpu)) &
                                 0xFFU;
                std::string cmd =
                    std::format("{} -y {} w2@{} {} 0x{:02x} r{}", I2CTRANSFER,
                                bus, APB_I2C_ADDR, b0, b1new, APB_READ_SIZE);
                std::string result = runCapture(cmd, tmp);
                txt << cmd << "\n" << result;
                if (!result.empty() &&
                    result.find("Error") == std::string::npos)
                {
                    any = true;
                }
            }
        }

        txt << "==== Module " << mod << " gpu_controller regs ====\n";
        for (const auto& [b0, b1] : gpuControllerOffsets)
        {
            std::string cmd =
                std::format("{} -y {} w2@{} {} {} r{}", I2CTRANSFER, bus,
                            APB_I2C_ADDR, b0, b1, APB_READ_SIZE);
            std::string result = runCapture(cmd, tmp);
            txt << cmd << "\n" << result;
            if (!result.empty() && result.find("Error") == std::string::npos)
            {
                any = true;
            }
        }
        txt.close();
    }
    return any;
}
} // namespace

int main(int argc, char** argv)
{
    using namespace phosphor::dump::collectors;
    auto cli = parseCli(argc, argv);
    if (cli.outDir.empty() || cli.entryId.empty())
    {
        lg2::error("nv-collector-fpga: missing --out-dir/--entry-id");
        return EXIT_FAILURE;
    }
    if (!ensureOutDir(cli.outDir))
    {
        return EXIT_FAILURE;
    }

    // Read the register-page table from the EM Configuration.Dump Pages<N>
    // interfaces over D-Bus.
    std::vector<em_args::EmCpldPage> pages = readFpgaPagesFromEm();

    if (pages.empty())
    {
        // Not fatal: the hardcoded APB controller dump still runs. We fail only
        // if both the register-page reads and the APB dump produce nothing.
        lg2::info("nv-collector-fpga: no FPGA Pages in EM Configuration.Dump; "
                  "running APB controller dump only");
    }

    // Stage under /tmp, then tar into cli.outDir (see makeTarball).
    std::string stem = obmcdumpStem(cli.entryId);
    std::string stage = "/tmp/" + stem;
    if (!ensureOutDir(stage))
    {
        return EXIT_FAILURE;
    }

    bool anyOk = false;
    for (const auto& page : pages)
    {
        std::string outName = buildPageName(page);
        std::string outPath = std::format("{}/{}", stage, outName);
        std::string rawPath = outPath + ".rawhex";

        // i2ctransfer -y <Bus> w<WCNT>@<SlaveAddr> <RegAddr> <Page> r<Size>;
        // capture ASCII-hex stdout, then convert to raw binary via
        // hexFileToBinary.
        std::string wcnt = "1";
        std::string regBlob;
        if (page.regAddr)
        {
            regBlob += " " + *page.regAddr;
            if (page.page)
            {
                wcnt = "2";
                regBlob += " " + *page.page;
            }
        }
        std::string size = page.size ? std::to_string(*page.size) : "1";
        std::string cmd =
            std::format("{} -y {} w{}@{}{} r{} > {} 2>&1", I2CTRANSFER,
                        page.bus, wcnt, page.slaveAddr, regBlob, size, rawPath);
        if (runExternal(cmd) != 0)
        {
            lg2::warning(
                "nv-collector-fpga: i2ctransfer exited non-zero for '{NAME}'",
                "NAME", outName);
            std::remove(rawPath.c_str());
            continue;
        }
        if (hexFileToBinary(rawPath, outPath))
        {
            anyOk = true;
        }
        std::remove(rawPath.c_str());
    }

    // APB controller dump — hardcoded FPGA offsets, independent of EM Pages[].
    if (dumpApbRegisters(stage))
    {
        anyOk = true;
    }

    if (!anyOk)
    {
        // Every i2ctransfer failed — deliver no archive and fail immediately.
        cleanupStage(stage);
        return EXIT_FAILURE;
    }
    if (!makeTarball(cli.outDir, stage))
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
