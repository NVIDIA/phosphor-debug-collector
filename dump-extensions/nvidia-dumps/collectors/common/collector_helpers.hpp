/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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

#include <string>

namespace phosphor
{
namespace dump
{
namespace collectors
{

/** Build the canonical dump stem "obmcdump_<entryId>_<epoch>", the exact shape
 *  PDC's inotify watch / createEntry() requires to register the entry. */
std::string obmcdumpStem(const std::string& entryId);

/** tar+xz <stageDir> into <outDir>/<stem>.tar.xz and remove the staging dir.
 *  PDC's watch on <outDir> then marks the entry Completed. */
bool makeTarball(const std::string& outDir, const std::string& stageDir);

/** Remove a staging dir without archiving (early-return-before-tar cleanup).
 *  Best-effort: errors are logged at debug level and ignored. */
void cleanupStage(const std::string& stageDir);

} // namespace collectors
} // namespace dump
} // namespace phosphor
