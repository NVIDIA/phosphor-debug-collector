#include "config.h"

#include "dump-extensions.hpp"
#include "dump-extensions/faultlog-dump/faultlog-dump-extensions.hpp"
#include "dump-extensions/fdr-dump/fdr-dump-extensions.hpp"
#include "dump_manager.hpp"
#include "dump_manager_bmc.hpp"
#include "dump_manager_faultlog.hpp"
#include "elog_watch.hpp"
#include "watch.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <vector>

int main()
{
    using namespace phosphor::logging;
    using InternalFailure =
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

    auto bus = sdbusplus::bus::new_default();
    sd_event* event = nullptr;
    auto rc = sd_event_default(&event);
    if (rc < 0)
    {
        lg2::error("Error occurred during the sd_event_default, rc: {RC}", "RC",
                   rc);
        report<InternalFailure>();
        return rc;
    }

    rc = sd_event_set_watchdog(event, 1);
    if (rc < 0)
    {
        lg2::error("Failed to enable watchdog, rc: {RC}", "RC", rc);
        report<InternalFailure>();
        return rc;
    }

    phosphor::dump::EventPtr eventP{event};
    event = nullptr;

    std::filesystem::create_directories(BMC_DUMP_PATH);

    // Blocking SIGCHLD is needed for calling sd_event_add_child
    sigset_t mask;
    if (sigemptyset(&mask) < 0)
    {
        lg2::error("Unable to initialize signal set, errno: {ERRNO}", "ERRNO",
                   errno);
        return EXIT_FAILURE;
    }

    if (sigaddset(&mask, SIGCHLD) < 0)
    {
        lg2::error("Unable to add signal to signal set, errno: {ERRNO}",
                   "ERRNO", errno);
        return EXIT_FAILURE;
    }

    // Block SIGCHLD first, so that the event loop can handle it
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
    {
        lg2::error("Unable to block signal, errno: {ERRNO}", "ERRNO", errno);
        return EXIT_FAILURE;
    }

    // Add sdbusplus ObjectManager for the 'root' path of the DUMP manager.
    sdbusplus::server::manager_t objManager(bus, DUMP_OBJPATH);

    try
    {
        phosphor::dump::DumpManagerList dumpMgrList{};
        std::unique_ptr<phosphor::dump::bmc::Manager> bmcDumpMgr =
            std::make_unique<phosphor::dump::bmc::Manager>(
                bus, eventP, BMC_DUMP_OBJPATH, BMC_DUMP_OBJ_ENTRY,
                BMC_DUMP_PATH);

        phosphor::dump::bmc::Manager* ptrBmcDumpMgr = bmcDumpMgr.get();

        dumpMgrList.push_back(std::move(bmcDumpMgr));

        phosphor::dump::loadExtensions(bus, dumpMgrList);

#ifdef FAULTLOG_DUMP_EXTENSION
        phosphor::dump::loadExtensionsFaultLog(bus, dumpMgrList);
#endif

#ifdef FDR_DUMP_EXTENSION
        phosphor::dump::loadExtensionsFDR(bus, dumpMgrList);
#endif
        // Restore dbus objects of all dumps
        for (auto& dmpMgr : dumpMgrList)
        {
            dmpMgr->restore();
        }

        phosphor::dump::elog::Watch eWatch(bus, *ptrBmcDumpMgr);

        bus.attach_event(eventP.get(), SD_EVENT_PRIORITY_NORMAL);

        // Daemon is all set up so claim the busname now.
        bus.request_name(DUMP_BUSNAME);

        auto rc = sd_event_loop(eventP.get());
        if (rc < 0)
        {
            lg2::error("Error occurred during the sd_event_loop, rc: {RC}",
                       "RC", rc);
            elog<InternalFailure>();
        }
    }
    catch (const InternalFailure& e)
    {
        commit<InternalFailure>();
        return -1;
    }

    return 0;
}
