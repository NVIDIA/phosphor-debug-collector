#include "create_dump_dbus.hpp"

#define FMT_HEADER_ONLY

#include <fmt/core.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <vector>

#include "../config.h"

namespace phosphor
{
namespace dump
{
namespace create
{

using namespace sdbusplus;
using namespace phosphor::logging;

std::string CreateDumpDbus::bmcDumpPath(BMC_DUMP_PATH);
std::string CreateDumpDbus::systemDumpPath(SYSTEM_DUMP_PATH);

CreateDumpDbus::~CreateDumpDbus()
{
    dispose();
}

void CreateDumpDbus::dispose()
{
    if (fd > 0)
    {
        close(fd);
        if (remove(SOCKET_PATH) < 0)
        {
            std::cerr << "remove called failed on socket" << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    if (dataSocket > 0)
    {
        close(dataSocket);
    }
}

void CreateDumpDbus::waitForDumpCreation(const std::string& entryPath)
{
    constexpr auto MAPPER_BUSNAME = "xyz.openbmc_project.Dump.Manager";
    constexpr auto MAPPER_INTERFACE = "xyz.openbmc_project.Common.Progress";
    constexpr auto PROPERTY_NAME = "Status";
    constexpr auto STATUS_IN_PROGRESS =
        "xyz.openbmc_project.Common.Progress.OperationStatus.InProgress";
    constexpr auto STATUS_COMPLETED =
        "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";

    int fails = 0;
    unsigned time_p = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // if dump collector will not change status after 30 minutes throws
        // exception
        if (time_p > 1000 * 60 * 30)
        {
            throw CreateDumpDbusException("Dump creation timed out.");
        }

        try
        {
            auto b = bus::new_default();
            auto m =
                b.new_method_call(MAPPER_BUSNAME, entryPath.c_str(),
                                  "org.freedesktop.DBus.Properties", "Get");
            m.append(MAPPER_INTERFACE, PROPERTY_NAME);
            auto reply = b.call(m);

            std::variant<std::monostate, std::string> entry;
            reply.read(entry);

            if (std::get<1>(entry) != STATUS_IN_PROGRESS)
            {
                if (std::get<1>(entry) == STATUS_COMPLETED)
                {
                    return;
                }
                else
                {
                    throw CreateDumpDbusException("Dump creation failed.");
                }
            }
        }
        catch (const exception::exception& e)
        {
            fails++;

            if (fails >= 3)
            {
                throw CreateDumpDbusException("Failed to get progress.");
            }
        }

        time_p += 50;
    }
}

int CreateDumpDbus::copyDumpToTmpDir(const std::string& dPath,
                                     std::string& response)
{
    const auto copyOptions = std::filesystem::copy_options::update_existing;
    auto dName = std::filesystem::path(dPath).filename().string();
    std::string sourcePath;
    if (dPath.find("/system/") != std::string::npos)
    {
        sourcePath = CreateDumpDbus::systemDumpPath;
    }
    else if (dPath.find("/bmc/") != std::string::npos)
    {
        sourcePath = CreateDumpDbus::bmcDumpPath;
    }
    else
    {
        log<level::ERR>("Unknown dump file path");
        response = "Unknown dump file path";
        return -1;
    }
    auto fPath = sourcePath + "/" + dName;

    try
    {
        // wait for collector create dump file
        CreateDumpDbus::waitForDumpCreation(response);
    }
    catch (CreateDumpDbusException& e)
    {
        log<level::ERR>(e.what().c_str());
        response = e.what();
        return -1;
    }

    unsigned time_p = 0;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (std::filesystem::exists(fPath))
        {
            for (auto& entry : std::filesystem::directory_iterator(fPath))
            {
                if (std::filesystem::is_regular_file(entry))
                {
                    std::string filename = entry.path().filename();
                    filename.insert(0, DUMP_COPY_PREFIX);

                    response = fmt::format("Copying {} to {} directory.",
                                           entry.path().string(), TMP_DIR_PATH);

                    std::filesystem::path dest(TMP_DIR_PATH);
                    dest.append(filename);

                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1000));
                    try
                    {
                        std::filesystem::copy(entry, dest, copyOptions);
                    }
                    catch (std::filesystem::filesystem_error& e)
                    {
                        response = e.what();
                        return -1;
                    }

                    return 0;
                }
            }
        }

