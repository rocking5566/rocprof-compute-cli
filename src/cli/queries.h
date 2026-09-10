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

#pragma once

#include <string>
#include <utility>
#include <vector>
#include "cli/digest.h"

namespace rcv
{

enum class SortKey
{
    Exposed,
    Total,
    Stall,
    Idle
};

enum class GroupBy
{
    Asm,
    Source
};

/// One row of a hotspot ranking. For GroupBy::Asm, `key` is the ASM index as
/// text and `index` is that index. For GroupBy::Source, `key` is the source
/// frame string and `index` is -1.
struct HotspotRow
{
    std::string key;
    int index = -1;
    std::string label;
    int64_t hitcount = 0, latency = 0, stall = 0, idle = 0, hidden = 0;
    int64_t issue() const { return latency - stall; }
    int64_t total() const { return latency + idle; }
    int64_t exposed() const { return total() - hidden; }
};

SortKey parseSortKey(const std::string& s);
GroupBy parseGroupBy(const std::string& s);

std::vector<HotspotRow> hotspot(const Digest& d, GroupBy by, SortKey sort, int top);

/// Cycle totals aggregated over every line.
struct Totals
{
    int64_t latency = 0, stall = 0, idle = 0, hidden = 0;
    int64_t issue() const { return latency - stall; }
    int64_t total() const { return latency + idle; }
    int64_t exposed() const { return total() - hidden; }
};
Totals totals(const Digest& d);

/// name -> cycles, descending. Zero-valued entries are dropped.
std::vector<std::pair<std::string, int64_t>> stallReasonBreakdown(const Digest& d);
std::vector<std::pair<std::string, int64_t>> instructionTypeBreakdown(const Digest& d);

/// Inclusive index range, clamped to the digest. `context` expands around `index`.
std::vector<LineDigest> asmRange(const Digest& d, int first, int last);
std::vector<LineDigest> asmAround(const Digest& d, int index, int context);

} // namespace rcv
