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

#include <gtest/gtest.h>
#include <string>
#include "analysis/hidden_latency.h"
#include "cli/digest_builder.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"

namespace
{
rcv::Digest analyzeFixture(const std::string& name)
{
    const std::string path = std::string(RCV_CLI_FIXTURE_DIR) + "/" + name;
    DataStore store;
    const auto load = rcv::loadTrace(path, store);
    EXPECT_TRUE(load.ok) << path << ": " << load.error;
    HiddenLatencyAnalysis::analyze(store);
    return rcv::buildDigest(store, path, 8);
}

const rcv::LineDigest* lineAt(const rcv::Digest& d, int index)
{
    for (const auto& l : d.lines)
        if (l.index == index) return &l;
    return nullptr;
}
} // namespace

TEST(Synthetic, FixtureLoads)
{
    const auto d = analyzeFixture("solo");
    EXPECT_EQ(d.lines.size(), 3u);
    EXPECT_EQ(d.meta.gfxip, 12);
    EXPECT_EQ(d.meta.wave_count, 1);
}

// Nothing else is running, so the stall cannot be masked.
TEST(Synthetic, SoloWaveHasNoHiddenLatency)
{
    const auto d = analyzeFixture("solo");
    const auto* line = lineAt(d, 1);
    ASSERT_NE(line, nullptr);

    EXPECT_EQ(line->stall, 180);
    EXPECT_EQ(line->hidden_stall, 0);
    EXPECT_EQ(line->exposed(), line->total());
}

// A second wave issues VALU work across the whole stall window, so part of the
// stall is masked. This is the assertion that a silent no-op implementation
// cannot pass.
TEST(Synthetic, ConcurrentWaveHidesPartOfTheStall)
{
    const auto d = analyzeFixture("synthetic");
    const auto* line = lineAt(d, 1);
    ASSERT_NE(line, nullptr);

    EXPECT_EQ(line->stall, 180);
    EXPECT_GT(line->hidden_stall, 0);
    EXPECT_LE(line->hidden_stall, line->stall);
    EXPECT_LT(line->exposed(), line->total());
}

TEST(Synthetic, TwoWavesAreBothLoaded)
{
    const auto d = analyzeFixture("synthetic");
    EXPECT_EQ(d.meta.wave_count, 2);
}
