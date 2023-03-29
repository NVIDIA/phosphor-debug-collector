#include "create_dump_dbus.hpp"

#include <getopt.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <iostream>

using namespace phosphor::dump::create;

CreateDumpDbus* server = nullptr;

void dispose(int s)
{
    (void)s;
    delete server; // make sure destructor is called (free socket file)
}

void help()
{
    std::cout << "create_dump_dbus utility, supported arguments:" << std::endl;
    std::cout << "--help, -h:             ";
    std::cout << "prints argument list and exits" << std::endl;
    std::cout << "--server, -s:           ";
    std::cout << "launches the application in server mode (opens "
                 "an Unix domain socket listening to client connections)"
              << std::endl;
    std::cout << "--bmc-dump-path, -p:    ";
    std::cout
        << "server mode only; sets the path where the "
           "application looks for "
           "bmc dump files created by phosphor-debug-collector, default: ";
    std::cout << CreateDumpDbus::bmcDumpPath << std::endl;
    std::cout << "--system-dump-path, -q: ";
    std::cout << "server mode only; sets the path "
                 "where the application looks "
                 "for system dump files created by phosphor-debug-collector, "
                 "default: ";
    std::cout << CreateDumpDbus::systemDumpPath << std::endl;
    std::cout << "--type, -t:             ";
    std::cout << "client mode only; sets dump type, supported types: ";
    std::cout << CreateDumpDbus::printSupportedTypes();
    std::cout << "." << std::endl;
}

int main(int argc, char** argv)
{
    struct option opts[] = {{"help", no_argument, NULL, 'h'},
                            {"server", no_argument, NULL, 's'},
                            {"bmc-dump-path", required_argument, NULL, 'p'},
                            {"system-dump-path", required_argument, NULL, 'q'},
                            {"type", required_argument, NULL, 't'},
                            {0, 0, 0, 0}};

    int c, option_index = 0;
    bool serverMode = false;
    std::string bmcPath, systemPath, type;
    while ((c = getopt_long(argc, argv, "hsp:q:t:", opts, &option_index)) != -1)
    {
        switch (c)
        {
            case 'h':
                help();
                exit(0);

            case 's':
                serverMode = true;
                break;

            case 'p':
                bmcPath = std::string(optarg);
                break;

            case 'q':
                systemPath = std::string(optarg);
                break;

            case 't':
                type = std::string(optarg);
                break;

            default:
                std::cerr << "Unknown argument: -" << static_cast<char>(c)
                          << std::endl;
                break;
        }
    }

    if (serverMode)
    {
        if (!type.empty())
        {
            std::cerr << "Server mode, dump type argument is ignored"
                      << std::endl;
        }
        if (!bmcPath.empty())
        {
            CreateDumpDbus::bmcDumpPath = bmcPath;
        }
        if (!systemPath.empty())
        {
            CreateDumpDbus::systemDumpPath = systemPath;
        }
        // remove trailing '/' from the path
        if (CreateDumpDbus::bmcDumpPath[CreateDumpDbus::bmcDumpPath.size() -
                                        1] == '/')
        {
            CreateDumpDbus::bmcDumpPath.pop_back();
        }
        if (CreateDumpDbus::systemDumpPath
                [CreateDumpDbus::systemDumpPath.size() - 1] == '/')
        {
            CreateDumpDbus::systemDumpPath.pop_back();
        }

        server = new CreateDumpDbus();

        struct sigaction sigIntHandler;

        // free socket when SIGINT
        sigIntHandler.sa_handler = dispose;
        sigemptyset(&sigIntHandler.sa_mask);
        sigIntHandler.sa_flags = 0;

        sigaction(SIGINT, &sigIntHandler, NULL);
        sigaction(SIGABRT, &sigIntHandler, NULL);
        sigaction(SIGTERM, &sigIntHandler, NULL);
        sigaction(SIGTSTP, &sigIntHandler, NULL);

        // make sure socket file is cleaned up
        if (remove(SOCKET_PATH) < 0)
        {
            std::cerr << "remove call failed on socket" <<  std::endl;
        }

        server->launchServer();
    }
    else
    {
        if (!bmcPath.empty() || !systemPath.empty())
        {
            std::cerr << "Client mode, dump path arguments are ignored"
                      << std::endl;
        }
        if (!type.empty())
        {
            bool supported = false;
            for (auto& d : SUPPORTED_DUMP_TYPES)
            {
                if (d == type)
                {
                    supported = true;
                    break;
                }
            }
            if (!supported)
            {
                std::cerr << "Dump type '" << type
                          << "' is not supported. Supported types: ";
                std::cerr << CreateDumpDbus::printSupportedTypes();
                std::cerr << "." << std::endl;
                std::cerr << "Exiting." << std::endl;
                exit(-1);
            }
        }
        else
        {
            std::cerr << "No dump type specified, defaulting to 'BMC'"
                      << std::endl;
            type = "BMC";
        }
        CreateDumpDbus client;
        client.doCreateDumpCall(type);
    }

    return 0;
}
