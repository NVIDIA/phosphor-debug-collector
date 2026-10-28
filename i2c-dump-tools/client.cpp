// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// hmc-dump-client - has the HMC create a dump and copies it to the BMC over
// the I2C mailbox that the HMC's hmc-dump-server serves.
//
//   hmc-dump-client [dump type] [file]
//       dump type: 1 HMC dump (default), 2 FPGA register table, 3 ERoT
//       file:      where to write the dump, default dump.tar.xz
//   hmc-dump-client exec <opcode> [args] [timeout]
//       runs one dump-server command; args is one space-separated string of
//       up to 16 bytes (default "0x00"), timeout in seconds (default 30).
//       Prints the 16 result bytes, or exits with the HMC's return code.
//
// The two modes keep the interface of i2c_dump_util.sh and
// execute_command.sh. The I2C buses and addresses come from the board's
// mailboxConfig.

#include "conf_file.hpp"
#include "mailbox.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

using namespace phosphor::dump::hmc_mailbox;

struct I2cTarget
{
    unsigned bus;
    uint16_t address;
};

struct Mailbox
{
    I2cTarget control;
    I2cTarget data;
};

// Ends the run: stop the dump server on the HMC and exit with code 1.
struct Failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The 16 result bytes of a dump-server command, or why it failed: exit_code
// is 1 for a transport error, otherwise the HMC's nonzero return code.
struct CommandResult
{
    std::vector<uint8_t> data;
    std::string message;
    int exit_code = 0;

    explicit operator bool() const
    {
        return exit_code == 0;
    }
};

CommandResult commandError(std::string message, int exitCode = 1)
{
    return {{}, std::move(message), exitCode};
}

struct ExecRequest
{
    uint8_t opcode;
    std::vector<uint8_t> args;
    std::chrono::seconds timeout;
};

