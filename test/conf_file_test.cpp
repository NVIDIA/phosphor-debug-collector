// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "i2c-dump-tools/conf_file.hpp"

#include <sstream>

#include <gtest/gtest.h>

namespace conf = phosphor::dump::conf;

TEST(ConfFile, ReadsKeyValues)
{
    std::istringstream in(
        "# comment\n"
        "\n"
        "ControlBus=1\n"
        "  DataBus = 3  \n"
        "ControlAddress=0x45\r\n"
        "not a pair\n"
        "Empty=\n");
    auto e = conf::read(in);
    ASSERT_EQ(e.size(), 5U);
    EXPECT_EQ(e[0].line, 3);
    EXPECT_EQ(e[0].key, "ControlBus");
    EXPECT_EQ(e[0].value, "1");
    EXPECT_TRUE(e[0].ok);
    EXPECT_EQ(e[1].key, "DataBus");
    EXPECT_EQ(e[1].value, "3");
    EXPECT_EQ(e[2].value, "0x45");
    EXPECT_FALSE(e[3].ok);
    EXPECT_EQ(e[3].line, 6);
    EXPECT_TRUE(e[4].ok);
    EXPECT_EQ(e[4].value, "");
}

TEST(ConfFile, ParseUnsigned)
{
    EXPECT_EQ(conf::parseUnsigned("12"), 12U);
    EXPECT_EQ(conf::parseUnsigned("0x4f"), 0x4fU);
    EXPECT_EQ(conf::parseUnsigned("0X4F"), 0x4fU);
    EXPECT_FALSE(conf::parseUnsigned(""));
    EXPECT_FALSE(conf::parseUnsigned("0x"));
    EXPECT_FALSE(conf::parseUnsigned("-1"));
    EXPECT_FALSE(conf::parseUnsigned("zz"));
    EXPECT_FALSE(conf::parseUnsigned("12a"));
    EXPECT_FALSE(conf::parseUnsigned("99999999999"));
}
