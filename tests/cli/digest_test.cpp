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

#include "cli/digest.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "cli/digest_builder.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"

namespace
{
std::string envOr(const char* key, const char* fallback)
{
    const char* v = std::getenv(key);
    return v ? v : fallback;
}

// Minimal CSV reader for stats_ui_output_*.csv. Fields may be quoted and may
// contain commas inside quotes, so a naive split will not do.
std::vector<std::string> splitCsvRow(const std::string& row)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < row.size(); ++i)
    {
        const char c = row[i];
        if (c == '"')
        {
            if (in_quotes && i + 1 < row.size() && row[i + 1] == '"')
            {
                cur += '"';
                ++i;
            }
            else
                in_quotes = !in_quotes;
        }
        else if (c == ',' && !in_quotes)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    out.push_back(cur);
    return out;
}
} // namespace

TEST(Digest, JsonRoundTripPreservesLines)
{
    rcv::Digest d;
    d.meta.gfxip = 12;
    d.meta.gfxv = "navi";
    d.meta.total_lines = 1;
    d.type_names = {"NONE", "SMEM"};
    rcv::LineDigest line;
    line.index = 7;
    line.addr = 9984;
    line.inst = "v_nop";
    line.type = 1;
    line.hitcount = 128;
    line.latency = 300;
    line.stall = 100;
    line.idle = 50;
    line.hidden_stall = 20;
    line.stallreasons = std::vector<int64_t>(16, 0);
    line.stallreasons[2] = 99;
    d.lines.push_back(line);

    const auto restored = rcv::fromJson(rcv::toJson(d));
    ASSERT_EQ(restored.lines.size(), 1u);
    const auto& r = restored.lines[0];
    EXPECT_EQ(r.index, 7);
    EXPECT_EQ(r.addr, 9984);
    EXPECT_EQ(r.inst, "v_nop");
    EXPECT_EQ(r.latency, 300);
    EXPECT_EQ(r.stall, 100);
    EXPECT_EQ(r.idle, 50);
    EXPECT_EQ(r.hidden_stall, 20);
    EXPECT_EQ(r.stallreasons[2], 99);
    EXPECT_EQ(restored.meta.gfxv, "navi");
}

TEST(Digest, MetricsFollowSpecDefinitions)
{
    rcv::LineDigest line;
    line.latency = 300;
    line.stall = 100;
    line.idle = 50;
    line.hidden_idle = 10;
    line.hidden_stall = 20;
    line.hidden_issue = 5;

    EXPECT_EQ(line.issue(), 200);
    EXPECT_EQ(line.total(), 350);
    EXPECT_EQ(line.hidden(), 35);
    EXPECT_EQ(line.exposed(), 315);
}

// Layer 1 validation: rocprofv3 independently reports Hitcount/Latency/Stall/
// Idle for every ASM line. Exact integer equality, no tolerance.
TEST(Digest, MatchesRocprofv3StatsCsvExactly)
{
    const char* trace = std::getenv("RCV_CLI_TEST_TRACE");
    const char* csv_path = std::getenv("RCV_CLI_TEST_STATS_CSV");
    if (!trace || !csv_path) GTEST_SKIP() << "RCV_CLI_TEST_TRACE / RCV_CLI_TEST_STATS_CSV not set";

    DataStore store;
    ASSERT_TRUE(rcv::loadTrace(trace, store).ok);
    const auto digest = rcv::buildDigest(store, trace, 200);

    std::ifstream csv(csv_path);
    ASSERT_TRUE(csv.is_open()) << csv_path;

    std::string header;
    std::getline(csv, header);
    // "CodeObj","Vaddr","Instruction","Hitcount","Latency","Stall","Idle","Source"
    const int kCodeObj = 0, kVaddr = 1, kHit = 3, kLatency = 4, kStall = 5, kIdle = 6;

    size_t row_index = 0;
    std::string row;
    while (std::getline(csv, row))
    {
        if (row.empty()) continue;
        const auto fields = splitCsvRow(row);
        ASSERT_GE(fields.size(), 7u) << "row " << row_index;
        ASSERT_LT(row_index, digest.lines.size()) << "digest has fewer lines than the CSV";

        const auto& line = digest.lines[row_index];
        EXPECT_EQ(line.codeobj_id, std::stoll(fields[kCodeObj])) << "row " << row_index;
        EXPECT_EQ(line.addr, std::stoll(fields[kVaddr])) << "row " << row_index;
        EXPECT_EQ(line.hitcount, std::stoll(fields[kHit])) << "row " << row_index;
        EXPECT_EQ(line.latency, std::stoll(fields[kLatency])) << "row " << row_index;
        EXPECT_EQ(line.stall, std::stoll(fields[kStall])) << "row " << row_index;
        EXPECT_EQ(line.idle, std::stoll(fields[kIdle])) << "row " << row_index;
        ++row_index;
    }

    EXPECT_EQ(row_index, digest.lines.size()) << "CSV and digest line counts differ";
    EXPECT_EQ(row_index, 2521u);
}

TEST(Digest, ReportsSourceAttributionCoverage)
{
    const char* trace = std::getenv("RCV_CLI_TEST_TRACE");
    if (!trace) GTEST_SKIP() << "RCV_CLI_TEST_TRACE not set";

    DataStore store;
    ASSERT_TRUE(rcv::loadTrace(trace, store).ok);
    const auto digest = rcv::buildDigest(store, trace, 200);

    EXPECT_EQ(digest.meta.total_lines, 2521);
    // This capture was built without -g: only the kernel-name comment carries
    // a cppline. If this ever changes, --by asm is still the right default.
    EXPECT_EQ(digest.meta.lines_with_source, 1);
}
