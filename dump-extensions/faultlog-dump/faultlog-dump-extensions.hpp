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

#include "dump-extensions.hpp"
#include "dump_manager.hpp"

namespace phosphor
{
namespace dump
{
/**
 * @brief load the faultLog dump extension
 *
 * @param[in] bus - Bus to attach to
 * @param[out] dumpMgrList - list dump manager objects.
 *
 */
void loadExtensionsFaultLog(sdbusplus::bus::bus& bus,
                            DumpManagerList& dumpMgrList);
} // namespace dump
} // namespace phosphor
