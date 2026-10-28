// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// The board configuration files of the I2C mailbox tools: key=value lines;
// blank lines and lines starting with # are skipped, spaces around the key
// and the value are dropped.

#pragma once

#include <charconv>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace phosphor::dump::conf
{

struct Entry
{
    int line = 0;
    std::string key;
    std::string value;
    // False for a line without '='; key then holds the whole line.
    bool ok = false;
};

inline std::string_view trim(std::string_view s)
{
    constexpr std::string_view space = " \t\r";
    const auto b = s.find_first_not_of(space);
    if (b == std::string_view::npos)
    {
        return {};
    }
    return s.substr(b, s.find_last_not_of(space) - b + 1);
}

// Decimal, or hexadecimal with 0x.
inline std::optional<unsigned> parseUnsigned(std::string_view s)
{
    int base = 10;
    if (s.starts_with("0x") || s.starts_with("0X"))
    {
        s.remove_prefix(2);
        base = 16;
    }
    unsigned v = 0;
    const auto* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, v, base);
    if (s.empty() || ec != std::errc{} || p != end)
    {
        return std::nullopt;
    }
    return v;
}

inline std::vector<Entry> read(std::istream& in)
{
    std::vector<Entry> entries;
    int n = 0;
    for (std::string text; std::getline(in, text);)
    {
        ++n;
        const auto l = trim(text);
        if (l.empty() || l.starts_with('#'))
        {
            continue;
        }
        const auto eq = l.find('=');
        if (eq == std::string_view::npos)
        {
            entries.push_back({n, std::string(l), {}, false});
            continue;
        }
        entries.push_back({n, std::string(trim(l.substr(0, eq))),
                           std::string(trim(l.substr(eq + 1))), true});
    }
    return entries;
}

} // namespace phosphor::dump::conf
