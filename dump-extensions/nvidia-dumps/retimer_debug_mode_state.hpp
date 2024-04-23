/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
