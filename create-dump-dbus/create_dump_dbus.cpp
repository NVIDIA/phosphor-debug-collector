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
#include <iostream>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace create
{

using namespace std;
using namespace sdbusplus;
using namespace phosphor::logging;

CreateDumpDbus::~CreateDumpDbus()
{
    dispose();
}

void CreateDumpDbus::dispose()
{
    if (fd > 0)
    {
        close(fd);
        remove(SOCKET_PATH);
    }

    if (dataSocket > 0)
        close(dataSocket);
}

int CreateDumpDbus::copyDumpToTmpDir(string dPath, string& response)
{
    constexpr auto TMP_DIR_PATH = "/tmp/";
    const auto copyOptions = filesystem::copy_options::update_existing;
    auto dName = filesystem::path(dPath).filename().string();
    auto fPath = string(DUMPS_PATH) + dName;

    // wait for collector creates dump file
    unsigned time_p = 0;
    while (true)
    {
        this_thread::sleep_for(chrono::milliseconds(50));

        if (filesystem::exists(fPath))
        {
            for (auto& entry : filesystem::directory_iterator(fPath))
            {
                if (std::filesystem::is_regular_file(entry))
                {
                    string filename = entry.path().filename();
                    filename.insert(0, "copy_");

                    response = fmt::format("Copying {} to {} directory.",
                                           entry.path().string(), TMP_DIR_PATH);

                    filesystem::path dest(TMP_DIR_PATH);
                    dest.append(filename);

                    this_thread::sleep_for(chrono::milliseconds(1000));
                    try
                    {
                        filesystem::copy(entry, dest, copyOptions);
                    }
                    catch (filesystem::filesystem_error& e)
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

int CreateDumpDbus::createDump(string& response)
{
    constexpr auto MAPPER_BUSNAME = "xyz.openbmc_project.Dump.Manager";
    constexpr auto MAPPER_PATH = "/xyz/openbmc_project/dump/bmc";
    constexpr auto MAPPER_INTERFACE = "xyz.openbmc_project.Dump.Create";
    constexpr auto METHOD_NAME = "CreateDump";

    auto b = bus::new_default();
    auto m = b.new_method_call(MAPPER_BUSNAME, MAPPER_PATH, MAPPER_INTERFACE,
                               METHOD_NAME);

    m.append(map<string, string>{});
    message::details::string_path_wrapper entry;

    try
    {
        auto reply = b.call(m);
        reply.read(entry);
    }
    catch (const exception::exception& e)
    {
        auto response = string("Failed to CreateDump: ") + e.what();

        return -1;
    }

    response = (string)entry;

    b.close();

    return 0;
}

void CreateDumpDbus::launchServer()
{
    sd_event* event = nullptr;
    struct sockaddr_un sock;
    mode_t target_mode = 0777;

    unique_ptr<sd_event, function<void(sd_event*)>> eventPtr(
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

    chmod(SOCKET_PATH, target_mode);

    r = listen(fd, 4);
    if (r < 0)
    {
        log<level::ERR>("Failed to launch server: listen returned error.");
        r = -errno;
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

            vector<unsigned char> buffer(BUFFER_SIZE);
            int ret = -1;

            ret = read(fd, &buffer[0], BUFFER_SIZE);
            if (ret < 0)
            {
                log<level::ERR>("Error while read data from socket.");
                close(fd);

                return 0;
            }

            string command(buffer.begin(), buffer.end());
            if (command.rfind(CREATE_DUMP_CMD) != string::npos)
            {
                string response;
                string path;
                log<level::INFO>("Creating dump ...");

                int cDumpResult = CreateDumpDbus::createDump(response);

                if (cDumpResult == 0)
                {
                    sendMsg(fd, fmt::format("DBus call succeed. Received: {}",
                                            response));
                    sendMsg(fd, "Waiting for dump creation ...");
                    path = response;
                    CreateDumpDbus::copyDumpToTmpDir(path, response);

                    sendMsg(fd, response);
                }
                else
                {
                    sendMsg(fd, response);
                }
            }

            sendMsg(fd, string(END_CMD));

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

void CreateDumpDbus::doCreateDumpCall()
{
    struct sockaddr_un addr;
    int ret = -1;
    vector<unsigned char> buffer(BUFFER_SIZE);

    dataSocket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (dataSocket == -1)
    {
        cerr << "socket" << endl;
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path));

    fd = connect(dataSocket, (const struct sockaddr*)&addr, sizeof(addr));

    if (fd == -1)
    {
        cerr << "The server is down." << endl;
        exit(EXIT_FAILURE);
    }

    sendMsg(dataSocket, string(CREATE_DUMP_CMD));

    while (true)
    {
        buffer.clear();
        buffer.resize(BUFFER_SIZE);

        ret = read(dataSocket, &buffer[0], BUFFER_SIZE);
        if (ret == -1)
        {
            cerr << "read" << endl;
            exit(EXIT_FAILURE);
        }

        string response(buffer.begin(), buffer.end());

        if (response.rfind(END_CMD) != string::npos)
        {
            break;
        }

        cout << response << endl;
    }

    close(dataSocket);
}

void CreateDumpDbus::sendMsg(int fd, string msg)
{
    if (fd != -1)
    {
        int ret;
        vector<unsigned char> buffer(msg.begin(), msg.end());

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
