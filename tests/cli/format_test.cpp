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
