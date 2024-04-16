#pragma once

#include <sdbusplus/bus.hpp>
#include "xyz/openbmc_project/Dump/DebugMode/server.hpp"
#include <xyz/openbmc_project/State/ServiceReady/server.hpp>

namespace phosphor
{
namespace dump
{
namespace retimer
{

using DebugModeIface =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::Dump::server::DebugMode>;
using ServiceReadyIface =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::server::ServiceReady>;

/** @class DebugMode
 *  @brief Object for implying the DebugMode state of retimer.
 *  @details DebugMode interface indicates the state of debug mode being true or false.
 */
class DebugMode : virtual public DebugModeIface, virtual public ServiceReadyIface
{
  public:
    DebugMode() = delete;
    DebugMode(const DebugMode&) = default;
    DebugMode& operator=(const DebugMode&) = delete;
    DebugMode(DebugMode&&) = delete;
    DebugMode& operator=(DebugMode&&) = delete;
    virtual ~DebugMode() = default;

    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     */
    DebugMode(sdbusplus::bus::bus& bus, const char* path) :
        DebugModeIface(bus, path),
        ServiceReadyIface(bus, path)
    {
      DebugModeIface::debugMode(false);
      ServiceReadyIface::state(States::Disabled);
    }

    bool debugMode() const override;
    bool debugMode(bool value) override;
};

} // namespace retimer
} // namespace dump
} // namespace phosphor
