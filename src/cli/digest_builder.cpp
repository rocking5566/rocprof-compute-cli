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
#include <tuple>
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

std::vector<double> binOccupancy(const std::vector<occupancy_record_t>& records, int64_t t0, int64_t t1, int bins)
{
    std::vector<double> out(static_cast<size_t>(std::max(bins, 0)), 0.0);
    if (out.empty() || records.empty()) return out;

    const int64_t span = t1 - t0;
    if (span <= 0) return out;

    // Integrate the running concurrency over each bin, then divide by bin width
    // so the value is a time-weighted mean rather than a sample.
    const double bin_width = static_cast<double>(span) / static_cast<double>(bins);
    std::vector<double> area(out.size(), 0.0);

    int concurrency = 0;
    int64_t prev_time = t0;

    auto accumulate = [&](int64_t from, int64_t to)
    {
        if (to <= from || concurrency == 0) return;
        const double a = std::clamp(static_cast<double>(from - t0), 0.0, static_cast<double>(span));
        const double b = std::clamp(static_cast<double>(to - t0), 0.0, static_cast<double>(span));
        size_t first = static_cast<size_t>(a / bin_width);
        size_t last = static_cast<size_t>(b / bin_width);
        first = std::min(first, out.size() - 1);
        last = std::min(last, out.size() - 1);

        for (size_t i = first; i <= last; ++i)
        {
            const double lo = std::max(a, static_cast<double>(i) * bin_width);
            const double hi = std::min(b, static_cast<double>(i + 1) * bin_width);
            if (hi > lo) area[i] += (hi - lo) * concurrency;
        }
    };

    for (const auto& r : records)
    {
        const auto t = static_cast<int64_t>(r.time);
        accumulate(prev_time, t);
        concurrency += 2 * static_cast<int>(r.start) - 1;
        prev_time = t;
    }
    accumulate(prev_time, t1);

    for (size_t i = 0; i < out.size(); ++i) out[i] = area[i] / bin_width;
    return out;
}

namespace
{
// Group deltas at identical timestamps before measuring residency. Integrate
// only positive-length overlap with [t0,t1); validate the entire event stream.
OccupancyGroup summarizeOccupancy(const std::map<int64_t, std::pair<int64_t, int64_t>>& events, int64_t t0, int64_t t1)
{
    OccupancyGroup out;
    out.recorded_wave_starts = 0;
    int64_t resident = 0, previous = events.empty() ? t0 : events.begin()->first;
    long double area = 0;
    bool valid = !events.empty() && t1 > t0;
    for (const auto& [time, counts] : events)
    {
        const auto lo = std::max(previous, t0), hi = std::min(time, t1);
        if (hi > lo)
        {
            area += static_cast<long double>(hi - lo) * resident;
            out.peak_waves = std::max(out.peak_waves, resident);
        }
        resident += counts.first;
        if (resident < 0) valid = false;
        if (time >= t0 && time < t1) out.wave_starts += counts.second;
        out.recorded_wave_starts += counts.second;
        previous = time;
    }
    if (resident != 0) valid = false;
    out.available = valid;
    if (valid)
        out.mean_waves = static_cast<double>(area / (t1 - t0));
    else
        out.peak_waves = 0;
    return out;
}

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
            d.waves.push_back(
                {coord.hwid.se, coord.hwid.cu, coord.hwid.simd, coord.hwid.slot, entry.begin, entry.end, coord.instance}
            );
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
    d.occupancy.granular_present = true;
    using Events = std::map<int64_t, std::pair<int64_t, int64_t>>;
    std::map<std::pair<int, int>, Events> cu_events;
    std::map<std::tuple<int, int, int>, Events> simd_events;
    for (const auto& [se, records] : store.occupancy_by_se)
        for (const auto& r : records)
        {
            const auto time = static_cast<int64_t>(r.time);
            for (auto* events : {&cu_events[{se, r.cu}], &simd_events[{se, r.cu, r.simd}]})
            {
                auto& counts = (*events)[time];
                counts.first += r.start ? 1 : -1;
                counts.second += r.start ? 1 : 0;
            }
        }
    for (const auto& [key, events] : cu_events)
    {
        auto g = summarizeOccupancy(events, begin, end);
        g.se = key.first;
        g.cu = key.second;
        d.occupancy.per_cu.push_back(g);
    }
    for (const auto& [key, events] : simd_events)
    {
        auto g = summarizeOccupancy(events, begin, end);
        std::tie(g.se, g.cu, g.simd) = key;
        d.occupancy.per_simd.push_back(g);
    }

    // An absent or present-but-empty occupancy source is unavailable, not a
    // measured series of zero activity. Preserve that distinction as an empty
    // total vector; nonempty records may still legitimately bin to all zeros.
    const bool has_occupancy_records = std::any_of(
        store.occupancy_by_se.begin(),
        store.occupancy_by_se.end(),
        [](const auto& entry) { return !entry.second.empty(); }
    );
    if (has_occupancy_records)
        d.occupancy.total.assign(static_cast<size_t>(std::max(occupancy_bins, 0)), 0.0);

    for (const auto& [se, records] : store.occupancy_by_se)
    {
        if (records.empty()) continue;
        auto series = binOccupancy(records, begin, end, occupancy_bins);
        for (size_t i = 0; i < series.size() && i < d.occupancy.total.size(); ++i) d.occupancy.total[i] += series[i];
        d.occupancy.per_se[se] = std::move(series);
    }

    return d;
}

} // namespace rcv
