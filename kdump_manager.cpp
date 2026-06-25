#include "config.h"

#include "kdump_manager.hpp"

#include "dump_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Common/OriginatedBy/common.hpp>
#include <xyz/openbmc_project/Dump/Create/common.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace kdump
{

namespace fs = std::filesystem;

namespace
{

constexpr auto LOGGING_BUSNAME = "xyz.openbmc_project.Logging";
constexpr auto LOGGING_INTERNAL_PATH =
    "/xyz/openbmc_project/logging/internal/manager";
constexpr auto LOGGING_INTERNAL_IFACE =
    "xyz.openbmc_project.Logging.Internal.Manager";

constexpr auto DUMP_CREATE_IFACE = "xyz.openbmc_project.Dump.Create";

constexpr auto MSGID_RESOURCE_CREATED = "ResourceEvent.1.2.ResourceCreated";
constexpr auto MSGID_RESOURCE_ERRORS_DETECTED =
    "ResourceEvent.1.2.ResourceErrorsDetected";

constexpr auto SEVERITY_NOTICE =
    "xyz.openbmc_project.Logging.Entry.Level.Notice";
constexpr auto SEVERITY_WARNING =
    "xyz.openbmc_project.Logging.Entry.Level.Warning";

constexpr auto ORIGINATOR_ID = "kdump-monitor";

} // namespace

Manager::Manager(sdbusplus::bus_t& busIn) : bus(busIn)
{
    if (!fs::exists(KDUMP_DUMP_PATH))
    {
        lg2::info("kdump path does not exist, nothing to capture: {PATH}",
                  "PATH", std::string{KDUMP_DUMP_PATH});
        return;
    }

    std::vector<fs::path> files;
    try
    {
        for (const auto& entry : fs::directory_iterator(KDUMP_DUMP_PATH))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const auto& name = entry.path().filename().string();
            if (name.starts_with(KDUMP_FILE_PREFIX))
            {
                files.push_back(entry.path());
            }
        }
    }
    catch (const fs::filesystem_error& e)
    {
        // eMMC could disappear mid-scan (unmount, permission change).
        // Log and bail; artifact stays in place for next-boot retry.
        lg2::error("Failed to scan kdump path {PATH}: {WHAT}", "PATH",
                   std::string{KDUMP_DUMP_PATH}, "WHAT", e.what());
        return;
    }

    if (files.empty())
    {
        lg2::info("kdump path empty, nothing to capture: {PATH}", "PATH",
                  std::string{KDUMP_DUMP_PATH});
        return;
    }

    // Process in deterministic (filename) order — vmcore-dmesg-<ts>
    // sorts chronologically.
    std::sort(files.begin(), files.end());

    if (files.size() > KDUMP_MAX_ENTRIES_PER_BOOT)
    {
        lg2::warning("kdump path has {COUNT} artifacts but capture is "
                     "single-slot; processing first {MAX}, leaving the rest "
                     "in place for inspection",
                     "COUNT", files.size(), "MAX", KDUMP_MAX_ENTRIES_PER_BOOT);
        files.resize(KDUMP_MAX_ENTRIES_PER_BOOT);
    }

    for (const auto& filePath : files)
    {
        try
        {
            auto entryPath = createKdump(filePath);
            createKdumpEvent(filePath, true, entryPath);
            lg2::info("kdump capture surfaced as dump entry {ENTRY}: {FILE}",
                      "ENTRY", entryPath, "FILE", filePath.string());
        }
        catch (const sdbusplus::exception_t& e)
        {
            lg2::error("CreateDump failed for {FILE}: {WHAT}", "FILE",
                       filePath.string(), "WHAT", e.what());
            createKdumpEvent(filePath, false, std::string{e.name()});
        }
        catch (const std::exception& e)
        {
            lg2::error("CreateDump failed for {FILE}: {WHAT}", "FILE",
                       filePath.string(), "WHAT", e.what());
            createKdumpEvent(filePath, false, "InternalError");
        }
    }
}

