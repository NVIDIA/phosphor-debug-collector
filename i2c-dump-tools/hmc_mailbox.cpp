// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// hmc-mailbox - runs on the HMC: publishes the HMC's state to the BMC in
// telemetry EEPROM bytes 0-19 and carries out the command the BMC writes to
// control EEPROM byte 0 (then 0xff): 0x01 soft reboot, 0x02 network reset,
// 0x03/0x04 remote logging on/off, 0x05/0x06 start/stop hmc-dump-server,
// 0x07 USB hub reset, 0x0e dataflash erase, 0x0f factory reset, 0xd0
// signature checking. Also checks the filesystems and logs resource events.
//
// Telemetry bytes, every 5 s: 0: 0, boot complete; 1-5: firmware git hash,
// byte 1 one nibble; 6: usb0 online; 7-10: usb0 IPv4; 11: services; 12: rwfs
// use %; 13: USB hardware; 14: filesystems, 0x80 | 0x01 rwfs missing | 0x02
// log missing; 15: more services; 16-19: uptime, big-endian, every 26th
// pass.
//
// The EEPROMs, the rwfs medium, the USB hub reset line and MCTP in the kernel
// are detected; hmc_mailbox_config.hpp's file covers the rest.

#include "hmc_mailbox_config.hpp"
#include "mailbox.hpp"

#include <fcntl.h>
#include <libuboot.h>
#include <mtd/mtd-user.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

using namespace phosphor::dump;

