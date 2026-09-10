// MIT License
//
// Copyright (c) 2024-2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "cli/queries.h"
#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
// A hand-built digest. No trace needed: the query layer is pure data.
rcv::Digest makeDigest()
{
    rcv::Digest d;
    d.type_names = {"NONE", "SMEM", "SALU", "VMEM"};
    d.stall_reason_names = {"NONE", "Inst Fetch", "ALU Dep", "Waitcnt"};

    auto line = [](int index,
                   const char* inst,
                   const char* src,
                   int type,
                   int64_t lat,
                   int64_t stall,
                   int64_t idle,
                   int64_t hidden_stall)
    {
        rcv::LineDigest l;
        l.index = index;
        l.inst = inst;
        l.cppline = src;
        l.type = type;
        l.hitcount = 1;
        l.latency = lat;
        l.stall = stall;
        l.idle = idle;
        l.hidden_stall = hidden_stall;
        l.stallreasons = std::vector<int64_t>(4, 0);
        l.stallreasons[3] = stall; // attribute all stall to "Waitcnt"
        return l;
    };

    // index 0: total 100, hidden 0   -> exposed 100
    // index 1: total 500, hidden 450 -> exposed  50  (big but mostly hidden)
    // index 2: total 200, hidden 0   -> exposed 200  (the real hotspot)
    d.lines.push_back(line(0, "v_nop", "a.cpp:1", 2, 100, 0, 0, 0));
    d.lines.push_back(line(1, "global_load_b32", "a.cpp:2", 3, 500, 450, 0, 450));
    d.lines.push_back(line(2, "v_fma_f32", "a.cpp:1 -> b.cpp:9", 2, 150, 50, 50, 0));
    d.meta.total_lines = 3;
    d.meta.lines_with_source = 3;
    return d;
}
} // namespace

TEST(Queries, ExposedSortRanksAboveRawTotal)
{
    const auto d = makeDigest();
    const auto rows = rcv::hotspot(d, rcv::GroupBy::Asm, rcv::SortKey::Exposed, 10);

    ASSERT_EQ(rows.size(), 3u);
    // index 1 has the largest total (500) but is almost entirely hidden, so it
    // must not lead. This is the whole point of defaulting to exposed.
    EXPECT_EQ(rows[0].index, 2);
    EXPECT_EQ(rows[0].exposed(), 200);
    EXPECT_EQ(rows[1].index, 0);
    EXPECT_EQ(rows[2].index, 1);
    EXPECT_EQ(rows[2].exposed(), 50);
}

TEST(Queries, TotalSortRanksDifferently)
{
    const auto d = makeDigest();
    const auto rows = rcv::hotspot(d, rcv::GroupBy::Asm, rcv::SortKey::Total, 10);

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].index, 1);
    EXPECT_EQ(rows[0].total(), 500);
}

TEST(Queries, TopNTruncates)
{
    const auto d = makeDigest();
    const auto rows = rcv::hotspot(d, rcv::GroupBy::Asm, rcv::SortKey::Exposed, 2);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].index, 2);
    EXPECT_EQ(rows[1].index, 0);
}

// Inline chains replicate the full cost to every frame (asmcode.cpp:225-227).
// Dividing would disagree with the GUI.
TEST(Queries, SourceGroupingReplicatesCostAcrossInlineFrames)
{
    const auto d = makeDigest();
    const auto rows = rcv::hotspot(d, rcv::GroupBy::Source, rcv::SortKey::Total, 10);

    int64_t a1 = 0, b9 = 0;
    for (const auto& r : rows)
    {
        if (r.key == "a.cpp:1") a1 = r.total();
        if (r.key == "b.cpp:9") b9 = r.total();
    }
    // Line index 0 (total 100) and index 2 (total 200) both name a.cpp:1.
    EXPECT_EQ(a1, 300);
    // b.cpp:9 appears only in index 2's chain and receives that line's FULL
    // cost, not a divided share.
    EXPECT_EQ(b9, 200);
}

TEST(Queries, TotalsAggregateEveryLine)
{
    const auto t = rcv::totals(makeDigest());
    EXPECT_EQ(t.latency, 750);
    EXPECT_EQ(t.stall, 50 + 450);
    EXPECT_EQ(t.idle, 50);
    EXPECT_EQ(t.hidden, 450);
    EXPECT_EQ(t.total(), 800);
    EXPECT_EQ(t.exposed(), 350);
}

TEST(Queries, StallReasonBreakdownIsDescendingAndNamed)
{
    const auto rows = rcv::stallReasonBreakdown(makeDigest());
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows[0].first, "Waitcnt");
    EXPECT_EQ(rows[0].second, 500);
    for (size_t i = 1; i < rows.size(); ++i) EXPECT_LE(rows[i].second, rows[i - 1].second);
}

TEST(Queries, InstructionTypeBreakdownUsesTypeNames)
{
    const auto rows = rcv::instructionTypeBreakdown(makeDigest());
    ASSERT_FALSE(rows.empty());
    // VMEM (type 3) has total 500; SALU (type 2) has 100 + 200 = 300.
    EXPECT_EQ(rows[0].first, "VMEM");
    EXPECT_EQ(rows[0].second, 500);
    EXPECT_EQ(rows[1].first, "SALU");
    EXPECT_EQ(rows[1].second, 300);
}

TEST(Queries, AsmRangeClampsToDigestBounds)
{
    const auto d = makeDigest();
    const auto rows = rcv::asmRange(d, -5, 99);
    EXPECT_EQ(rows.size(), 3u);

    const auto middle = rcv::asmRange(d, 1, 1);
    ASSERT_EQ(middle.size(), 1u);
    EXPECT_EQ(middle[0].index, 1);
}

TEST(Queries, AsmAroundExpandsByContext)
{
    const auto d = makeDigest();
    const auto rows = rcv::asmAround(d, 1, 1);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows.front().index, 0);
    EXPECT_EQ(rows.back().index, 2);

    const auto edge = rcv::asmAround(d, 0, 1);
    ASSERT_EQ(edge.size(), 2u);
    EXPECT_EQ(edge.front().index, 0);
}

TEST(Queries, ParseRejectsUnknownKeys)
{
    EXPECT_EQ(rcv::parseSortKey("exposed"), rcv::SortKey::Exposed);
    EXPECT_EQ(rcv::parseGroupBy("asm"), rcv::GroupBy::Asm);
    EXPECT_THROW(rcv::parseSortKey("bogus"), std::invalid_argument);
    EXPECT_THROW(rcv::parseGroupBy("bogus"), std::invalid_argument);
}