std::optional<uint16_t> parseNumber(std::string_view s)
{
    try
    {
        std::size_t used = 0;
        auto v = std::stoul(std::string(s), &used, 0);
        if (used != s.size() || v > 0xffff)
        {
            return std::nullopt;
        }
        return static_cast<uint16_t>(v);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// ControlBus, ControlAddress, DataBus and DataAddress; boards without the
// file have no HMC dump mailbox.
constexpr auto mailboxConfig = "/usr/share/hmc-dump/hmc-dump.conf";

std::optional<Mailbox> findMailbox()
{
    namespace conf = phosphor::dump::conf;
    std::ifstream f(mailboxConfig);
    if (!f)
    {
        return std::nullopt;
    }
    std::optional<unsigned> controlBus;
    std::optional<unsigned> dataBus;
    std::optional<unsigned> controlAddr;
    std::optional<unsigned> dataAddr;
    for (const auto& e : conf::read(f))
    {
        auto value = e.ok ? conf::parseUnsigned(e.value) : std::nullopt;
        if (e.ok && e.key == "ControlBus")
        {
            controlBus = value;
        }
        else if (e.ok && e.key == "DataBus")
        {
            dataBus = value;
        }
        else if (e.ok && e.key == "ControlAddress")
        {
            controlAddr = value;
        }
        else if (e.ok && e.key == "DataAddress")
        {
            dataAddr = value;
        }
        else
        {
            lg2::warning("{FILE} line {LINE} ignored", "FILE", mailboxConfig,
                         "LINE", e.line);
        }
    }
    if (controlBus && *controlBus <= 0xffff && dataBus && *dataBus <= 0xffff &&
        controlAddr && *controlAddr <= 0x7f && dataAddr && *dataAddr <= 0x7f)
    {
        return Mailbox{{*controlBus, static_cast<uint16_t>(*controlAddr)},
                       {*dataBus, static_cast<uint16_t>(*dataAddr)}};
    }
    std::cerr << std::format(
        "Ignoring {}: needs ControlBus, DataBus, ControlAddress and "
        "DataAddress (addresses 0x00-0x7f).\n",
        mailboxConfig);
    lg2::warning("hmc-dump-client: incomplete {FILE}", "FILE", mailboxConfig);
    return std::nullopt;
}

// One I2C_RDWR transaction, as i2ctransfer does: an optional write message
// followed by an optional read message.
bool transfer(const I2cTarget& target, std::span<const uint8_t> write,
              std::span<uint8_t> read)
{
    std::string dev = std::format("/dev/i2c-{}", target.bus);
    int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    // i2c_msg takes a non-const buffer even for a write.
    std::vector<uint8_t> out(write.begin(), write.end());
    std::array<i2c_msg, 2> msgs{};
    unsigned count = 0;
    if (!out.empty())
    {
        msgs[count++] = {target.address, 0, static_cast<uint16_t>(out.size()),
                         out.data()};
    }
    if (!read.empty())
    {
        msgs[count++] = {target.address, I2C_M_RD,
                         static_cast<uint16_t>(read.size()), read.data()};
    }
    i2c_rdwr_ioctl_data data{msgs.data(), count};
    int rc = ::ioctl(fd, I2C_RDWR, &data);
    ::close(fd);
    return rc == static_cast<int>(count);
}

// The time as date(1) prints it.
std::string stamp()
{
    std::array<char, 64> buf{};
    std::time_t now = std::time(nullptr);
    if (const std::tm* tm = std::localtime(&now))
    {
        std::strftime(buf.data(), buf.size(), "%a %b %e %H:%M:%S %Z %Y", tm);
    }
    return buf.data();
}

void logLine(std::string_view message)
{
    std::cout << stamp() << ' ' << message << '\n';
}

class Client
{
  public:
    explicit Client(const Mailbox& mailbox) : mailbox(mailbox) {}

    bool serverCommand(uint8_t command)
    {
        return transfer(mailbox.control, std::to_array<uint8_t>({0, command}),
                        {});
    }

    CommandResult execute(uint8_t opcode, std::span<const uint8_t> args,
                          std::chrono::seconds timeout = 30s)
    {
        if (args.size() > max_args)
        {
            return commandError(
                "More than 16 bytes given as an argument. Max is 16.");
        }
        // Zero the unused argument bytes: the server reads all of them.
        std::vector<uint8_t> request(2 + max_args, 0);
        request[0] = opcode_offset;
        request[1] = opcode;
        std::ranges::copy(args, request.begin() + 2);
        if (!transfer(mailbox.control, request, {}))
        {
            return commandError(
                std::format("Failed to send command {}.", opcode));
        }
        if (!transfer(mailbox.control,
                      std::to_array<uint8_t>({request_offset, 1}), {}))
        {
            return commandError("Failed to set the request flag.");
        }
        if (!waitClear(request_offset, 5s))
        {
            return commandError(
                "Waiting for the HMC to take the request more than 5 seconds.");
        }
        if (!waitClear(running_offset, timeout))
        {
            return commandError(std::format(
                "Waiting for the command to complete more than {} seconds.",
                timeout.count()));
        }
        std::array<uint8_t, 1 + max_args> response{};
        if (!transfer(mailbox.control,
                      std::to_array<uint8_t>({return_code_offset}), response))
        {
            return commandError("Failed to get status and response.");
        }
        if (response[0] != 0)
        {
            return commandError("Command status code != 0.", response[0]);
        }
        return {{response.begin() + 1, response.end()}, {}, 0};
    }

    // The data EEPROM takes a two-byte address.
    bool readData(uint16_t offset, std::span<uint8_t> out)
    {
        return transfer(
            mailbox.data,
            std::to_array<uint8_t>({static_cast<uint8_t>(offset >> 8),
                                    static_cast<uint8_t>(offset)}),
            out);
    }

  private:
    bool waitClear(uint8_t offset, std::chrono::seconds timeout)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout)
        {
            std::array<uint8_t, 1> value{0xff};
            if (transfer(mailbox.control, std::to_array<uint8_t>({offset}),
                         value) &&
                value[0] == 0)
            {
                return true;
            }
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }

    Mailbox mailbox;
};

uint32_t be32(std::span<const uint8_t> b)
{
    return (uint32_t{b[0]} << 24) | (uint32_t{b[1]} << 16) |
           (uint32_t{b[2]} << 8) | b[3];
}

uint32_t readBlockSize(Client& client)
{
    std::array<uint8_t, 4> size{};
    if (!client.readData(block_size_offset, size))
    {
        throw Failure("Failed to read block size.");
    }
    return be32(size);
}

int downloadDump(Client& client, unsigned dumpTypeArg, const std::string& file)
{
    // 1 is the HMC (bmc) dump; 2 and 3 are system dumps with sub type 1 and
    // 2, which the HMC maps to its FPGA and ERoT diagnostics.
    const uint8_t dumpType = dumpTypeArg == 1 ? 1 : 2;
    const uint8_t dumpSubType = dumpTypeArg == 1 ? 1 : dumpTypeArg - 1;

    logLine("Turn on i2c dump server on HMC...");
    if (!client.serverCommand(start_dump_server))
    {
        throw Failure("Cannot start i2c dump server on HMC.");
    }
    std::this_thread::sleep_for(3s); // let the server start
    std::cout << '\n';

    logLine("Init dump creation...");
    auto created =
        client.execute(create_dump, std::to_array({dumpType, dumpSubType}));
    if (!created)
    {
        throw Failure(created.exit_code == dump_busy
                          ? "Another dump creation already in progress. "
                            "Try again in couple of minutes."
                          : created.message);
    }
    const uint8_t idHi = created.data[0];
    const uint8_t idLo = created.data[1];
    const unsigned entryId = (idHi << 8) | idLo;
    logLine(
        std::format("Dump creation initiated. Entry id: {} (0x{:02x} 0x{:02x})",
                    entryId, idHi, idLo));

    uint32_t dumpSize = 0;
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        if (std::chrono::steady_clock::now() - start >= 30min)
        {
            throw Failure("Waiting for the dump generation more than 30 min.");
        }
        auto progress = client.execute(dump_progress,
                                       std::to_array({dumpType, idHi, idLo}));
        if (!progress)
        {
            std::cout << "ERROR: " << progress.message << '\n';
        }
        else if (progress.data[0] == 0x00)
        {
            std::cout << std::format("\r{} Dump({}) creation is in progress.. "
                                     "Percentage: {}%",
                                     stamp(), entryId, progress.data[1])
                      << std::flush;
        }
        else if (progress.data[0] == 0x01)
        {
            dumpSize = be32(std::span(progress.data).subspan(2, 4));
            break;
        }
        else if (progress.data[0] == 0x02)
        {
            std::cout << '\n';
            throw Failure(std::format(
                "Dump({}) creation failed. Task ended with not Completed state.",
                entryId));
        }
        std::this_thread::sleep_for(5s);
    }
    std::cout << '\n';
    logLine(std::format(
        "Dump({}) creation completed. Dump size: {}. Creation time: {} seconds",
        entryId, dumpSize,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start)
            .count()));
    if (dumpSize == 0)
    {
        throw Failure("Generated dump is zero bytes.");
    }
    // The block number the server takes is 16 bits.
    if (dumpSize > 0x10000 * max_block_size)
    {
        throw Failure(std::format("Dump size {} is too large.", dumpSize));
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::ofstream out(file, std::ios::binary);
    if (!out)
    {
        throw Failure(std::format("Cannot create {}.", file));
    }

    uint32_t left = dumpSize;
    unsigned block = 0;
    start = std::chrono::steady_clock::now();
    while (left > 0)
    {
        logLine(std::format("Request for block number {}.", block));
        auto staged = client.execute(
            dump_block,
            std::to_array<uint8_t>(
                {dumpType, idHi, idLo, static_cast<uint8_t>(block >> 8),
                 static_cast<uint8_t>(block)}));
        if (!staged)
        {
            throw Failure(std::format("Request file block data failed. Msg: {}",
                                      staged.message));
        }

        uint32_t size = readBlockSize(client);
        constexpr int maxRetries = 3;
        for (int retry = 0; size == 0 && retry < maxRetries; ++retry)
        {
            std::cout << std::format(
                "WARNING: Block size is 0. Retrying ({}/{})...\n", retry + 1,
                maxRetries);
            std::this_thread::sleep_for(2s);
            size = readBlockSize(client);
        }
        if (size == 0)
        {
            throw Failure(std::format(
                "Block size is 0 after {} retries. Aborting dump transfer.",
                maxRetries));
        }
        if (size > max_block_size)
        {
            throw Failure(
                std::format("Block size {} exceeds {}.", size, max_block_size));
        }
        std::cout << std::format("Block {} size: {}. left_bytes_to_copy: {}\n",
                                 block, size, left);

        std::vector<uint8_t> data(size);
        if (!client.readData(block_data_offset, data))
        {
            throw Failure("Failed to read block data.");
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out)
        {
            throw Failure(std::format("Cannot write {}.", file));
        }
        left = size >= left ? 0 : left - size;
        ++block;
    }

    logLine(std::format(
        "Dump({}) download completed. Dump size: {}. Download time: {} seconds",
        file, dumpSize,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start)
            .count()));
    logLine("Turn off i2c dump server on HMC...");
    if (!client.serverCommand(stop_dump_server))
    {
        throw Failure("Cannot stop i2c dump server on HMC.");
    }
    return 0;
}

