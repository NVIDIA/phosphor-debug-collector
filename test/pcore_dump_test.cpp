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

// Selector handling for the PCoreDump diagnostic type. This is the contract
// the dump manager validates a CreateDump request against and the contract
// cpu-diagnostic-dump consumes on the other side of the fork, so the two ends
// are tested through the same header they both include.

#include "dump-extensions/nvidia-dumps/pcore_selectors.hpp"

#include <gtest/gtest.h>

namespace
{

namespace pcore = phosphor::dump::pcore;

// Live PDR of the reference platform: OOBPcoreDump accepts 1 through 6.
constexpr uint64_t minId = 1;
constexpr uint64_t maxId = 6;

pcore::SelectorList parse(std::string_view csv)
{
    return pcore::parseSelectors(csv, minId, maxId);
}

TEST(PCoreSelectors, SingleSelectorParses)
{
    const auto list = parse("3");
    ASSERT_TRUE(list);
    EXPECT_EQ(list.ids, std::vector<uint64_t>({3}));
}

TEST(PCoreSelectors, ExplicitSubsetKeepsOnlyThatSubset)
{
    const auto list = parse("1,3");
    ASSERT_TRUE(list);
    EXPECT_EQ(list.ids, std::vector<uint64_t>({1, 3}));
}

TEST(PCoreSelectors, AbsentListMeansAllPCores)
{
    // An omitted PCoreIds reaches the dump manager as an empty string.
    const auto list = parse("");
    ASSERT_TRUE(list);
    EXPECT_TRUE(list.ids.empty());
    EXPECT_EQ(pcore::expandSelectors(list.ids, minId, maxId),
              std::vector<uint64_t>({1, 2, 3, 4, 5, 6}));
}

TEST(PCoreSelectors, AllTokenMeansAllPCores)
{
    const auto list = parse(pcore::allSelectorsToken);
    ASSERT_TRUE(list);
    EXPECT_TRUE(list.ids.empty());
}

TEST(PCoreSelectors, DuplicatesCollapseToOneTrigger)
{
    const auto list = parse("2,2,2");
    ASSERT_TRUE(list);
    EXPECT_EQ(list.ids, std::vector<uint64_t>({2}));
}

TEST(PCoreSelectors, OutOfOrderInputIsSorted)
{
    const auto list = parse("5,1,3");
    ASSERT_TRUE(list);
    EXPECT_EQ(list.ids, std::vector<uint64_t>({1, 3, 5}));
}

TEST(PCoreSelectors, BelowMinimumIsRejected)
{
    const auto list = parse("0");
    EXPECT_FALSE(list);
    EXPECT_EQ(list.badToken, "0");
    EXPECT_TRUE(list.ids.empty());
}

TEST(PCoreSelectors, AboveMaximumIsRejected)
{
    const auto list = parse("7");
    EXPECT_FALSE(list);
    EXPECT_EQ(list.badToken, "7");
}

TEST(PCoreSelectors, OneBadElementRejectsTheWholeList)
{
    // The mixed case matters: a partially valid list must not collect its
    // valid half and silently drop the rest.
    const auto list = parse("1,7");
    EXPECT_FALSE(list);
    EXPECT_EQ(list.badToken, "7");
    EXPECT_TRUE(list.ids.empty());
}

TEST(PCoreSelectors, NonNumericTokenIsRejected)
{
    EXPECT_FALSE(parse("two"));
    EXPECT_FALSE(parse("1,two"));
    EXPECT_FALSE(parse("0x2"));
    EXPECT_FALSE(parse("2.0"));
    EXPECT_FALSE(parse("-1"));
}

TEST(PCoreSelectors, EmptyTokenIsRejected)
{
    EXPECT_FALSE(parse(","));
    EXPECT_FALSE(parse("1,"));
    EXPECT_FALSE(parse(",1"));
    EXPECT_FALSE(parse("1,,3"));
}

TEST(PCoreSelectors, SurroundingBlanksAreTolerated)
{
    const auto list = parse(" 1, 3 ");
    ASSERT_TRUE(list);
    EXPECT_EQ(list.ids, std::vector<uint64_t>({1, 3}));
}

TEST(PCoreSelectors, BoundsComeFromTheDeviceNotTheDefault)
{
    // A device advertising a single selector must reject everything else,
    // whatever the reference platform happens to allow.
    const auto list = pcore::parseSelectors("2", 2, 2);
    ASSERT_TRUE(list);
    EXPECT_EQ(list.ids, std::vector<uint64_t>({2}));
    EXPECT_FALSE(pcore::parseSelectors("1", 2, 2));
    EXPECT_FALSE(pcore::parseSelectors("3", 2, 2));
}

TEST(PCoreSelectors, ExpandKeepsAnExplicitList)
{
    EXPECT_EQ(pcore::expandSelectors({4}, minId, maxId),
              std::vector<uint64_t>({4}));
}

TEST(PCoreSelectors, FormatRendersTheCollectorArgument)
{
    EXPECT_EQ(pcore::formatSelectors({}), pcore::allSelectorsToken);
    EXPECT_EQ(pcore::formatSelectors({3}), "3");
    EXPECT_EQ(pcore::formatSelectors({1, 3}), "1,3");
}

TEST(PCoreSelectors, NormalisedListSurvivesTheHandoff)
{
    // The dump manager normalises what it parsed and passes it to the
    // collector as -c, which parses it again. Both must see the same list.
    const auto atManager = parse("3,1,3");
    ASSERT_TRUE(atManager);

    const auto onArgv = pcore::formatSelectors(atManager.ids);
    EXPECT_EQ(onArgv, "1,3");

    const auto atCollector = parse(onArgv);
    ASSERT_TRUE(atCollector);
    EXPECT_EQ(atCollector.ids, atManager.ids);
}

} // namespace