        time_p += 50;
        if (time_p > TIMEOUT)
        {
            response = "Copying dump to tmp dir failed: timeout.";
            return -1;
        }
    }
}

int CreateDumpDbus::createDump(const std::string& type, std::string& response)
{
    constexpr auto MAPPER_BUSNAME = "xyz.openbmc_project.Dump.Manager";
    constexpr auto MAPPER_PATH_PREFIX = "/xyz/openbmc_project/dump/";
    constexpr auto MAPPER_INTERFACE = "xyz.openbmc_project.Dump.Create";
    constexpr auto METHOD_NAME = "CreateDump";

    std::string path;
    std::map<std::string, std::string> paramMap{};
    if (type.empty() || type == "BMC")
    {
        path = std::string(MAPPER_PATH_PREFIX) + "bmc";
    }
    else
    {
        path = std::string(MAPPER_PATH_PREFIX) + "system";
        paramMap["DiagnosticType"] = type;
    }

    auto bus = bus::new_default();
    auto method = bus.new_method_call(MAPPER_BUSNAME, path.c_str(),
                                      MAPPER_INTERFACE, METHOD_NAME);

    method.append(paramMap);
    message::details::string_path_wrapper entry;

    int ret = 0;
    try
    {
        auto reply = bus.call(method);
        reply.read(entry);
    }
    catch (const exception::exception& e)
    {
        response = std::string("Failed to create dump: ");
        response += std::string("path - '") + path + std::string("', ");
        response += std::string("type - '") + type + std::string("', ");
        response += std::string("error - '") + e.what() + std::string("'");
        ret = -1;
    }
    if (ret == 0)
    {
        response = std::string(entry);
    }
    bus.close();

    return ret;
}

void CreateDumpDbus::processSingleDump(int fd, const std::string& type)
{
    std::string response;
    int cDumpResult = CreateDumpDbus::createDump(type, response);
    if (cDumpResult == 0)
    {
        sendMsg(
            fd,
            fmt::format(
                "CreateDump call successful for dump type '{}', received: {}",
                type, response));
        sendMsg(fd, "Waiting for dump creation to finish...");
        std::string path = response;
        CreateDumpDbus::copyDumpToTmpDir(path, response);
        sendMsg(fd, response);
    }
    else
    {
        sendMsg(fd, response);
    }
}

void CreateDumpDbus::processDumpRequest(int fd, const std::string& type)
{
    sendMsg(fd, "Deleting existing dump files...");
    std::filesystem::path tmpDir(TMP_DIR_PATH);
    if (std::filesystem::exists(tmpDir))
    {
        for (auto& entry : std::filesystem::directory_iterator(tmpDir))
        {
            if (std::filesystem::is_regular_file(entry))
            {
                std::string filename = entry.path().filename();
                if (filename.rfind(DUMP_COPY_PREFIX, 0) == 0 &&
                    filename.find("dump") != std::string::npos)
                {
                    {
                        try
                        {
                            std::filesystem::remove(entry);
                        }
                        catch (const std::exception&)
                        {
                            std::string err =
                                "Failed to delete dump file: " + filename;
                            sendMsg(fd, err);
                            log<level::ERR>(err.c_str());
                        }
                    }
                }
            }
        }
    }
    if (type == "all")
    {
        for (const auto& d : SUPPORTED_DUMP_TYPES)
        {
            if (d != "all")
            {
                CreateDumpDbus::processSingleDump(fd, d);
            }
        }
    }
    else
    {
        CreateDumpDbus::processSingleDump(fd, type);
    }
}

void CreateDumpDbus::launchServer()
{
    sd_event* event = nullptr;
    struct sockaddr_un sock;
    mode_t target_mode = 0777;

    std::unique_ptr<sd_event, std::function<void(sd_event*)>> eventPtr(
        event, [](sd_event* event) {
            if (!event)
            {
                event = sd_event_unref(event);
            }
        });

    int r = -1;
    sigset_t ss;

    r = sd_event_default(&event);
    if (r < 0)
    {
        goto finish;
    }

    eventPtr.reset(event);
    event = nullptr;

    if (sigemptyset(&ss) < 0 || sigaddset(&ss, SIGTERM) < 0 ||
        sigaddset(&ss, SIGINT) < 0)
    {
        r = -errno;
        goto finish;
    }

    /* Block SIGTERM first, so that the event loop can handle it */
    if (sigprocmask(SIG_BLOCK, &ss, NULL) < 0)
    {
        r = -errno;
        goto finish;
    }

    /* Let's make use of the default handler and "floating"
       reference features of sd_event_add_signal() */
    r = sd_event_add_signal(eventPtr.get(), NULL, SIGTERM, NULL, NULL);
    if (r < 0)
    {
        goto finish;
    }

    r = sd_event_add_signal(eventPtr.get(), NULL, SIGINT, NULL, NULL);
    if (r < 0)
    {
        goto finish;
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
    {
        r = -errno;
        goto finish;
    }

    sock.sun_family = AF_UNIX;
    strncpy(sock.sun_path, (char*)SOCKET_PATH, sizeof(sock.sun_path));

    r = bind(fd, (struct sockaddr*)&sock, sizeof(struct sockaddr_un));
    if (r < 0)
    {
        r = -errno;
        goto finish;
    }

    r = chmod(SOCKET_PATH, target_mode);
    if (r < 0)
    {
        log<level::ERR>(fmt::format("chmod error: {}", r).c_str());
        goto finish;
    }

    r = listen(fd, 4);
    if (r < 0)
    {
        r = -errno;
        log<level::ERR>(fmt::format("listen error: {}", r).c_str());
        goto finish;
    }

    r = sd_event_add_io(
        eventPtr.get(), nullptr, fd, EPOLLIN,
        [](sd_event_source*, int fd, uint32_t, void*) -> int {
            fd = accept(fd, NULL, NULL);

            if (fd < 0)
            {
                return 0;
            }

            std::vector<unsigned char> buffer(BUFFER_SIZE);
            int ret = read(fd, &buffer[0], BUFFER_SIZE);
            if (ret < 0)
            {
                log<level::ERR>(fmt::format("read error: {}", ret).c_str());
                close(fd);

                return 0;
            }

            std::string command(buffer.begin(), buffer.end());
            std::istringstream iss(command);
            std::vector<std::string> tokens{
                std::istream_iterator<std::string>{iss},
                std::istream_iterator<std::string>{}};
            if (tokens.size() != 0 && tokens[0] == std::string(CREATE_DUMP_CMD))
            {
                std::string type;
                if (tokens.size() > 1)
                {
                    tokens[1].erase(std::remove_if(tokens[1].begin(),
                                                   tokens[1].end(),
                                                   [](auto const& c) -> bool {
                                                       return !std::isalnum(c);
                                                   }),
                                    tokens[1].end());
                    for (const auto& d : SUPPORTED_DUMP_TYPES)
                    {
                        if (d == tokens[1])
                        {
                            type = d;
                            break;
                        }
                    }
                    if (type.empty())
                    {
                        sendMsg(fd, "Invalid dump type requested");
                        log<level::ERR>(
                            fmt::format("Invalid dump type requested: {}",
                                        tokens[1])
                                .c_str());
                    }
                }
                else
                {
                    type = DEFAULT_DUMP_TYPE;
                }
                if (!type.empty())
                {
                    log<level::INFO>(
                        fmt::format("Processing dump request, type: {}", type)
                            .c_str());
                    CreateDumpDbus::processDumpRequest(fd, type);
                }
            }
            sendMsg(fd, std::string(END_CMD));
            close(fd);

            return 0;
        },
        nullptr);

    if (r < 0)
    {
        goto finish;
    }

    r = sd_event_loop(eventPtr.get());

finish:
    dispose();

    if (r < 0)
    {
        auto err = std::strerror(-r);
        log<level::ERR>(fmt::format("Failure: {}", err).c_str());
    }
}

void CreateDumpDbus::doCreateDumpCall(const std::string& type)
{
    struct sockaddr_un addr;
    int ret = -1;
    std::vector<unsigned char> buffer(BUFFER_SIZE);

    dataSocket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (dataSocket == -1)
    {
        std::cerr << "socket" << std::endl;
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path));

    fd = connect(dataSocket, (const struct sockaddr*)&addr, sizeof(addr));

    if (fd == -1)
    {
        std::cerr << "The server is down." << std::endl;
        exit(EXIT_FAILURE);
    }

    std::string command(CREATE_DUMP_CMD);
    if (!type.empty())
    {
        command += " " + type;
    }
    sendMsg(dataSocket, command);

    while (true)
    {
        buffer.clear();
        buffer.resize(BUFFER_SIZE);

        ret = read(dataSocket, &buffer[0], BUFFER_SIZE);
        if (ret == -1)
        {
            std::cerr << "read" << std::endl;
            exit(EXIT_FAILURE);
        }

        std::string response(buffer.begin(), buffer.end());

        if (response.rfind(END_CMD) != std::string::npos)
        {
            break;
        }

        std::cout << response << std::endl;
    }

    close(dataSocket);
}

void CreateDumpDbus::sendMsg(int fd, const std::string& msg)
{
    if (fd != -1)
    {
        int ret;
        std::vector<unsigned char> buffer(msg.begin(), msg.end());

        ret = write(fd, buffer.data(), buffer.size());
        if (ret == -1)
        {
            log<level::ERR>("Error while writing response data.");
            close(fd);

            throw std::exception();
        }
    }
    else
    {
        log<level::ERR>("socket closed");

        throw std::exception();
    }
}

} // namespace create
} // namespace dump
} // namespace phosphor
