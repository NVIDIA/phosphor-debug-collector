#include "config.h"

#include "ramoops_manager.hpp"

#include "dump_manager.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Dump/Create/common.hpp>
#include <xyz/openbmc_project/Dump/Create/server.hpp>

#include <filesystem>

namespace phosphor
{
namespace dump
{
namespace ramoops
{

Manager::Manager(const std::string& filePath)
{
    std::filesystem::path dir(filePath);
    if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir))
    {
        return;
    }

    // Create error to notify user that a ramoops has been detected
    createError();

    std::vector<std::string> files;
    files.push_back(filePath);

    createHelper(files);
}

void Manager::createError()
{
    try
    {
        std::map<std::string, std::string> additionalData;

        // Always add the _PID on for some extra logging debug
        additionalData.emplace("_PID", std::to_string(getpid()));

        auto bus = sdbusplus::bus::new_default();
        auto method = bus.new_method_call(
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create");

        method.append("xyz.openbmc_project.Dump.Error.Ramoops",
                      sdbusplus::server::xyz::openbmc_project::logging::Entry::
                          Level::Error,
                      additionalData);
        auto resp = bus.call(method);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error(
            "sdbusplus D-Bus call exception, error {ERROR} trying to create "
            "an error for ramoops detection",
            "ERROR", e);
        // This is a best-effort logging situation so don't throw anything
    }
    catch (const std::exception& e)
    {
        lg2::error("D-bus call exception: {ERROR}", "ERROR", e);
        // This is a best-effort logging situation so don't throw anything
    }
}

void Manager::createHelper(const std::vector<std::string>& files)
{
    constexpr auto DUMP_CREATE_IFACE = "xyz.openbmc_project.Dump.Create";

    // The unit is ordered after the dump manager which owns its bus name
    // once registered so no mapper dependency is needed.
    auto b = sdbusplus::bus::new_default();
    auto m = b.new_method_call(DUMP_BUSNAME, BMC_DUMP_OBJPATH,
                               DUMP_CREATE_IFACE, "CreateDump");
    m.append(createDumpParams(files));
    try
    {
        b.call_noreply(m);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to create ramoops dump, errormsg: {ERROR}", "ERROR",
                   e);
    }
}

phosphor::dump::DumpCreateParams Manager::createDumpParams(
    const std::vector<std::string>& files)
{
    phosphor::dump::DumpCreateParams params;
    if (files.empty())
    {
        lg2::error("Cannot build CreateDump params: files list is empty");
        return params;
    }
    using CreateParameters =
        sdbusplus::common::xyz::openbmc_project::dump::Create::CreateParameters;
    using DumpType =
        sdbusplus::common::xyz::openbmc_project::dump::Create::DumpType;
    using DumpIntr = sdbusplus::common::xyz::openbmc_project::dump::Create;
    params[DumpIntr::convertCreateParametersToString(
        CreateParameters::DumpType)] =
        DumpIntr::convertDumpTypeToString(DumpType::Ramoops);
    params[DumpIntr::convertCreateParametersToString(
        CreateParameters::FilePath)] = files.front();
    return params;
}

} // namespace ramoops
} // namespace dump
} // namespace phosphor
