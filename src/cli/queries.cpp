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

#include "cli/queries.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace rcv
{

namespace
{
int64_t keyValue(const HotspotRow& r, SortKey key)
{
    switch (key)
    {
        case SortKey::Exposed: return r.exposed();
        case SortKey::Total: return r.total();
        case SortKey::Stall: return r.stall;
        case SortKey::Idle: return r.idle;
    }
    return r.exposed();
}

/// Split an inlined-source chain on " -> ". Empty frames are dropped. Mirrors
/// ASMLine::ASMLine (asmcode.cpp:206-227).
std::vector<std::string> sourceFrames(const std::string& cppline)
{
    std::vector<std::string> out;
    const std::string sep = " -> ";
    size_t start = 0;
    while (start <= cppline.size())
    {
        const size_t end = cppline.find(sep, start);
        auto frame = cppline.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!frame.empty()) out.push_back(std::move(frame));
        if (end == std::string::npos) break;
        start = end + sep.size();
    }
    return out;
}

void accumulate(HotspotRow& row, const LineDigest& l)
{
    row.hitcount += l.hitcount;
    row.latency += l.latency;
    row.stall += l.stall;
    row.idle += l.idle;
    row.hidden += l.hidden();
}

std::vector<std::pair<std::string, int64_t>> sortedDescending(std::map<std::string, int64_t> acc)
{
    std::vector<std::pair<std::string, int64_t>> out(acc.begin(), acc.end());
    std::erase_if(out, [](const auto& p) { return p.second == 0; });
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}
} // namespace

SortKey parseSortKey(const std::string& s)
{
    if (s == "exposed") return SortKey::Exposed;
    if (s == "total") return SortKey::Total;
    if (s == "stall") return SortKey::Stall;
    if (s == "idle") return SortKey::Idle;
    throw std::invalid_argument("unknown sort key: " + s + " (expected exposed|total|stall|idle)");
}

GroupBy parseGroupBy(const std::string& s)
{
    if (s == "asm") return GroupBy::Asm;
    if (s == "source") return GroupBy::Source;
    throw std::invalid_argument("unknown grouping: " + s + " (expected asm|source)");
}

std::vector<HotspotRow> hotspot(const Digest& d, GroupBy by, SortKey sort, int top)
{
    std::vector<HotspotRow> rows;

    if (by == GroupBy::Asm)
    {
        rows.reserve(d.lines.size());
        for (const auto& l : d.lines)
        {
            HotspotRow r;
            r.key = std::to_string(l.index);
            r.index = l.index;
            r.label = l.inst;
            accumulate(r, l);
            rows.push_back(std::move(r));
        }
    }
    else
    {
        std::map<std::string, HotspotRow> by_frame;
        for (const auto& l : d.lines)
            for (const auto& frame : sourceFrames(l.cppline))
            {
                auto& r = by_frame[frame];
                if (r.key.empty())
                {
                    r.key = frame;
                    r.label = frame;
                }
                // Replicate the FULL cost to every frame in the chain. Do not
                // divide: the GUI charges each inlined frame the whole cost.
                accumulate(r, l);
            }
        for (auto& [frame, row] : by_frame) rows.push_back(std::move(row));
    }

    std::stable_sort(
        rows.begin(),
        rows.end(),
        [sort](const HotspotRow& a, const HotspotRow& b) { return keyValue(a, sort) > keyValue(b, sort); }
    );

    if (top > 0 && rows.size() > static_cast<size_t>(top)) rows.resize(static_cast<size_t>(top));
    return rows;
}

Totals totals(const Digest& d)
{
    Totals t;
    for (const auto& l : d.lines)
    {
        t.latency += l.latency;
        t.stall += l.stall;
        t.idle += l.idle;
        t.hidden += l.hidden();
    }
    return t;
}

std::vector<std::pair<std::string, int64_t>> stallReasonBreakdown(const Digest& d)
{
    std::map<std::string, int64_t> acc;
    for (const auto& l : d.lines)
        for (size_t i = 0; i < l.stallreasons.size(); ++i)
        {
            const std::string name =
                i < d.stall_reason_names.size() ? d.stall_reason_names[i] : "reason_" + std::to_string(i);
            acc[name] += l.stallreasons[i];
        }
    return sortedDescending(std::move(acc));
}

std::vector<std::pair<std::string, int64_t>> instructionTypeBreakdown(const Digest& d)
{
    std::map<std::string, int64_t> acc;
    for (const auto& l : d.lines)
    {
        const auto t = static_cast<size_t>(l.type);
        const std::string name = t < d.type_names.size() ? d.type_names[t] : "type_" + std::to_string(l.type);
        acc[name] += l.total();
    }
    return sortedDescending(std::move(acc));
}

std::vector<LineDigest> asmRange(const Digest& d, int first, int last)
{
    std::vector<LineDigest> out;
    if (d.lines.empty() || last < first) return out;
    for (const auto& l : d.lines)
        if (l.index >= first && l.index <= last) out.push_back(l);
    return out;
}

std::vector<LineDigest> asmAround(const Digest& d, int index, int context)
{
    return asmRange(d, index - context, index + context);
}

} // namespace rcv
