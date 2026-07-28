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

// PCore selector list handling, shared by the dump manager (which validates a
// CreateDump request) and by cpu-diagnostic-dump (which consumes the validated
// list). Header-only and dependency-free on purpose: the collector links
// neither phosphor-logging nor the dump manager sources.
//
// The selector list is a Redfish-level concept. It reaches the dump manager as
// a comma-separated string in the PCoreIds AdditionalData key and reaches the
// collector as the -c argument. No D-Bus interface carries the list.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phosphor::dump::pcore
{

/** @brief Interface pldmd publishes on the per-CPU numeric effecter object. */
constexpr auto pcoreDumpInterface = "com.nvidia.PCoreDump";

/** @brief Method on pcoreDumpInterface that dispatches one PCore dump.
 *
 *  Dispatch only: it returns once the set is handed to the device, never once
 *  a payload has been collected. pldmd keeps no collection state and does not
 *  serialise callers, so the per-terminus lock this collector takes is the
 *  only thing keeping two collections off one staging file.
 */
constexpr auto createDumpMethod = "CreateDump";

/** @brief Bus name owning the effecter objects. */
constexpr auto pldmService = "xyz.openbmc_project.PLDM";

/** @brief Subtree the effecter objects live under. */
constexpr auto controlRoot = "/xyz/openbmc_project/control";

/** @brief Static configuration mapping CPU instances to PLDM termini. */
constexpr auto pldmStaticConfigPath =
    "/usr/share/pldm/pldm_static_configuration.json";

/** @brief AdditionalData key carrying the selector list. */
constexpr auto pcoreIdsKey = "PCoreIds";

/** @brief DiagnosticType value that selects this collection mode. */
constexpr auto pcoreDiagnosticType = "PCoreDump";

/** @brief The -c argument that stands for "every PCore of this CPU". */
constexpr auto allSelectorsToken = "all";

/** @brief Outcome of parsing a PCoreIds selector list.
 *
 *  An empty @c ids with no @c badToken means "every PCore", which is what an
 *  absent or empty PCoreIds requests.
 */
struct SelectorList
{
    /** @brief Ascending, de-duplicated selectors. Empty means all PCores. */
    std::vector<uint64_t> ids;

    /** @brief Set when parsing failed; holds the offending token. */
    std::optional<std::string> badToken;

    /** @brief True when the list is usable. */
    explicit operator bool() const
    {
        return !badToken.has_value();
    }
};

namespace detail
{

/** @brief Drop leading and trailing ASCII blanks from a token. */
inline std::string_view trim(std::string_view token)
{
    constexpr std::string_view blanks = " \t\r\n";
    const auto begin = token.find_first_not_of(blanks);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    return token.substr(begin, token.find_last_not_of(blanks) - begin + 1);
}

} // namespace detail

/** @brief Parse and bounds-check a comma-separated PCore selector list.
 *
 *  Accepts the empty string (and the literal "all") as "every PCore". Every
 *  other input must be a comma-separated list of decimal selectors, each
 *  within [minId, maxId]. Duplicates are collapsed rather than rejected, so a
 *  Redfish array of [2,2,2] triggers one collection. A non-numeric, empty or
 *  out-of-range token fails the whole list and names itself in @c badToken;
 *  the caller maps that to InvalidArgument.
 *
 *  @param[in] csv - The selector list as it arrives over D-Bus or argv.
 *  @param[in] minId - Lowest selector the device accepts (PDR minSettable).
 *  @param[in] maxId - Highest selector the device accepts (PDR maxSettable).
 *
 *  @return Parsed selectors, or a list naming the token that failed.
 */
inline SelectorList parseSelectors(std::string_view csv, uint64_t minId,
                                   uint64_t maxId)
{
    SelectorList out;

    const auto trimmed = detail::trim(csv);
    if (trimmed.empty() || trimmed == allSelectorsToken)
    {
        return out;
    }

    for (size_t pos = 0; pos <= trimmed.size();)
    {
        const auto comma = trimmed.find(',', pos);
        const auto end =
            comma == std::string_view::npos ? trimmed.size() : comma;
        const auto token = detail::trim(trimmed.substr(pos, end - pos));

        uint64_t value = 0;
        const auto* first = token.data();
        const auto* last = first + token.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);

        if (token.empty() || ec != std::errc{} || ptr != last ||
            value < minId || value > maxId)
        {
            out.badToken = std::string(token);
            out.ids.clear();
            return out;
        }

        out.ids.push_back(value);

        if (comma == std::string_view::npos)
        {
            break;
        }
        pos = comma + 1;
    }

    std::sort(out.ids.begin(), out.ids.end());
    out.ids.erase(std::unique(out.ids.begin(), out.ids.end()), out.ids.end());
    return out;
}

/** @brief Expand a parsed list to the concrete selectors to collect.
 *
 *  An empty list means every selector the device advertises.
 *
 *  @param[in] ids - Parsed selectors, possibly empty.
 *  @param[in] minId - Lowest selector the device accepts.
 *  @param[in] maxId - Highest selector the device accepts.
 *
 *  @return The selectors to trigger, in ascending order.
 */
inline std::vector<uint64_t> expandSelectors(const std::vector<uint64_t>& ids,
                                             uint64_t minId, uint64_t maxId)
{
    if (!ids.empty())
    {
        return ids;
    }

    std::vector<uint64_t> all;
    for (uint64_t id = minId; id <= maxId; ++id)
    {
        all.push_back(id);
    }
    return all;
}

/** @brief Render selectors for the collector's -c argument.
 *
 *  @param[in] ids - Selectors to render; empty renders as "all".
 *
 *  @return "all", or a comma-separated decimal list.
 */
inline std::string formatSelectors(const std::vector<uint64_t>& ids)
{
    if (ids.empty())
    {
        return allSelectorsToken;
    }

    std::string out;
    for (const auto id : ids)
    {
        if (!out.empty())
        {
            out += ',';
        }
        out += std::to_string(id);
    }
    return out;
}

} // namespace phosphor::dump::pcore
