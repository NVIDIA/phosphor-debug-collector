/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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

// Dump-manager side discovery for the PCoreDump diagnostic type: resolve a
// DeviceType (CPU_<N>) to the pldmd effecter object that carries
// com.nvidia.PCoreDump, and read the selector bounds it advertises.
//
// Nothing here is hard-coded to an EID, effecter ID or terminus name.
// Resolution is static configuration (which CPU instance owns which terminus)
// plus ObjectMapper (which object carries the interface) plus the
// terminus-prefixed object name, so PDR and EID renumbering are survivable.

#include "pcore_selectors.hpp"

#include <sdbusplus/bus.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace phosphor::dump::pcore
{

/** @brief The PLDM terminus that owns one CPU package.
 *
 *  A terminus name does not identify a package on its own: a platform may put
 *  both packages behind one name and separate them only by EID.
 */
struct TerminusTarget
{
    /** @brief Terminus name, e.g. ProcessorModule_0. */
    std::string name;

    /** @brief MCTP EID of the package, negative when not configured. */
    int eid = -1;
};

/** @brief The effecter object resolved for one CPU package. */
struct EffecterTarget
{
    /** @brief PLDM terminus name owning the object, e.g. ProcessorModule_0. */
    std::string terminus;

    /** @brief MCTP EID of the package that owns the object. */
    int eid = -1;

    /** @brief Object path carrying com.nvidia.PCoreDump. */
    std::string path;

    /** @brief Lowest selector the device accepts (PDR minSettable). */
    uint64_t minId;

    /** @brief Highest selector the device accepts (PDR maxSettable). */
    uint64_t maxId;
};

/** @brief Map a CPU device type to the terminus and EID that own it.
 *
 *  Reads the PLDMTermini array of the pldm static configuration and matches
 *  the CPU_<Instance> entry.
 *
 *  @param[in] configPath - Path to the pldm static configuration JSON.
 *  @param[in] deviceType - Device type as given in CreateDump, e.g. "CPU_0".
 *
 *  @return The terminus and its EID, or nullopt when the device is not
 *          configured.
 */
std::optional<TerminusTarget> terminusForDevice(const std::string& configPath,
                                                const std::string& deviceType);

/** @brief Resolve a CPU device type to its PCore dump effecter object.
 *
 *  @param[in] bus - Bus to query ObjectMapper and pldmd on.
 *  @param[in] deviceType - Device type as given in CreateDump, e.g. "CPU_0".
 *
 *  @return The effecter object and its selector bounds, or nullopt when the
 *          device is unknown, exposes no such effecter, or advertises an
 *          unusable range.
 */
std::optional<EffecterTarget> resolveEffecter(sdbusplus::bus_t& bus,
                                              const std::string& deviceType);

} // namespace phosphor::dump::pcore