// execute_command.sh <opcode> [args] [timeout]. Checked before the mailbox
// is looked up, as the script did.
std::optional<ExecRequest> parseExec(int argc, char** argv)
{
    if (argc < 3 || argv[2][0] == '\0')
    {
        std::cout << "Opcode not given. Please provide opcode.\n";
        return std::nullopt;
    }
    auto opcode = parseNumber(argv[2]);
    const std::string argText =
        argc > 3 && argv[3][0] != '\0' ? argv[3] : "0x00";
    std::vector<uint8_t> args;
    std::istringstream tokens(argText);
    bool valid = opcode && *opcode <= 0xff;
    for (std::string tok; tokens >> tok;)
    {
        auto v = parseNumber(tok);
        valid = valid && v && *v <= 0xff;
        args.push_back(v ? static_cast<uint8_t>(*v) : 0);
    }
    if (args.size() > max_args)
    {
        std::cout << "More than 16 bytes given as an argument. Max is 16.\n";
        return std::nullopt;
    }
    if (!valid || !opcode)
    {
        std::cout << std::format("Invalid opcode or argument: \"{} {}\"\n",
                                 argv[2], argText);
        return std::nullopt;
    }
    std::chrono::seconds timeout = 30s;
    if (argc > 4 && argv[4][0] != '\0')
    {
        unsigned long secs = 0;
        std::string_view t = argv[4];
        auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), secs);
        if (ec != std::errc{} || ptr != t.data() + t.size() || secs > 86400)
        {
            std::cout << std::format("Invalid timeout: {}\n", t);
            return std::nullopt;
        }
        timeout = std::chrono::seconds{secs};
    }
    return ExecRequest{static_cast<uint8_t>(*opcode), std::move(args), timeout};
}

