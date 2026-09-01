/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace phosphor::dump::nsm
{
namespace detail
{

inline constexpr std::string_view networkAdapters = "/NetworkAdapters/";

inline bool isDecimalIndex(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    for (const char digit : value)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
    }
    return true;
}

inline bool pathHasLegacySelectorSegment(std::string_view path,
                                         std::string_view selector)
{
    if (selector.empty())
    {
        return false;
    }

    std::size_t segmentStart = 0;
    while (segmentStart <= path.size())
    {
        const auto segmentEnd = path.find('/', segmentStart);
        const auto segment =
            path.substr(segmentStart, segmentEnd == std::string_view::npos
                                          ? std::string_view::npos
                                          : segmentEnd - segmentStart);
        if (segment == selector ||
            (segment.size() > selector.size() && segment.ends_with(selector) &&
             segment[segment.size() - selector.size() - 1] == '_'))
        {
            return true;
        }

        if (segmentEnd == std::string_view::npos)
        {
            break;
        }
        segmentStart = segmentEnd + 1;
    }
    return false;
}

inline std::size_t longestStemOverlap(std::string_view chassisStem,
                                      std::string_view leafStem)
{
    std::size_t tokenStart = 0;
    while (tokenStart < chassisStem.size())
    {
        const auto suffix = chassisStem.substr(tokenStart);
        if (leafStem.starts_with(suffix) && (leafStem.size() == suffix.size() ||
                                             leafStem[suffix.size()] == '_'))
        {
            return suffix.size();
        }

        const auto separator = chassisStem.find('_', tokenStart);
        if (separator == std::string_view::npos)
        {
            break;
        }
        tokenStart = separator + 1;
    }
    return 0;
}

} // namespace detail

/**
 * Derive a selector from an NSM inventory path for an EM-provided DeviceType.
 *
 * This naming transform is a debug-collector compatibility convention; it is
 * not an EM or NSM schema contract. NetworkAdapter selectors use the parent
 * chassis index while preserving descriptive stems; additional generic
 * adapters retain a local sub-index. Unparseable paths fall back to the leaf.
 */
inline std::string deviceSelectorFromPath(std::string_view path)
{
    const auto adaptersPos = path.find(detail::networkAdapters);
    if (adaptersPos == std::string_view::npos)
    {
        const auto start = path.rfind('/');
        return std::string(path.substr(start + 1));
    }

    const auto chassisStart = adaptersPos == 0
                                  ? std::string_view::npos
                                  : path.rfind('/', adaptersPos - 1);
    const auto leafStart = adaptersPos + detail::networkAdapters.size();
    const auto leafEnd = path.find('/', leafStart);
    const auto leaf = path.substr(leafStart, leafEnd == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : leafEnd - leafStart);
    if (chassisStart == std::string_view::npos)
    {
        return std::string(leaf);
    }

    const auto chassis =
        path.substr(chassisStart + 1, adaptersPos - chassisStart - 1);
    const auto chassisSeparator = chassis.rfind('_');
    const auto leafSeparator = leaf.rfind('_');
    if (chassisSeparator == std::string_view::npos ||
        leafSeparator == std::string_view::npos)
    {
        return std::string(leaf);
    }

    const auto chassisStem = chassis.substr(0, chassisSeparator);
    const auto chassisIndex = chassis.substr(chassisSeparator + 1);
    const auto leafStem = leaf.substr(0, leafSeparator);
    const auto leafIndex = leaf.substr(leafSeparator + 1);
    if (!detail::isDecimalIndex(chassisIndex) ||
        !detail::isDecimalIndex(leafIndex))
    {
        return std::string(leaf);
    }

    if (("_" + std::string(leafStem) + "_")
            .contains("_" + std::string(chassisStem) + "_"))
    {
        std::string selector(leafStem);
        selector += '_';
        selector += chassisIndex;
        return selector;
    }

    const auto overlap = detail::longestStemOverlap(chassisStem, leafStem);
    const auto leafSuffix =
        overlap == 0                 ? leafStem
        : overlap == leafStem.size() ? std::string_view{}
                                     : leafStem.substr(overlap + 1);

    std::string selector(chassisStem);
    if (!leafSuffix.empty())
    {
        selector += '_';
        selector += leafSuffix;
    }
    selector += '_';
    selector += chassisIndex;
    if (leafIndex != "0")
    {
        selector += '_';
        selector += leafIndex;
    }
    return selector;
}

enum class DeviceSelectorMatch
{
    None,
    Legacy,
    Exact,
};

/** Classify a path match so callers can prefer exact selectors. */
inline DeviceSelectorMatch deviceSelectorMatch(std::string_view path,
                                               std::string_view selector)
{
    if (selector.empty())
    {
        return DeviceSelectorMatch::None;
    }

    const auto resolved = deviceSelectorFromPath(path);
    if (resolved == selector)
    {
        return DeviceSelectorMatch::Exact;
    }

    return detail::pathHasLegacySelectorSegment(path, selector)
               ? DeviceSelectorMatch::Legacy
               : DeviceSelectorMatch::None;
}

/** Match an EM selector or a legacy platform-prefixed inventory leaf. */
inline bool pathMatchesDeviceSelector(std::string_view path,
                                      std::string_view selector)
{
    return deviceSelectorMatch(path, selector) != DeviceSelectorMatch::None;
}

} // namespace phosphor::dump::nsm
