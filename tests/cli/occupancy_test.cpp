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
#include "cli/digest_builder.h"
#include "data/records.h"

namespace
{
occupancy_record_t rec(uint64_t time, bool start)
{
    occupancy_record_t r{};
    r.time = time;
    r.start = start ? 1u : 0u;
    return r;
}
} // namespace

// One wave occupies the whole window: every bin must read exactly 1.
TEST(Occupancy, SingleWaveSpanningWindow)
{
    const std::vector<occupancy_record_t> records{rec(0, true), rec(100, false)};
    const auto bins = rcv::binOccupancy(records, 0, 100, 4);

    ASSERT_EQ(bins.size(), 4u);
    for (const double b : bins) EXPECT_DOUBLE_EQ(b, 1.0);
}

// Two waves start together and one leaves halfway: 2,2,1,1.
TEST(Occupancy, ConcurrencyDropsMidWindow)
{
    const std::vector<occupancy_record_t> records{rec(0, true), rec(0, true), rec(50, false), rec(100, false)};
    const auto bins = rcv::binOccupancy(records, 0, 100, 4);

    ASSERT_EQ(bins.size(), 4u);
    EXPECT_DOUBLE_EQ(bins[0], 2.0);
    EXPECT_DOUBLE_EQ(bins[1], 2.0);
    EXPECT_DOUBLE_EQ(bins[2], 1.0);
    EXPECT_DOUBLE_EQ(bins[3], 1.0);
}

TEST(Occupancy, EmptyRecordsGiveZeroBins)
{
    const auto bins = rcv::binOccupancy({}, 0, 100, 4);
    ASSERT_EQ(bins.size(), 4u);
    for (const double b : bins) EXPECT_DOUBLE_EQ(b, 0.0);
}

TEST(Occupancy, DegenerateWindowDoesNotDivideByZero)
{
    const std::vector<occupancy_record_t> records{rec(5, true)};
    const auto bins = rcv::binOccupancy(records, 5, 5, 4);
    ASSERT_EQ(bins.size(), 4u);
}
