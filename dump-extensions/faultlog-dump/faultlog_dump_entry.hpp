#pragma once

#include "dump_entry.hpp"
#include "xyz/openbmc_project/Dump/Entry/FaultLog/server.hpp"
#include "xyz/openbmc_project/Dump/Entry/server.hpp"
#include "xyz/openbmc_project/Object/Delete/server.hpp"
#include "xyz/openbmc_project/Time/EpochTime/server.hpp"
#include "xyz/openbmc_project/Dump/Entry/CPERDecode/server.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>

#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace phosphor
{
namespace dump
{
namespace faultLog
{
template <typename T>
using ServerObject = typename sdbusplus::server::object::object<T>;

using EntryIfaces = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Dump::Entry::server::FaultLog, sdbusplus::xyz::openbmc_project::Dump::Entry::server::CPERDecode>;

using FaultDataType = sdbusplus::xyz::openbmc_project::Dump::Entry::server::
    FaultLog::FaultDataType;


namespace fs = std::filesystem;
class Manager;

/** @class Entry
 *  @brief OpenBMC Dump Entry implementation.
 *  @details A concrete implementation for the
 *  xyz.openbmc_project.Dump.Entry DBus API
 */
class Entry : virtual public EntryIfaces, virtual public phosphor::dump::Entry
{
  public:
    Entry() = delete;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;
    ~Entry() = default;

    /** @brief Constructor for the System Dump Entry Object
     *  @param[in] bus - Bus to attach to.
     *  @param[in] objPath - Object path to attach to
     *  @param[in] dumpId - Dump id.
     *  @param[in] timeStamp - Dump creation timestamp
     *             since the epoch.
     *  @param[in] faultLogType - Type of  fault data
     *  @param[in] additionalTypeName - Additional string to further the type
     *  @param[in] fileSize - Dump file size in bytes.
     *  @param[in] file - Name of dump file.
     *  @param[in] status - status  of the dump.
     *  @param[in] parent - The dump entry's parent.
     */
    Entry(sdbusplus::bus::bus &bus, const std::string &objPath, uint32_t dumpId,
          uint64_t timeStamp, FaultDataType typeIn,
          const std::string &additionalTypeNameIn, const std::string &primaryLogIdIn, uint64_t fileSize,
          const fs::path &file, phosphor::dump::OperationStatus status, const std::string &SectionTypeIn, const std::string &fruIDIn, const std::string &severityIn, const std::string &nvIPSigIn, const std::string &nvSevIn, const std::string &nvSockNumIn, const std::string &pcieVendorIDIn, const std::string &pcieDeviceIDIn, const std::string &pcieClassCodeIn, const std::string &pcieFuncNumberIn, const std::string &pcieDeviceNumberIn, const std::string &pcieSegmentNumberIn, const std::string &pcieDeviceBusNumberIn, const std::string &pcieSecondaryBusNumberIn, const std::string &pcieSlotNumberIn,
          phosphor::dump::Manager &parent) : EntryIfaces(bus, objPath.c_str(), EntryIfaces::action::defer_emit),phosphor::dump::Entry(bus, objPath.c_str(), dumpId, timeStamp, fileSize, status, parent),
        file(file)
    {
        type(typeIn);
        additionalTypeName(additionalTypeNameIn);
        primaryLogId(primaryLogIdIn);
        sectionType(SectionTypeIn);
        fruid(fruIDIn);
        severity(severityIn);
        nvipSignature(nvIPSigIn);
        nvSeverity(nvSevIn);
        nvSocketNumber(nvSockNumIn);
        pcieVendorID(pcieVendorIDIn);
        pcieDeviceID(pcieDeviceIDIn);
        pcieClassCode(pcieClassCodeIn);
        pcieFunctionNumber(pcieFuncNumberIn);
        pcieDeviceNumber(pcieDeviceNumberIn);
        pcieSegmentNumber(pcieSegmentNumberIn);
        pcieDeviceBusNumber(pcieDeviceBusNumberIn);
        pcieSecondaryBusNumber(pcieSecondaryBusNumberIn);
        pcieSlotNumber(pcieSlotNumberIn);

        //  Emit deferred signal.
        this->phosphor::dump::faultLog::EntryIfaces::emit_object_added();
    }

    /** @brief Delete this d-bus object.
     */
    void delete_() override;

    /** @brief Method to initiate the offload of dump
     *  @param[in] uri - URI to offload dump
     */
    void initiateOffload(std::string uri) override;

    /**
    * @brief Method to update an existing dump entry, once the dump creation
    *        is completed this function will be used to update the entry which got
    *        created during the dump request.
    * @param[in] timeStamp - Dump creation timestamp
    * @param[in] fileSize - Dump file size in bytes.
    * @param[in] file - Name of dump file.
    */
    void update(uint64_t timeStamp, uint64_t fileSize, const fs::path& filePath, std::string id)
    {
        elapsed(timeStamp);
        size(fileSize);
        status(OperationStatus::Completed);
        file = filePath;
        completedTime(timeStamp);

        std::string cperDecodePath = "/var/lib/logging/dumps/faultlog/" + id + "/Decoded/decoded.json";
        std::ifstream cperFile(cperDecodePath.c_str());

        if (cperFile.is_open())
        {
            json jsonData;
            jsonData = json::parse(cperFile);
            if (jsonData.contains("Header") && jsonData["Header"].contains("Section Count"))
            {
                int secCount = jsonData["Header"]["Section Count"];

                for (int i = 0; i < secCount; i++)
                {
                    if (jsonData.contains("Sections") && jsonData["Sections"].is_array() &&
                        !jsonData["Sections"].empty())
                    {
                        if (jsonData["Sections"][i].contains("Section Descriptor"))
                        {
                            // Extracting existing fields
                            if (jsonData["Sections"][i]["Section Descriptor"].contains("Section Type")) {
                                sectionType(jsonData["Sections"][i]["Section Descriptor"]["Section Type"]);
                            }

                            if (jsonData["Sections"][i]["Section Descriptor"].contains("FRU Id")) {
                                fruid(jsonData["Sections"][i]["Section Descriptor"]["FRU Id"]);
                            }

                            if (jsonData["Sections"][i]["Section Descriptor"].contains("Section Severity")) {
                                severity(jsonData["Sections"][i]["Section Descriptor"]["Section Severity"]);
                            }

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("IPSignature")) {
                                nvipSignature(jsonData["Sections"][i]["Section"]["IPSignature"]);
                            }

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("Severity")) {
                                nvSeverity(jsonData["Sections"][i]["Section"]["Severity"]);
                            }

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("Socket Number")) {
                                int nvSockNumber = jsonData["Sections"][i]["Section"]["Socket Number"];
                                nvSocketNumber(std::to_string(nvSockNumber));
                            }

                            if (jsonData["Sections"][i].contains("Section") && jsonData["Sections"][i]["Section"].contains("Device ID")) {
                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Vendor ID")) {
                                    pcieVendorID(jsonData["Sections"][i]["Section"]["Device ID"]["Vendor ID"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Device ID")) {
                                    pcieDeviceID(jsonData["Sections"][i]["Section"]["Device ID"]["Device ID"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Class Code")) {
                                    pcieClassCode(jsonData["Sections"][i]["Section"]["Device ID"]["Class Code"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Function Number")) {
                                    pcieFunctionNumber(jsonData["Sections"][i]["Section"]["Device ID"]["Function Number"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Device Number")) {
                                    pcieDeviceNumber(jsonData["Sections"][i]["Section"]["Device ID"]["Device Number"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Segment Number")) {
                                    pcieSegmentNumber(jsonData["Sections"][i]["Section"]["Device ID"]["Segment Number"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Device Bus Number")) {
                                    pcieDeviceBusNumber(jsonData["Sections"][i]["Section"]["Device ID"]["Device Bus Number"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Secondary Bus Number")) {
                                    pcieSecondaryBusNumber(jsonData["Sections"][i]["Section"]["Device ID"]["Secondary Bus Number"]);
                                }

                                if (jsonData["Sections"][i]["Section"]["Device ID"].contains("Slot Number")) {
                                    int pcieSlotNum = jsonData["Sections"][i]["Section"]["Device ID"]["Slot Number"];
                                    pcieSlotNumber(std::to_string(pcieSlotNum));
                                }
                            }
                        }
                    }
                }
            }
        }
    }


  private:
    /** @Dump file name */
    fs::path file;
};

} // namespace faultLog
} // namespace dump
} // namespace phosphor
