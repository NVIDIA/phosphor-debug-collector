#pragma once

#include "config.h"

#include <sdbusplus/bus.hpp>

#include <filesystem>
#include <string>

namespace phosphor
{
namespace dump
{
namespace kdump
{

// Single-slot capture: capture /init writes vmcore-dmesg.txt, overwriting
// any prior dump. Match on the bare prefix (no trailing hyphen) so future
// suffix variants (.txt, .gz, etc.) still surface.
constexpr auto KDUMP_FILE_PREFIX = "vmcore-dmesg";

/** @class Manager
 *  @brief Kdump capture monitor — mirrors phosphor::dump::ramoops::Manager.
 *
 *  Iterates the kdump capture directory on construction. For each captured
 *  vmcore-dmesg file present on persistent storage, invokes
 *  xyz.openbmc_project.Dump.Create.CreateDump on the BMC dump manager with
 *  DumpType=Kdump and the file path. The dreport `kdump` plugin (also part
 *  of this MR) is invoked by the dump manager and is responsible for
 *  packaging and removing the source file after a successful copy. This
 *  preserves source-on-failure semantics: if any step fails the file is
 *  left in place for next-boot retry.
 */
class Manager
{
  public:
    Manager() = delete;
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager() = default;

    /** @brief Iterate the kdump dump directory and create a BMC dump entry
     *         for each captured vmcore-dmesg file.
     *  @param[in] bus - sdbusplus bus connection.
     */
    explicit Manager(sdbusplus::bus_t& bus);

  private:
    /** @brief Emit a phosphor-logging Redfish event for the captured
     *         artifact (KDP-REQ-20 success or KDP-REQ-21 failure).
     *  @param[in] filePath      - absolute path of the source vmcore-dmesg.
     *  @param[in] success       - true for ResourceCreated, false for
     *                             ResourceErrorsDetected.
     *  @param[in] entryOrReason - on success, the BMC dump entry path
     *                             returned by CreateDump; on failure, a
     *                             short failure-category string included
     *                             in REDFISH_MESSAGE_ARGS.
     */
    void createKdumpEvent(const std::filesystem::path& filePath, bool success,
                          const std::string& entryOrReason = {});

    /** @brief Invoke Dump.Manager.CreateDump on /xyz/openbmc_project/dump/bmc
     *         with DumpType=Kdump and the file path.
     *  @returns the created entry object path on success.
     *  @throws sdbusplus::exception::SdBusError on D-Bus failure.
     */
    std::string createKdump(const std::filesystem::path& filePath);

    /** @brief sdbusplus bus connection (owned by main()). */
    sdbusplus::bus_t& bus;
};

} // namespace kdump
} // namespace dump
} // namespace phosphor
