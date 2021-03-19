#include "config.h"

#include "dump-extensions.hpp"

#include "dump_manager_system.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <sdbusplus/bus.hpp>
#include "xyz/openbmc_project/Common/error.hpp"

namespace phosphor
{
namespace dump
{

void loadExtensions(sdbusplus::bus::bus& bus, DumpManagerList& dumpList)
{
    using namespace phosphor::logging;
    using InternalFailure =
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

    sd_event* event = nullptr;
    auto rc = sd_event_default(&event);
    if (rc < 0)
    {
        log<level::ERR>("System dump: Error occurred during the sd_event_default",
                        entry("RC=%d", rc));
        report<InternalFailure>();
        return;
    }
    phosphor::dump::EventPtr eventP{event};
    event = nullptr;

    namespace fs = std::experimental::filesystem;
    fs::create_directories(SYSTEM_DUMP_PATH);

    dumpList.push_back(std::make_unique<phosphor::dump::system::Manager>(
        bus, eventP, SYSTEM_DUMP_OBJPATH, SYSTEM_DUMP_OBJ_ENTRY, SYSTEM_DUMP_PATH));
}
} // namespace dump
} // namespace phosphor
