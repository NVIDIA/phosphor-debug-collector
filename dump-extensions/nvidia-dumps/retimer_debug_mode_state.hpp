/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "xyz/openbmc_project/Dump/DebugMode/server.hpp"
#include "xyz/openbmc_project/State/ServiceReady/server.hpp"

#include <sdbusplus/bus.hpp>

constexpr auto SWITCH_INTERFACE = "xyz.openbmc_project.Inventory.Item.Switch";
constexpr auto RETIMER_SWITCHES_BASE_PATH =
    "/xyz/openbmc_project/inventory/system/fabrics";

namespace phosphor
{
namespace dump
{
namespace retimer
{

using DebugModeIface = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Dump::server::DebugMode>;
using ServiceReadyIface = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::ServiceReady>;

/** @class State
 *  @brief Object for implying the state of retimer.
 *  @details DebugMode interface indicates the state of debug mode being true or
 * false. ServiceReady interface indicates the service state which will be read
 * by CSM. Switch interface maintains Vendor ID information for retimer, the
 * property will be set by nvidia-retimer-app.
 */
class State : virtual public DebugModeIface, virtual public ServiceReadyIface
{
  public:
    State() = delete;
    State(const State&) = default;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    virtual ~State() = default;

    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     */
    State(sdbusplus::bus::bus& bus, const char* path) :
        DebugModeIface(bus, path), ServiceReadyIface(bus, path)
    {
        DebugModeIface::debugMode(false);
        ServiceReadyIface::state(States::Disabled);

        // fetch retimer vendor id from gpuMgr or NSM during start up
        std::string vendorId;
        try
        {
            std::string objectPath = getDBusObject(bus,
                                                   RETIMER_SWITCHES_BASE_PATH);
            std::string service = getService(bus, objectPath.c_str(),
                                             SWITCH_INTERFACE);
            auto method =
                bus.new_method_call(service.c_str(), objectPath.c_str(),
                                    "org.freedesktop.DBus.Properties", "Get");
            method.append(SWITCH_INTERFACE, "VendorId");
            auto reply = bus.call(method);
            std::variant<std::string> propertyValue;
            reply.read(propertyValue);
            vendorId = std::get<std::string>(propertyValue);
        }
        catch (const std::exception& e)
        {
            // ignore the error when the resource service is also starting
        }
        if (!vendorId.empty())
        {
            retimerVendorId = vendorId;
        }
        listenRetimerVendorIdEvents(bus);
    }

    bool debugMode() const override;
    bool debugMode(bool value) override;

    /** @brief Get retimer vendor id.
     *  @return retimerVendorId
     */
    std::string getVendorId() const;

  private:
    /** @brief a string for retimer vendor id*/
    std::string retimerVendorId;

    /** @brief pointer for signal match object*/
    std::unique_ptr<sdbusplus::bus::match_t> switchObjectAddedMatch;

    /** @brief Get the resource dbus object that populates retimer vendor id.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] rootPath - Prefix of the resource dbus object path that has
     * the retimer info.
     *  @return service name - The target resouce service name.
     */
    std::string getDBusObject(sdbusplus::bus::bus& bus,
                              const std::string& rootPath);

    /** @brief Get the resource service that populates retimer vendor id.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Dbus object path that has the retimer info.
     *  @param[in] interface - interface that has the retimer info.
     *  @return service name - The target resouce service name.
     */
    std::string getService(sdbusplus::bus::bus& bus, const char* path,
                           const char* interface) const;

    /** @brief Starts listener for retimer property changing events.
     *  @param[in] bus - Bus to attach to.
     */
    void listenRetimerVendorIdEvents(sdbusplus::bus::bus& bus);

    /** @brief Callback function for listenRetimerVendorIdEvents().
     *         Set retimerVendorId to the captured value if retimerVendorId
     *         is empty.
     *  @param[in] m - message captured by event listener.
     */
    void switchObjectCallback(sdbusplus::message::message& m);
};

} // namespace retimer
} // namespace dump
} // namespace phosphor
