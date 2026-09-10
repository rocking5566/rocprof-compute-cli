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

#include "cli/format.h"
#include <gtest/gtest.h>
#include <string>
#include "cli/queries.h"

namespace
{
rcv::Digest smallDigest()
{
    rcv::Digest d;
    d.type_names = {"NONE", "SMEM", "SALU", "VMEM"};
    d.stall_reason_names = {"NONE", "Inst Fetch", "ALU Dep", "Waitcnt"};

    rcv::LineDigest a;
    a.index = 0;
    a.addr = 9984;
    a.inst = "v_nop";
    a.type = 2;
    a.hitcount = 4;
    a.latency = 100;
    a.stall = 40;
    a.stallreasons = {0, 0, 10, 30};

    rcv::LineDigest b;
    b.index = 1;
    b.addr = 9988;
    b.inst = "global_load_b32 v0, v[2:3]";
    b.type = 3;
    b.hitcount = 4;
    b.latency = 500;
    b.stall = 400;
    b.idle = 20;
    b.hidden_stall = 350;
    b.stallreasons = {0, 0, 0, 400};

    d.lines = {a, b};
    d.meta.total_lines = 2;
    d.occupancy.bins = 4;
    d.occupancy.t0 = 0;
    d.occupancy.t1 = 400;
    d.occupancy.total = {1.0, 3.0, 2.5, 0.5};
    d.occupancy.per_se[0] = {1.0, 2.0, 1.5, 0.5};
    d.occupancy.per_se[1] = {0.0, 1.0, 1.0, 0.0};
    return d;
}
} // namespace

TEST(Format, TableAlignsNumericColumnsRight)
{
    const auto out = rcv::renderTable(
        {
            "name", "cycles"
    },
        {{"alpha", "5"}, {"b", "1234"}}
    );

    // Header, then one row per entry. Numeric column right-aligned under its header.
    EXPECT_NE(out.find("name   cycles"), std::string::npos) << out;
    EXPECT_NE(out.find("alpha       5"), std::string::npos) << out;
    EXPECT_NE(out.find("b        1234"), std::string::npos) << out;
}

TEST(Format, TableHandlesEmptyRows)
{
    const auto out = rcv::renderTable({"a", "b"}, {});
    EXPECT_NE(out.find("a"), std::string::npos);
    EXPECT_NE(out.find("b"), std::string::npos);
}

TEST(Format, PercentGuardsAgainstZeroDenominator)
{
    EXPECT_EQ(rcv::formatPercent(1, 4), "25.0%");
    EXPECT_EQ(rcv::formatPercent(0, 0), "-");
}

TEST(Format, SummaryReportsSourceAttributionCoverage)
{
    rcv::Digest d;
    d.meta.total_lines = 2521;
    d.meta.lines_with_source = 1;
    d.meta.gfxv = "navi";
    d.meta.wave_count = 128;
    d.meta.hidden_latency_available = true;
    d.type_names = {"NONE"};
    d.stall_reason_names = {"NONE"};

    const auto out = rcv::renderSummary(d);
    EXPECT_NE(out.find("1/2521"), std::string::npos) << out;
    EXPECT_NE(out.find("-g"), std::string::npos) << "summary must explain WHY source view is empty, not just show 0%";
}

TEST(Format, SummaryWarnsWhenHiddenLatencyMissing)
{
    rcv::Digest d;
    d.meta.total_lines = 1;
    d.meta.hidden_latency_available = false;
    d.type_names = {"NONE"};
    d.stall_reason_names = {"NONE"};

    const auto out = rcv::renderSummary(d);
    EXPECT_NE(out.find("hidden latency"), std::string::npos) << out;
}

TEST(Format, DominantStallReasonPicksLargest)
{
    const auto d = smallDigest();
    EXPECT_EQ(rcv::dominantStallReason(d.lines[0]), 3);

    rcv::LineDigest quiet;
    quiet.stallreasons = {0, 0, 0, 0};
    EXPECT_EQ(rcv::dominantStallReason(quiet), -1);
}

TEST(Format, HotspotTableNamesTheDominantStallReason)
{
    const auto d = smallDigest();
    const auto rows = rcv::hotspot(d, rcv::GroupBy::Asm, rcv::SortKey::Exposed, 10);
    const auto out = rcv::renderHotspot(d, rows);

    EXPECT_NE(out.find("exposed"), std::string::npos) << out;
    EXPECT_NE(out.find("v_nop"), std::string::npos) << out;
    // index 0 is exposed 100; index 1 is exposed 520-350=170, so index 1 leads.
    const auto pos_load = out.find("global_load_b32");
    const auto pos_nop = out.find("v_nop");
    EXPECT_LT(pos_load, pos_nop) << out;
}

TEST(Format, AsmTableShowsPerLineStallReason)
{
    const auto d = smallDigest();
    const auto out = rcv::renderAsm(d, rcv::asmRange(d, 0, 1));

    EXPECT_NE(out.find("Waitcnt"), std::string::npos) << out;
    EXPECT_NE(out.find("global_load_b32"), std::string::npos) << out;
    EXPECT_NE(out.find("9984"), std::string::npos) << out;
}

TEST(Format, OccupancyRendersAllSesAndOneSe)
{
    const auto d = smallDigest();

    const auto all = rcv::renderOccupancy(d, -1);
    EXPECT_NE(all.find("3.0"), std::string::npos) << all;

    const auto se1 = rcv::renderOccupancy(d, 1);
    EXPECT_NE(se1.find("SE1"), std::string::npos) << se1;
}

TEST(Format, OccupancyReportsMissingSe)
{
    const auto d = smallDigest();
    const auto out = rcv::renderOccupancy(d, 99);
    EXPECT_NE(out.find("no occupancy data"), std::string::npos) << out;
}

TEST(Format, JsonRenderersProduceArrays)
{
    const auto d = smallDigest();
    const auto rows = rcv::hotspot(d, rcv::GroupBy::Asm, rcv::SortKey::Exposed, 10);

    EXPECT_TRUE(rcv::hotspotJson(rows).is_array());
    EXPECT_TRUE(rcv::asmJson(d, rcv::asmRange(d, 0, 1)).is_array());
    EXPECT_TRUE(rcv::occupancyJson(d, -1).is_object());
}