std::string Manager::createKdump(const fs::path& filePath)
{
    using DumpIntr = sdbusplus::common::xyz::openbmc_project::dump::Create;
    using DumpType = DumpIntr::DumpType;
    using CreateParameters = DumpIntr::CreateParameters;
    using OriginatedBy =
        sdbusplus::common::xyz::openbmc_project::common::OriginatedBy;

    phosphor::dump::DumpCreateParams params;

    params[DumpIntr::convertCreateParametersToString(
        CreateParameters::DumpType)] =
        DumpIntr::convertDumpTypeToString(DumpType::Kdump);
    params[DumpIntr::convertCreateParametersToString(
        CreateParameters::FilePath)] = filePath.string();
    params[DumpIntr::convertCreateParametersToString(
        CreateParameters::OriginatorId)] = std::string{ORIGINATOR_ID};
    params[DumpIntr::convertCreateParametersToString(
        CreateParameters::OriginatorType)] =
        OriginatedBy::convertOriginatorTypesToString(
            OriginatedBy::OriginatorTypes::Internal);

    auto method = bus.new_method_call(DUMP_BUSNAME, BMC_DUMP_OBJPATH,
                                      DUMP_CREATE_IFACE, "CreateDump");
    method.append(params);
    auto reply = bus.call(method);

    sdbusplus::object_path objPath;
    reply.read(objPath);
    return objPath.str;
}

void Manager::createKdumpEvent(const fs::path& filePath, bool success,
                               const std::string& entryOrReason)
{
    // The phosphor-logging sendEvent() helper is only available with a
    // boost::asio connection and a fixed message-id table that does not
    // include ResourceErrorsDetected. To keep this oneshot synchronous
    // and to support both the success (ResourceCreated) and failure
    // (ResourceErrorsDetected) paths called out in work order §5, invoke
    // RFSendEvent directly on the Logging.Internal.Manager interface.
    try
    {
        const std::string messageId =
            success ? MSGID_RESOURCE_CREATED : MSGID_RESOURCE_ERRORS_DETECTED;
        const std::string severity =
            success ? SEVERITY_NOTICE : SEVERITY_WARNING;

        // REDFISH_MESSAGE_ARGS:
        //   success: "Kdump,<created entry path>"
        //   failure: "Kdump,<failure category>"
        std::string args = "Kdump,";
        args += entryOrReason;

        std::map<std::string, std::string> addData;
        addData["REDFISH_MESSAGE_ID"] = messageId;
        addData["REDFISH_MESSAGE_ARGS"] = args;
        if (success)
        {
            // origin-of-condition is the created BMC dump entry path.
            addData["REDFISH_ORIGIN_OF_CONDITION"] = entryOrReason;
        }
        else
        {
            // No dump entry on failure; surface the source artifact path
            // as the origin so operators can correlate the alert.
            addData["REDFISH_ORIGIN_OF_CONDITION"] = filePath.string();
        }

        const std::string message =
            success ? std::string{"BMC kernel-panic dump captured"}
                    : std::string{"BMC kernel-panic dump capture failed: "} +
                          entryOrReason;

        auto method =
            bus.new_method_call(LOGGING_BUSNAME, LOGGING_INTERNAL_PATH,
                                LOGGING_INTERNAL_IFACE, "RFSendEvent");
        method.append(message, severity, addData);
        bus.call(method);
    }
    catch (const sdbusplus::exception_t& e)
    {
        // Logging is best-effort — never block the capture flow.
        lg2::error("RFSendEvent failed for kdump capture {FILE}: {WHAT}",
                   "FILE", filePath.string(), "WHAT", e.what());
    }
    catch (const std::exception& e)
    {
        lg2::error("RFSendEvent failed for kdump capture {FILE}: {WHAT}",
                   "FILE", filePath.string(), "WHAT", e.what());
    }
}

} // namespace kdump
} // namespace dump
} // namespace phosphor
