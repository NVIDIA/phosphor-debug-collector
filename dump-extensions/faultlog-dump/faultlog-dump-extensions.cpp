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
#include "config.h"

#include "faultlog-dump-extensions.hpp"

#include "dump-extensions.hpp"
#include "dump_manager_faultlog.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <sdbusplus/bus.hpp>

#include "dump-extensions/faultlog-dump/faultlog_dump_config.h"

namespace phosphor
{
namespace dump
{

void loadExtensionsFaultLog(sdbusplus::bus::bus& bus, DumpManagerList& dumpList)
{
    using namespace phosphor::logging;
    using InternalFailure =
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

    sd_event* event = nullptr;
    auto rc = sd_event_default(&event);
    if (rc < 0)
    {
        log<level::ERR>(
            "FaultLog dump: Error occurred during the sd_event_default",
            entry("RC=%d", rc));
        report<InternalFailure>();
        return;
    }
    phosphor::dump::EventPtr eventP{event};
    event = nullptr;

    std::filesystem::create_directories(FAULTLOG_DUMP_PATH);

    dumpList.push_back(std::make_unique<phosphor::dump::faultLog::Manager>(
        bus, eventP, FAULTLOG_DUMP_OBJPATH, FAULTLOG_DUMP_OBJ_ENTRY,
        FAULTLOG_DUMP_PATH));
}
} // namespace dump
} // namespace phosphor
