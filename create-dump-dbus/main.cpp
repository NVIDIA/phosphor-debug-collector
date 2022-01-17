#include "create_dump_dbus.hpp"

#include <csignal>
#include <cstring>
#include <iostream>

using namespace std;
using namespace phosphor::dump::create;

CreateDumpDbus* server = nullptr;

void dispose(int s)
{
    (void)s;
    delete server; // make sure destructor is called (free socket file)
}

int main(int argc, char** argv)
{
    if (argc > 1 && (strncmp(argv[1], "-s", 2) == 0))
    {
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

        server->launchServer();
    }
    else if (argc < 2)
    {
        CreateDumpDbus client;
        client.doCreateDumpCall();
    }
    else
    {
        cout << "unrecognized arguments" << endl;
    }

    return 0;
}