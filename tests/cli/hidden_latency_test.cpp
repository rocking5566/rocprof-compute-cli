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

#include "analysis/hidden_latency.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include "cli/digest_builder.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"

namespace
{
rcv::Digest analyzeReference()
{
    const char* trace = std::getenv("RCV_CLI_TEST_TRACE");
    if (!trace) return {};

    DataStore store;
    if (!rcv::loadTrace(trace, store).ok) return {};
    HiddenLatencyAnalysis::analyze(store);
    return rcv::buildDigest(store, trace, 200);
}
} // namespace

// Proves the ANALYSIS produces non-zero hidden latency. It cannot prove the CLI
// invokes it, because this helper calls analyze() directly - Task 9's smoke test
// covers that by reading a digest the rcv-cli binary actually wrote.
TEST(HiddenLatency, AnalysisProducesNonZeroHiddenLatency)
{
    if (!std::getenv("RCV_CLI_TEST_TRACE")) GTEST_SKIP() << "RCV_CLI_TEST_TRACE not set";
    const auto d = analyzeReference();

    ASSERT_FALSE(d.lines.empty());
    EXPECT_TRUE(d.meta.hidden_latency_available);

    int64_t sum = 0;
    for (const auto& l : d.lines) sum += l.hidden();
    EXPECT_GT(sum, 0) << "hidden latency is entirely zero - the wave-load budget "
                         "gate has probably been inherited from MainWindow";
}

// The RAW components are NOT bounded by their counterparts - hidden_latency.cpp
// accumulates per-token idle gaps keyed by code_line, while idle_sum comes from
// rocprofv3's Idle column in code.json. Different producers, different
// quantities. On the reference capture line 1695 has hidden_idle 529185 against
// idle 207116. The GUI clamps at read time (hotspot.hpp:46-57) and so must the
// digest; these assertions are on the CLAMPED accessors.
TEST(HiddenLatency, ClampedComponentsRespectBounds)
{
    if (!std::getenv("RCV_CLI_TEST_TRACE")) GTEST_SKIP() << "RCV_CLI_TEST_TRACE not set";
    const auto d = analyzeReference();
    ASSERT_FALSE(d.lines.empty());

    for (const auto& l : d.lines)
    {
        EXPECT_GE(l.hiddenIdle(), 0) << "line " << l.index;
        EXPECT_LE(l.hiddenIdle(), l.idle) << "line " << l.index;
        EXPECT_GE(l.hiddenStall(), 0) << "line " << l.index;
        EXPECT_LE(l.hiddenStall(), l.stall) << "line " << l.index;
        EXPECT_GE(l.hiddenIssue(), 0) << "line " << l.index;
        EXPECT_LE(l.hiddenIssue(), l.issue()) << "line " << l.index;
        EXPECT_GE(l.exposed(), 0) << "line " << l.index;
        EXPECT_LE(l.hidden(), l.total()) << "line " << l.index;
    }
}

// Guards the reason clamping exists: at least one line on the reference capture
// really does report a raw hidden_idle larger than its idle. If this ever stops
// being true the clamp is still correct, but the invariant above stops being a
// meaningful test, so fail loudly rather than passing vacuously.
TEST(HiddenLatency, RawComponentsDoOverflowSomewhere)
{
    if (!std::getenv("RCV_CLI_TEST_TRACE")) GTEST_SKIP() << "RCV_CLI_TEST_TRACE not set";
    const auto d = analyzeReference();

    int overflow = 0;
    for (const auto& l : d.lines)
        if (l.hidden_idle > l.idle || l.hidden_stall > l.stall || l.hidden_issue > l.issue()) ++overflow;

    EXPECT_GT(overflow, 0) << "no raw component overflows; the clamp is now untested";
}

// Regression guard: a token_def.json in the CWD replaces the built-in list. If
// it lacks VALU or MATRIX, hidden-latency analysis returns false and every
// hidden value silently becomes zero.
TEST(HiddenLatency, ActiveTokenListSupportsAnalysis)
{
    EXPECT_TRUE(rcv::tokenTypesSupportHiddenLatency())
        << "the active token list lacks VALU or MATRIX/MFMA/WMMA; a token_def.json in the "
           "working directory has probably replaced the built-in defaults";
}
