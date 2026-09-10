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

#include "data/trace_loader.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include "data/datastore.h"
#include "data/shaderdata.h"

namespace
{
std::string traceRoot()
{
    const char* env = std::getenv("RCV_CLI_TEST_TRACE");
    return env ? env : "";
}
} // namespace

TEST(TraceLoader, LoadsReferenceCapture)
{
    const auto root = traceRoot();
    if (root.empty()) GTEST_SKIP() << "RCV_CLI_TEST_TRACE not set";

    DataStore store;
    const auto result = rcv::loadTrace(root, store);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(store.code.size(), 2521u);
    EXPECT_EQ(store.gfxip, 12);

    int wave_count = 0;
    store.forEachWave([&](const DataStore::WaveCoordinate&, const WaveEntry&) { ++wave_count; });
    EXPECT_EQ(wave_count, 128);

    EXPECT_EQ(store.occupancy_by_se.size(), 16u);
}

TEST(TraceLoader, ReportsErrorForMissingPath)
{
    DataStore store;
    const auto result = rcv::loadTrace("/nonexistent/path/xyz", store);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}
