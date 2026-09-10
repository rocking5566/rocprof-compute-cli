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

#include "cli/digest_builder.h"

#include <algorithm>
#include "code/codeload.hpp"
#include "config/config.hpp"
#include "data/datastore.h"

namespace rcv
{

bool tokenTypesSupportHiddenLatency()
{
    bool has_valu = false;
    bool has_matrix = false;
    for (const auto& c : Config::TokenColors())
    {
        if (c.name.find("VALU") != std::string::npos) has_valu = true;
        if (c.name.find("MATRIX") != std::string::npos || c.name.find("MFMA") != std::string::npos ||
            c.name.find("WMMA") != std::string::npos)
            has_matrix = true;
    }
    return has_valu && has_matrix;
}

namespace
{
std::vector<std::string> namesOf(const std::vector<StyleColor>& colors)
{
    std::vector<std::string> out;
    out.reserve(colors.size());
    for (const auto& c : colors) out.push_back(c.name);
    return out;
}
} // namespace

Digest buildDigest(const DataStore& store, const std::string& trace_path, int occupancy_bins)
{
    Digest d;
    d.meta.gfxip = store.gfxip;
    d.meta.gfxv = store.gfxv;
    d.meta.trace_path = trace_path;
    d.meta.se_count = static_cast<int>(store.occupancy_by_se.size());
    d.meta.hidden_latency_available = store.hidden_latency_analyzed;

    d.type_names = namesOf(Config::TokenColors());
    d.stall_reason_names = namesOf(Config::StallReasonColors());

    d.lines.reserve(store.code.size());
    for (const auto& code : store.code)
    {
        if (!code.line) continue;
        const auto& src = *code.line;

        LineDigest l;
        l.index = src.index.load();
        l.addr = src.addr;
        l.codeobj_id = src.codeobj_id;
        l.inst = src.inst;
        l.cppline = src.cppline;
        l.type = src.type.load();
        l.hitcount = src.hitcount;
        l.latency = src.latency_sum;
        l.stall = src.stall_sum;
        l.idle = src.idle_sum;
        l.stallreasons = src.stallreasons;

        if (const auto it = store.hidden_latency_by_line.find(l.index); it != store.hidden_latency_by_line.end())
        {
            l.hidden_idle = it->second.idle;
            l.hidden_stall = it->second.stall;
            l.hidden_issue = it->second.issue;
        }

        if (!l.cppline.empty()) ++d.meta.lines_with_source;
        d.lines.push_back(std::move(l));
    }
    d.meta.total_lines = static_cast<int>(d.lines.size());

    // The kernel name is the first code entry, emitted as a "; <mangled>" comment.
    if (!d.lines.empty() && d.lines.front().inst.rfind("; ", 0) == 0)
        d.meta.kernel_name = d.lines.front().inst.substr(2);

    int64_t begin = 0;
    int64_t end = 0;
    bool first = true;
    store.forEachWave(
        [&](const DataStore::WaveCoordinate& coord, const WaveEntry& entry)
        {
            d.waves.push_back({coord.hwid.se, coord.hwid.cu, coord.hwid.simd, coord.hwid.slot, entry.begin, entry.end});
            if (first)
            {
                begin = entry.begin;
                end = entry.end;
                first = false;
            }
            else
            {
                begin = std::min(begin, entry.begin);
                end = std::max(end, entry.end);
            }
        }
    );
    d.meta.wave_count = static_cast<int>(d.waves.size());
    d.meta.trace_begin = begin;
    d.meta.trace_end = end;

    d.occupancy.bins = occupancy_bins;
    d.occupancy.t0 = begin;
    d.occupancy.t1 = end;

    return d;
}

} // namespace rcv