int execMode(Client& client, const ExecRequest& request)
{
    auto result = client.execute(request.opcode, request.args, request.timeout);
    if (!result)
    {
        std::cout << result.message << '\n';
        return result.exit_code;
    }
    std::string line;
    for (auto b : result.data)
    {
        line += std::format("{}0x{:02x}", line.empty() ? "" : " ", b);
    }
    std::cout << line << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    // Progress goes to a terminal or a test's pipe; never hold it back.
    std::cout << std::unitbuf;

    std::optional<ExecRequest> request;
    if (argc > 1 && std::string_view(argv[1]) == "exec")
    {
        request = parseExec(argc, argv);
        if (!request)
        {
            return 1;
        }
    }

    std::optional<Mailbox> mailbox;
    try
    {
        mailbox = findMailbox();
    }
    catch (const std::exception& e)
    {
        std::cout << "ERROR: " << e.what() << '\n';
        return 1;
    }
    if (!mailbox)
    {
        std::cout << "This platform has no HMC dump mailbox.\n";
        return 1;
    }
    Client client(*mailbox);

    if (request)
    {
        return execMode(client, *request);
    }

    // Empty arguments take the defaults, as ${1:-1} did.
    const std::string_view typeArg =
        argc > 1 && argv[1][0] != '\0' ? argv[1] : "1";
    const std::string file =
        argc > 2 && argv[2][0] != '\0' ? argv[2] : "dump.tar.xz";
    try
    {
        auto dumpType = parseNumber(typeArg);
        if (!dumpType || *dumpType < 1 || *dumpType > 3)
        {
            throw Failure(std::format("Unknown dump type: {}", typeArg));
        }
        return downloadDump(client, *dumpType, file);
    }
    catch (const std::exception& e)
    {
        std::cout << "ERROR: " << e.what() << '\n';
        client.serverCommand(stop_dump_server);
        return 1;
    }
}
