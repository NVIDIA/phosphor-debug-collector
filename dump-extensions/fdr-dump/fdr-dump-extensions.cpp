#include "config.h"

#include "fdr-dump-extensions.hpp"

#include "dump-extensions.hpp"
#include "dump_manager_fdr.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <sdbusplus/bus.hpp>

#include "dump-extensions/fdr-dump/fdr_dump_config.h"

namespace phosphor
{
namespace dump
{

void loadExtensionsFDR(sdbusplus::bus::bus& bus, DumpManagerList& dumpList)
{
    using namespace phosphor::logging;
    using InternalFailure =
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

    sd_event* event = nullptr;
    auto rc = sd_event_default(&event);
    if (rc < 0)
    {
        log<level::ERR>("FDR dump: Error occurred during the sd_event_default",
                        entry("RC=%d", rc));
        report<InternalFailure>();
        return;
    }
    phosphor::dump::EventPtr eventP{event};
    event = nullptr;

    std::filesystem::create_directories(FDR_DUMP_PATH);

    dumpList.push_back(std::make_unique<phosphor::dump::FDR::Manager>(
        bus, eventP, FDR_DUMP_OBJPATH, FDR_DUMP_OBJ_ENTRY, FDR_DUMP_PATH));
}
} // namespace dump
} // namespace phosphor