namespace
{

namespace fs = std::filesystem;

constexpr auto rwfsMount = "/run/initramfs/rw";
constexpr auto logMount = "/var/lib/logging";
constexpr auto signatureFile = "/var/check_signature";
constexpr auto systemd = "org.freedesktop.systemd1";
constexpr auto systemdPath = "/org/freedesktop/systemd1";
constexpr auto manager = "org.freedesktop.systemd1.Manager";
constexpr auto properties = "org.freedesktop.DBus.Properties";
constexpr auto corrected = "ResourceEvent.1.0.ResourceErrorsCorrected";
constexpr auto detected = "ResourceEvent.1.0.ResourceErrorsDetected";
constexpr int maxRecoveryAttempts = 3;

constexpr auto usbHubResetGpio = "USB_HUB6_RESET_L-O";
constexpr int loopDelay = 5;

hmc_mailbox::Config config;
// How rwfs is recovered: MtdErase erases /dev/mtd6, Initramfs leaves it to
// the initramfs (no health checks), None erases nothing.
std::string rwfs;
std::string telemetry;
std::string control;
std::array<uint8_t, 16> bytes{}; // telemetry bytes 0-15
bool mctpKernel = false;
bool mctpUsbKernel = false;
bool mctpSpiKernel = false;

void sleepS(int s)
{
    std::this_thread::sleep_for(std::chrono::seconds{s});
}

void eepromWrite(const std::string& path, std::size_t offset,
                 std::span<const uint8_t> data)
{
    const std::string out(data.begin(), data.end());
    std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    f.flush();
    if (!f)
    {
        lg2::error("cannot write {COUNT} bytes to {PATH} at {OFFSET}", "COUNT",
                   data.size(), "PATH", path, "OFFSET", offset);
    }
}

std::string readFile(const fs::path& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The mount point of the first mount whose device is dev (or contains it
// when !exact), or nothing.
std::optional<std::string> mountOf(std::string_view dev, bool exact)
{
    std::ifstream f("/proc/mounts");
    for (std::string line; std::getline(f, line);)
    {
        std::istringstream fields(line);
        std::string d;
        std::string m;
        if (fields >> d >> m && (exact ? d == dev : d.contains(dev)))
        {
            return m;
        }
    }
    return std::nullopt;
}

bool mountpoint(std::string_view dir)
{
    std::ifstream f("/proc/mounts");
    for (std::string line; std::getline(f, line);)
    {
        std::istringstream fields(line);
        std::string d;
        std::string m;
        if (fields >> d >> m && m == dir)
        {
            return true;
        }
    }
    return false;
}

// df's Use%.
std::optional<int> usePercent(const std::string& dir)
{
    std::error_code ec;
    const auto space = fs::space(dir, ec);
    const uint64_t used = space.capacity - space.free;
    const uint64_t total = used + space.available;
    if (ec || space.capacity == 0)
    {
        return std::nullopt;
    }
    return total == 0 ? 0 : static_cast<int>((used * 100 + total / 2) / total);
}

void umountDevice(std::string_view dev)
{
    if (auto dir = mountOf(dev, true); dir && umount(dir->c_str()) != 0)
    {
        lg2::error("umount {DEVICE} failed: {ERROR}", "DEVICE", dev, "ERROR",
                   std::strerror(errno));
    }
}

// flash_eraseall: erase every good block.
void flashEraseall(const char* dev)
{
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    const int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        lg2::error("cannot open {DEVICE}", "DEVICE", dev);
        return;
    }
    mtd_info_t info{};
    if (ioctl(fd, MEMGETINFO, &info) == 0 && info.erasesize != 0)
    {
        for (uint32_t start = 0; start < info.size; start += info.erasesize)
        {
            loff_t offset = start;
            erase_info_t erase{.start = start, .length = info.erasesize};
            if (ioctl(fd, MEMGETBADBLOCK, &offset) <= 0 &&
                ioctl(fd, MEMERASE, &erase) != 0)
            {
                lg2::error("{DEVICE}: erase at {OFFSET} failed", "DEVICE", dev,
                           "OFFSET", std::format("{:#x}", start));
            }
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    close(fd);
}

// The u-boot environment, as fw_printenv and fw_setenv open it.
class UbootEnv
{
  public:
    UbootEnv()
    {
        if (libuboot_initialize(&ctx, nullptr) < 0)
        {
            ctx = nullptr;
            return;
        }
        ok = libuboot_read_config(ctx, "/etc/fw_env.config") == 0 &&
             libuboot_open(ctx) == 0;
    }
    ~UbootEnv()
    {
        if (ctx != nullptr)
        {
            libuboot_close(ctx);
            libuboot_exit(ctx);
        }
    }
    UbootEnv(const UbootEnv&) = delete;
    UbootEnv& operator=(const UbootEnv&) = delete;
    UbootEnv(UbootEnv&&) = delete;
    UbootEnv& operator=(UbootEnv&&) = delete;

    // Nothing when the environment cannot be read; def when key is unset.
    std::optional<std::string> get(const char* key, const char* def)
    {
        char* value = ok ? libuboot_get_env(ctx, key) : nullptr;
        if (value == nullptr)
        {
            return ok ? std::optional<std::string>(def) : std::nullopt;
        }
        std::string out(value);
        std::free(value); // NOLINT(cppcoreguidelines-no-malloc)
        return out;
    }

    // An empty value deletes the variable.
    static void set(const std::string& key, const std::string& value = {})
    {
        UbootEnv env;
        if (!env.ok ||
            libuboot_set_env(env.ctx, key.c_str(),
                             value.empty() ? nullptr : value.c_str()) != 0 ||
            libuboot_env_store(env.ctx) != 0)
        {
            lg2::error("fw_setenv {KEY} failed", "KEY", key);
        }
    }

  private:
    struct uboot_ctx* ctx = nullptr;
    bool ok = false;
};

// A method call; on failure logs "<failed>: <error>" unless failed is empty.
template <typename... Args>
std::optional<sdbusplus::message_t> call(
    sdbusplus::bus_t& bus, const std::string& failed, const char* service,
    const char* path, const char* iface, const char* method,
    const Args&... args)
{
    try
    {
        auto m = bus.new_method_call(service, path, iface, method);
        (m.append(args), ...);
        return bus.call(m);
    }
    catch (const std::exception& e)
    {
        if (!failed.empty())
        {
            lg2::error("{FAILED}: {ERROR}", "FAILED", failed, "ERROR",
                       e.what());
        }
        return std::nullopt;
    }
}

// systemctl start/stop: the job, then its end.
void systemctl(sdbusplus::bus_t& bus, const char* method,
               const std::string& unit)
{
    auto reply = call(bus, std::format("{} {} failed", method, unit), systemd,
                      systemdPath, manager, method, unit, "replace");
    sdbusplus::object_path job;
    if (reply)
    {
        reply->read(job);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes{5};
    while (reply && std::chrono::steady_clock::now() < deadline &&
           call(bus, "", systemd, job.str.c_str(), properties, "Get",
                "org.freedesktop.systemd1.Job", "State"))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
}

void reboot(sdbusplus::bus_t& bus, bool force)
{
    if (force)
    {
        call(bus, "reboot failed", systemd, systemdPath, manager, "Reboot");
    }
    else
    {
        call(bus, "reboot failed", systemd, systemdPath, manager, "StartUnit",
             "reboot.target", "replace-irreversibly");
    }
}

// A Redfish event; critical when it has a resolution.
bool event(sdbusplus::bus_t& bus, const char* id, const std::string& args,
           const std::string& resolution = {}, const char* message = nullptr)
{
    std::map<std::string, std::string> additional{
        {"REDFISH_MESSAGE_ID", id}, {"REDFISH_MESSAGE_ARGS", args}};
    if (!resolution.empty())
    {
        additional["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    }
    message = message == nullptr ? id : message;
    return call(bus, std::format("Failed to create log {}", message),
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "xyz.openbmc_project.Logging.Create", "Create", message,
                resolution.empty()
                    ? "xyz.openbmc_project.Logging.Entry.Level.Informational"
                    : "xyz.openbmc_project.Logging.Entry.Level.Critical",
                additional)
        .has_value();
}

void rebootWithReason(sdbusplus::bus_t& bus, const std::string& reason)
{
    event(bus, "OpenBMC.0.4.BMCRebootReason", reason);
    sleepS(2); // let the log reach flash
    reboot(bus, config.forceReboot);
}

template <typename T>
void syslogSet(sdbusplus::bus_t& bus, const char* iface, const char* prop,
               const T& value)
{
    call(bus, std::format("Failed to set {}", prop),
         "xyz.openbmc_project.Syslog.Config",
         "/xyz/openbmc_project/logging/config/remote", properties, "Set", iface,
         prop, std::variant<T>(value));
}

template <typename T>
std::optional<T> getProperty(sdbusplus::bus_t& bus, const char* service,
                             const std::string& path, const char* iface,
                             const char* prop)
{
    std::variant<T> v;
    auto reply =
        call(bus, "", service, path.c_str(), properties, "Get", iface, prop);
    try
    {
        if (reply)
        {
            reply->read(v);
            return std::get<T>(v);
        }
    }
    catch (const std::exception&)
    {
        return std::nullopt; // not readable as T: as if not set
    }
    return std::nullopt;
}

// 1 << bit when the unit's service is running.
uint8_t running(sdbusplus::bus_t& bus, const std::string& unit, int bit)
{
    sdbusplus::object_path path;
    auto reply = call(bus, "", systemd, systemdPath, manager, "GetUnit",
                      unit + ".service");
    try
    {
        if (!reply)
        {
            return 0;
        }
        reply->read(path);
    }
    catch (const std::exception&)
    {
        return 0;
    }
    return getProperty<std::string>(bus, systemd, path.str,
                                    "org.freedesktop.systemd1.Unit",
                                    "SubState") == "running"
               ? 1 << bit
               : 0;
}

void readKernelConfig()
{
    gzFile f = gzopen("/proc/config.gz", "rb");
    if (f == nullptr)
    {
        return;
    }
    std::array<char, 512> line{};
    while (gzgets(f, line.data(), line.size()) != nullptr)
    {
        const std::string_view l(line.data());
        mctpKernel |= l.contains("CONFIG_MCTP=y");
        mctpUsbKernel |= l.contains("CONFIG_MCTP_TRANSPORT_USB=y");
        mctpSpiKernel |= l.contains("CONFIG_MCTP_TRANSPORT_SPI=y");
    }
    gzclose(f);
}

// Bytes 1-5: the firmware git hash from VERSION_ID
// core-XX.XX-rcx-xx-gHASH.xxxxxx.xxxxxx.
void readVersion()
{
    std::ifstream f("/etc/os-release");
    if (!f)
    {
        lg2::warning("update_swversion: /etc/os-release missing or "
                     "unreadable; using zero version bytes");
        return;
    }
    std::string version;
    for (std::string line; std::getline(f, line);)
    {
        if (line.starts_with("VERSION_ID="))
        {
            version = line.substr(line.find('=') + 1);
            break;
        }
    }
    std::erase(version, '"');
    if (auto pos = version.find("-dirty"); pos != std::string::npos)
    {
        version.erase(pos, 6);
    }
    const auto dash = version.rfind('-');
    const auto last = dash == std::string::npos ? "" : version.substr(dash + 1);
    const auto hash = last.substr(0, last.find('.'));
    if (hash.size() < 9 || !hash.starts_with('g') ||
        hash.find_first_not_of("0123456789abcdefABCDEF", 1) !=
            std::string::npos)
    {
        lg2::warning("update_swversion: VERSION_ID '{VERSION}' has no valid "
                     "-g<hash> suffix; using zero version bytes",
                     "VERSION", version);
        return;
    }
    bytes[1] = static_cast<uint8_t>(std::stoul(hash.substr(1, 1), nullptr, 16));
    for (std::size_t i = 1; i < 5; ++i)
    {
        bytes[i + 1] = static_cast<uint8_t>(
            std::stoul(hash.substr(i * 2, 2), nullptr, 16));
    }
}

// Bytes 6-10 from phosphor-networkd: usb0 online (link up with an IPv4
// address) and its first IPv4 address.
void updateUsb0(sdbusplus::bus_t& bus)
{
    using Properties = std::map<std::string, std::variant<bool, std::string>>;
    using Objects =
        std::map<sdbusplus::object_path, std::map<std::string, Properties>>;
    constexpr std::string_view usb0 = "/xyz/openbmc_project/network/usb0";
    Objects objects;
    auto reply = call(
        bus, "", "xyz.openbmc_project.Network", "/xyz/openbmc_project/network",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    try
    {
        if (reply)
        {
            reply->read(objects);
        }
    }
    catch (const std::exception&)
    {
        objects.clear();
    }
    bool linkUp = false;
    std::vector<std::string> addrs;
    for (const auto& [path, ifaces] : objects)
    {
        if (path.str == usb0)
        {
            auto it =
                ifaces.find("xyz.openbmc_project.Network.EthernetInterface");
            if (it != ifaces.end())
            {
                auto p = it->second.find("LinkUp");
                linkUp = p != it->second.end() &&
                         std::holds_alternative<bool>(p->second) &&
                         std::get<bool>(p->second);
            }
            continue;
        }
        auto ip = ifaces.find("xyz.openbmc_project.Network.IP");
        if (!path.str.starts_with(std::string(usb0) + "/") ||
            ip == ifaces.end())
        {
            continue;
        }
        auto type = ip->second.find("Type");
        auto address = ip->second.find("Address");
        if (type != ip->second.end() && address != ip->second.end() &&
            std::holds_alternative<std::string>(type->second) &&
            std::holds_alternative<std::string>(address->second) &&
            std::get<std::string>(type->second) ==
                "xyz.openbmc_project.Network.IP.Protocol.IPv4")
        {
            addrs.push_back(std::get<std::string>(address->second));
        }
    }
    bytes[6] = linkUp && !addrs.empty() ? 1 : 0;
    std::ranges::copy(hmc_mailbox::firstIpv4(addrs), bytes.begin() + 7);
}

// Bytes 11 and 15.
void updateServices(sdbusplus::bus_t& bus)
{
    const bool fixed = config.fixedMctpStatus;
    uint8_t status = running(bus, "bmcweb", 0);
    status |= running(bus, "pldmd", 1);
    status |= running(bus, "nsmd", 2);
    if (!fixed)
    {
        status |= running(bus, mctpKernel ? "mctpd" : "mctp-usb-ctrl", 3);
        status |= running(
            bus,
            mctpKernel ? "xyz.openbmc_project.mctpreactor" : "mctp-spi0-ctrl",
            4);
    }
    constexpr auto syslog = "xyz.openbmc_project.Syslog.Config";
    constexpr auto remote = "/xyz/openbmc_project/logging/config/remote";
    constexpr auto client = "xyz.openbmc_project.Network.Client";
    const auto address =
        getProperty<std::string>(bus, syslog, remote, client, "Address");
    const auto port =
        getProperty<uint16_t>(bus, syslog, remote, client, "Port");
    if (address == config.bmcSyslogAddress && port == 6514)
    {
        status |= 0x80;
    }
    // Bit 6, remote logging supported, and the fixed MCTP bits only go out
    // with another bit, as the scripts had it.
    bytes[11] = status == 0 ? 0 : status | 0x40 | (fixed ? 0x18 : 0);

    status = running(bus, "xyz.openbmc_project.Dump.Manager", 0);
    if (!fixed && !mctpUsbKernel)
    {
        status |= running(bus, "mctp-usb-demux", 1);
    }
    if (!fixed && !mctpSpiKernel)
    {
        status |= running(bus, "mctp-spi0-demux", 2);
    }
    bytes[15] = status == 0 ? 0 : status | (fixed ? 0x06 : 0);
}

// Bytes 12-14; rwfsFull for the recovery, kept while rwfs is not mounted.
void updateDiagnostics(bool& rwfsFull, int& mountDelay)
{
    int usage = 0;
    if (mountpoint(rwfsMount))
    {
        usage = usePercent(rwfsMount).value_or(0);
        if (rwfs != "Initramfs")
        {
            rwfsFull = usage >= 99;
        }
    }
    bytes[12] = static_cast<uint8_t>(usage);

    std::string state;
    std::string uevent;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/udc", ec))
    {
        state += readFile(e.path() / "state");
        uevent += readFile(e.path() / "uevent");
    }
    uint8_t usb = state.contains("configured") ? 1 : 0;
    std::istringstream lines(uevent);
    for (std::string line; std::getline(lines, line);)
    {
        if (line.contains("USB_UDC_DRIVER") && line.contains("g_ether"))
        {
            usb |= 2;
        }
    }
    if (readFile("/sys/class/net/usb0/operstate").contains("up"))
    {
        usb |= 4;
    }
    bytes[13] = usb;

    // Where the initramfs recovers rwfs there is no health check: the mounts,
    // as late as the health check would be, so mounts still coming up do not
    // read as missing.
    if (rwfs != "Initramfs")
    {
        return;
    }
    if (mountDelay <= 600)
    {
        mountDelay += loopDelay;
        bytes[14] = 0;
    }
    else
    {
        bytes[14] = (mountpoint(rwfsMount) ? 0 : 0x81) |
                    (mountpoint(logMount) ? 0 : 0x82);
    }
}

// Bytes 16-19.
void updateUptime()
{
    double seconds = 0;
    std::ifstream("/proc/uptime") >> seconds;
    const auto u = static_cast<uint32_t>(seconds);
    eepromWrite(
        telemetry, 16,
        std::to_array<uint8_t>(
            {static_cast<uint8_t>(u >> 24), static_cast<uint8_t>(u >> 16),
             static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u)}));
}

void runCommand(sdbusplus::bus_t& bus)
{
    constexpr auto rsyslog = "xyz.openbmc_project.Logging.RsyslogClient";
    constexpr auto client = "xyz.openbmc_project.Network.Client";
    std::ifstream f{control, std::ios::binary};
    char c = 0;
    const uint8_t opcode = f.get(c) ? static_cast<uint8_t>(c) : 0xff;
    switch (opcode)
    {
        case 0x01:
            lg2::info("Softreboot requested");
            rebootWithReason(bus, config.softRebootReason);
            break;
        case 0x02:
        {
            lg2::info("Network reset requested");
            systemctl(bus, "StopUnit", "systemd-networkd.service");
            // Back to the read-only image's network configuration.
            const fs::path ro = "/run/initramfs/ro/etc/systemd/network/";
            std::error_code ec;
            fs::create_directories("/etc/systemd/network/", ec);
            for (const auto& e : fs::directory_iterator(
                     "/run/initramfs/rw/cow/etc/systemd/network/", ec))
            {
                const auto name = e.path().filename();
                if (name.string().starts_with('.'))
                {
                    continue;
                }
                if (fs::exists(ro / name))
                {
                    fs::copy_file(ro / name,
                                  fs::path("/etc/systemd/network") / name,
                                  fs::copy_options::overwrite_existing, ec);
                    lg2::info("Copied {FILE}", "FILE", name.string());
                }
                else
                {
                    lg2::info("No corresponding file found for {FILE}", "FILE",
                              name.string());
                }
            }
            systemctl(bus, "StartUnit", "systemd-networkd.service");
            break;
        }
        case 0x03:
        case 0x04:
            lg2::info("Request to turn remote logging {STATE}", "STATE",
                      opcode == 0x03 ? "ON" : "OFF");
            syslogSet(bus, rsyslog, "Enabled", opcode == 0x03);
            if (opcode == 0x03)
            {
                syslogSet(bus, rsyslog, "Severity",
                          std::string("xyz.openbmc_project.Logging."
                                      "RsyslogClient.SeverityType.All"));
            }
            syslogSet(bus, client, "Port",
                      static_cast<uint16_t>(opcode == 0x03 ? 6514 : 0));
            syslogSet(bus, client, "Address",
                      opcode == 0x03 ? config.bmcSyslogAddress : std::string());
            break;
        case hmc_mailbox::start_dump_server:
        case hmc_mailbox::stop_dump_server:
            lg2::info(
                "{ACTION} of dump server requested", "ACTION",
                opcode == hmc_mailbox::start_dump_server ? "Start" : "Stop");
            systemctl(bus,
                      opcode == hmc_mailbox::start_dump_server ? "StartUnit"
                                                               : "StopUnit",
                      "hmc-dump-server.service");
            break;
        case 0x07:
        {
            // Only boards with the line take the command.
            gpiod::line line;
            try
            {
                line = gpiod::find_line(usbHubResetGpio);
            }
            catch (const std::exception& e)
            {
                lg2::debug("cannot look up {GPIO}: {ERROR}", "GPIO",
                           usbHubResetGpio, "ERROR", e.what());
            }
            if (!line)
            {
                break;
            }
            lg2::info("USB hub reset requested");
            // gpioset <line>=0, two seconds, gpioset <line>=1.
            try
            {
                const gpiod::line_request out{
                    "hmc-mailbox", gpiod::line_request::DIRECTION_OUTPUT, 0};
                line.request(out, 0);
                line.release();
                sleepS(2);
                line.request(out, 1);
                line.release();
            }
            catch (const std::exception& e)
            {
                lg2::error("cannot reset the USB hub through {GPIO}: {ERROR}",
                           "GPIO", usbHubResetGpio, "ERROR", e.what());
            }
            break;
        }
        case 0x0e:
            lg2::info("Dataflash erase requested");
            // The log partition's users; they come back after the reboot.
            systemctl(bus, "StopUnit", "xyz.openbmc_project.Logging.service");
            systemctl(bus, "StopUnit",
                      "xyz.openbmc_project.Dump.Manager.service");
            if (rwfs == "Initramfs")
            {
                UbootEnv::set("rwfs_recovery", "1"); // the initramfs formats
            }
            else
            {
                if (rwfs == "MtdErase" && mountpoint(rwfsMount))
                {
                    umountDevice("/dev/mtdblock6");
                }
                if (mountpoint(logMount))
                {
                    umountDevice("/dev/mtdblock7");
                }
                if (rwfs == "MtdErase")
                {
                    flashEraseall("/dev/mtd6");
                }
            }
            UbootEnv::set("openbmclog", "dataflash-erase");
            UbootEnv::set("emmc_secure_erase_partition", "/var/emmc/user-logs");
            sleepS(5); // let the u-boot environment reach flash
            reboot(bus, config.forceReboot);
            break;
        case 0x0f:
            lg2::info("Factory reset requested");
            call(bus, "Factory reset failed",
                 "xyz.openbmc_project.Software.BMC.Inventory",
                 "/xyz/openbmc_project/software/bmc",
                 "xyz.openbmc_project.Common.FactoryReset", "Reset");
            rebootWithReason(
                bus, "Factory Reset triggered by external command over I2C");
            break;
        case 0xd0:
            if (config.signatureCheck)
            {
                lg2::info("Signature checking requested");
                std::ofstream{signatureFile, std::ios::app};
            }
            break;
        default:
            break;
    }
    eepromWrite(control, 0, std::to_array<uint8_t>({0xff}));
}

// Every 26th pass: the HMC not ready, once per outage, and the log
// partition passing 90%, once until it drops to 85%.
void runEventCheck(sdbusplus::bus_t& bus, bool& checkEvents, int& logUsage,
                   bool& logUsageArmed)
{
    std::stringstream ready;
    ready
        << std::ifstream("/sys/class/leds/hmc_ready_noled/brightness").rdbuf();
    if (!ready.str().contains('1') && checkEvents)
    {
        constexpr auto restart = "If problem persists, try restarting HMC";
        event(bus, detected, "HMC Ready,Service Unavailable", restart);
        std::string oper;
        std::string online;
        std::ifstream ifindex("/sys/class/net/usb0/ifindex");
        std::string index;
        std::getline(ifindex, index);
        std::ifstream links(std::format("/run/systemd/netif/links/{}", index));
        for (std::string line; !index.empty() && std::getline(links, line);)
        {
            if (oper.empty() && line.starts_with("OPER_STATE="))
            {
                oper = line.substr(11);
            }
            if (online.empty() && line.starts_with("ONLINE_STATE="))
            {
                online = line.substr(13);
            }
        }
        if (oper != "routable")
        {
            event(bus, detected, "HMC USB Connection,Service Unavailable",
                  restart);
        }
        if (online != "online")
        {
            event(bus, detected, "HMC USB Network,Service Unavailable",
                  restart);
        }
        sdbusplus::object_path path;
        std::variant<std::string> active;
        if (auto r = call(bus, "", systemd, systemdPath, manager, "GetUnit",
                          std::string("bmcweb.service")))
        {
            r->read(path);
            if (auto g = call(bus, "", systemd, path.str.c_str(), properties,
                              "Get", "org.freedesktop.systemd1.Unit",
                              "ActiveState"))
            {
                g->read(active);
            }
        }
        if (std::get<std::string>(active) == "failed")
        {
            event(bus, detected, "HMC Redfish,Service Unavailable", restart);
        }
        checkEvents = false;
    }
    else
    {
        checkEvents = true;
    }

    if (rwfs == "Initramfs")
    {
        return;
    }
    auto dir = mountOf("/dev/mmcblk0p3", false);
    auto usage = dir ? usePercent(*dir) : std::nullopt;
    if (!usage)
    {
        lg2::error("Log partition {DEVICE} not found or not mounted", "DEVICE",
                   std::string("/dev/mmcblk0p3"));
        return;
    }
    if (*usage >= 90 && logUsage <= 89 && logUsage >= 1 && logUsageArmed)
    {
        event(bus, "ResourceEvent.1.0.ResourceWarningThresholdExceeded",
              "HMC Log Storage,90% Usage", {},
              "ResourceEvent.1.0.ResourceStatusChangedWarning");
        logUsageArmed = false;
    }
    logUsageArmed = logUsageArmed || *usage <= 85;
    logUsage = *usage;
}

// The one filesystem check, ten minutes in when the log partition is up.
void runFsHealthCheck(sdbusplus::bus_t& bus)
{
    const bool rwfsMounted = mountpoint(rwfsMount);
    const bool logMounted = mountpoint(logMount);
    if (!rwfsMounted)
    {
        lg2::error("Mount missing at {DIR}", "DIR", rwfsMount);
    }
    if (!logMounted)
    {
        lg2::error("Mount missing at {DIR}", "DIR", logMount);
    }
    bytes[14] = (rwfsMounted ? 0 : 0x81) | (logMounted ? 0 : 0x82);
    eepromWrite(telemetry, 0, bytes);

    bool recovered = false;
    if (!rwfsMounted && rwfs == "MtdErase")
    {
        lg2::warning("Erasing /dev/mtd6 partition on detection of corruption");
        flashEraseall("/dev/mtd6");
        recovered = true;
    }
    else if (!rwfsMounted)
    {
        lg2::error("[ERROR] rwfs is not mounted; recovery is owned by the eMMC "
                   "path");
    }
    if (!logMounted)
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator("/sys/class/mtd", ec))
        {
            std::string name;
            std::getline(std::ifstream(e.path() / "name"), name);
            if (name == "log")
            {
                lg2::warning("Erasing /dev/mtd7 partition on detection of "
                             "corruption");
                flashEraseall("/dev/mtd7");
                break;
            }
        }
        lg2::warning("Erasing /var/emmc/user-logs partition on detection of "
                     "corruption");
        UbootEnv::set("emmc_secure_erase_partition", "/var/emmc/user-logs");
        recovered = true;
    }

    // fw_printenv rwfs_recovery_count, "0" when unset.
    const auto countStr = UbootEnv().get("rwfs_recovery_count", "0");
    if (!countStr)
    {
        lg2::error("fw_printenv error: cannot read the u-boot environment");
    }
    std::optional<long> count;
    std::istringstream in(countStr.value_or(""));
    long value = 0;
    char extra = 0;
    if (in >> value && !(in >> extra))
    {
        count = value;
    }

    if (recovered && count && *count < maxRecoveryAttempts)
    {
        lg2::info("Data flash recovery done, rebooting");
        UbootEnv::set("dataflashrecovery", "yes");
        UbootEnv::set("rwfs_recovery_count", std::to_string(*count + 1));
        sleepS(5);
        reboot(bus, true);
        std::exit(0);
    }
    if (count && *count > 0 && !recovered && rwfsMounted)
    {
        lg2::info("Filesystem is healthy after previous recovery attempts, "
                  "resetting counter");
        event(bus, corrected,
              "HMC Rwfs,Filesystem is healthy after previous recovery "
              "attempts resetting counter");
        UbootEnv::set("rwfs_recovery_count", "0");
    }
    else if (!countStr && !recovered)
    {
        lg2::error("Failed to read environment variable rwfs_recovery_count "
                   "using fwgetenv.");
    }
    else if (!countStr)
    {
        lg2::warning("System has recovered using the recovery filesystem. An "
                     "external reboot is required to complete the resolution "
                     "as fwgetenv failed.");
        event(bus, detected,
              "HMC Rwfs,System has recovered using the recovery filesystem",
              "An external reboot is required to complete the resolution.");
    }
    else if (count && *count >= maxRecoveryAttempts)
    {
        lg2::error("Maximum RWFS recovery attempts ({ATTEMPTS}) reached. An "
                   "external recovery is required try physical power cycle "
                   "to complete the resolution.",
                   "ATTEMPTS", maxRecoveryAttempts);
        event(bus, detected,
              "HMC Rwfs,Recovery failed and stopped due to attempts reaching "
              "the limit",
              "An external recovery is required try physical power cycle to "
              "complete the resolution.");
    }
}

// The board's configuration file, if any, and what is detected.
void configure()
{
    if (std::ifstream f{hmc_mailbox::configFile}; f)
    {
        config = hmc_mailbox::parseConfig(f);
    }
    rwfs = config.rwfsRecovery.empty()
               ? hmc_mailbox::detectRwfsRecovery("/proc/mounts",
                                                 "/sys/class/mtd", rwfsMount)
               : config.rwfsRecovery;

    // platform_init.service creates the EEPROMs before this starts.
    const fs::path devices = "/sys/bus/i2c/devices";
    bool logged = false;
    while (true)
    {
        auto telemetryBus =
            config.telemetryBus
                ? config.telemetryBus
                : hmc_mailbox::findEepromBus(devices, config.telemetryAddress);
        auto controlBus =
            config.controlBus
                ? config.controlBus
                : hmc_mailbox::findEepromBus(devices, config.controlAddress);
        if (telemetryBus && controlBus)
        {
            telemetry =
                std::format("{}/{}-10{:02x}/slave-eeprom", devices.string(),
                            *telemetryBus, config.telemetryAddress);
            control =
                std::format("{}/{}-10{:02x}/slave-eeprom", devices.string(),
                            *controlBus, config.controlAddress);
            break;
        }
        if (!logged)
        {
            lg2::error(
                "No single I2C target EEPROM at {TELEMETRY} and "
                "{CONTROL}; waiting",
                "TELEMETRY", std::format("{:#04x}", config.telemetryAddress),
                "CONTROL", std::format("{:#04x}", config.controlAddress));
            logged = true;
        }
        sleepS(1);
    }
}

} // namespace

int main()
{
    configure();
    if (config.startupDelay > 0)
    {
        sleepS(static_cast<int>(config.startupDelay));
    }
    auto bus = sdbusplus::bus::new_default();
    readKernelConfig();
    readVersion();
    if (rwfs != "Initramfs")
    {
        // After a flash erase recovered the filesystems: log it, once.
        if (UbootEnv().get("dataflashrecovery", "") == "yes")
        {
            sleepS(10); // wait for the logging service
            lg2::info("Creating Redfish log for flash auto recovery");
            if (event(bus, corrected, "HMC Flash,Data Corruption"))
            {
                UbootEnv::set("dataflashrecovery");
            }
            else
            {
                lg2::error("Failed to create Redfish log for auto recovery");
            }
        }
    }
    if (config.signatureCheck)
    {
        std::error_code ec;
        fs::remove(signatureFile, ec);
    }

    bool rwfsFull = false;
    int rwfsFullCount = 0;
    int mountDelay = 0;
    int fsDelay = 0;
    bool fsChecked = false;
    int eventDelay = 0;
    bool checkEvents = true;
    int logUsage = 0;
    bool logUsageArmed = true;
    while (true)
    {
        updateUsb0(bus);
        updateServices(bus);
        updateDiagnostics(rwfsFull, mountDelay);
        eepromWrite(telemetry, 0, bytes);
        runCommand(bus);
        if (rwfs != "Initramfs")
        {
            // rwfs full three passes in a row: recover and reboot.
            rwfsFullCount = rwfsFull ? rwfsFullCount + 1 : 0;
            if (rwfsFullCount >= 3)
            {
                if (rwfs == "MtdErase")
                {
                    umountDevice("/dev/mtdblock6");
                    flashEraseall("/dev/mtd6");
                }
                event(bus, corrected, "HMC Rwfs,Resource Exhaustion");
                sleepS(5);
                rebootWithReason(bus, "Auto Recovery");
            }
            // Mounting the log partition takes a while: one check, ten
            // minutes in.
            if (fsDelay <= 600)
            {
                fsDelay += loopDelay;
            }
            else if (!fsChecked)
            {
                fsChecked = true;
                runFsHealthCheck(bus);
            }
        }
        if (eventDelay <= 120)
        {
            eventDelay += loopDelay;
        }
        else
        {
            eventDelay = 0;
            runEventCheck(bus, checkEvents, logUsage, logUsageArmed);
            updateUptime();
        }
        std::this_thread::sleep_for(std::chrono::seconds{loopDelay});
    }
}
