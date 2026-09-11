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

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "json/include/nlohmann/json.hpp"

namespace rcv
{

struct LineDigest
{
    int index = 0;
    int64_t addr = 0;
    int64_t codeobj_id = 0;
    std::string inst, cppline;
    int type = 0;
    int64_t hitcount = 0, latency = 0, stall = 0, idle = 0;
    int64_t hidden_idle = 0, hidden_stall = 0, hidden_issue = 0;
    std::vector<int64_t> stallreasons;
    int64_t issue() const { return latency - stall; }
    int64_t total() const { return latency + idle; }
    // Raw components come from two different producers and are not mutually
    // bounded - see the spec's "Metric definitions". Clamp on read, as the GUI does.
    int64_t hiddenIdle() const { return std::clamp<int64_t>(hidden_idle, 0, idle); }
    int64_t hiddenStall() const { return std::clamp<int64_t>(hidden_stall, 0, stall); }
    int64_t hiddenIssue() const { return std::clamp<int64_t>(hidden_issue, 0, issue()); }
    int64_t hidden() const { return hiddenIdle() + hiddenStall() + hiddenIssue(); }
    int64_t exposed() const { return total() - hidden(); }
};

struct WaveDigest
{
    int se = 0, cu = -1, simd = 0, slot = 0;
    int64_t begin = 0, end = 0;
    int instance = -1;
};

struct OccupancyGroup
{
    int se = 0, cu = 0, simd = -1; // -1 means aggregated across SIMD
    bool available = false;
    int64_t peak_waves = 0, wave_starts = 0;
    int64_t recorded_wave_starts = -1; // capture-wide; -1 if not stored
    double mean_waves = 0;
};

struct OccupancyDigest
{
    int bins = 0;
    int64_t t0 = 0, t1 = 0;
    std::map<int, std::vector<double>> per_se;
    std::vector<double> total;
    bool granular_present = false;
    std::vector<OccupancyGroup> per_cu, per_simd;
};

struct DigestMeta
{
    int gfxip = 0;
    std::string gfxv, trace_path, kernel_name;
    int wave_count = 0, se_count = 0;
    int64_t trace_begin = 0, trace_end = 0;
    int lines_with_source = 0, total_lines = 0;
    bool hidden_latency_available = false;
};

struct Digest
{
    int version = 2;
    DigestMeta meta;
    std::vector<std::string> type_names, stall_reason_names;
    std::vector<LineDigest> lines;
    OccupancyDigest occupancy;
    std::vector<WaveDigest> waves;
};

nlohmann::json toJson(const Digest& digest);
Digest fromJson(const nlohmann::json& j);

} // namespace rcv
