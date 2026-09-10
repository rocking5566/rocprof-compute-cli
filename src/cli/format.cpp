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

#include "cli/format.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include "cli/queries.h"

namespace rcv
{

namespace
{
bool looksNumeric(const std::string& s)
{
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '.' && s[i] != '%') return false;
    return true;
}
} // namespace

std::string renderTable(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows)
{
    const size_t cols = headers.size();
    std::vector<size_t> width(cols, 0);
    std::vector<bool> numeric(cols, true);

    for (size_t c = 0; c < cols; ++c) width[c] = headers[c].size();
    for (const auto& row : rows)
        for (size_t c = 0; c < cols && c < row.size(); ++c)
        {
            width[c] = std::max(width[c], row[c].size());
            if (!looksNumeric(row[c])) numeric[c] = false;
        }
    // The first column is a label even when it happens to be all digits.
    if (!numeric.empty()) numeric[0] = false;

    std::ostringstream out;
    auto emit = [&](const std::vector<std::string>& cells)
    {
        for (size_t c = 0; c < cols; ++c)
        {
            const std::string& cell = c < cells.size() ? cells[c] : std::string{};
            if (c) out << "  ";
            if (numeric[c])
                out << std::string(width[c] - cell.size(), ' ') << cell;
            else
            {
                out << cell;
                if (c + 1 < cols) out << std::string(width[c] - cell.size(), ' ');
            }
        }
        out << "\n";
    };

    emit(headers);
    for (const auto& row : rows) emit(row);
    return out.str();
}

std::string formatPercent(int64_t part, int64_t whole)
{
    if (whole == 0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
    return buf;
}

std::string renderSummary(const Digest& d)
{
    std::ostringstream out;
    const auto t = totals(d);

    out << "trace:  " << d.meta.trace_path << "\n";
    out << "gpu:    gfxip " << d.meta.gfxip << " (" << d.meta.gfxv << ")\n";
    if (!d.meta.kernel_name.empty()) out << "kernel: " << d.meta.kernel_name << "\n";
    out << "scale:  " << d.meta.wave_count << " waves, " << d.meta.se_count << " SEs, " << d.meta.total_lines
        << " ASM lines\n";
    out << "window: " << d.meta.trace_begin << " - " << d.meta.trace_end << " ("
        << (d.meta.trace_end - d.meta.trace_begin) << " cycles)\n\n";

    out << "cycles\n";
    out << renderTable(
        {
            "bucket", "cycles", "share"
    },
        {{"issue", std::to_string(t.issue()), formatPercent(t.issue(), t.total())},
         {"stall", std::to_string(t.stall), formatPercent(t.stall, t.total())},
         {"idle", std::to_string(t.idle), formatPercent(t.idle, t.total())},
         {"total", std::to_string(t.total()), "100.0%"},
         {"hidden", std::to_string(t.hidden), formatPercent(t.hidden, t.total())},
         {"exposed", std::to_string(t.exposed()), formatPercent(t.exposed(), t.total())}}
    );

    if (!d.meta.hidden_latency_available)
        out << "\nWARNING: hidden latency was not computed; hidden/exposed columns are "
               "unreliable and 'exposed' degenerates to 'total'.\n";

    out << "\nstall reasons\n";
    std::vector<std::vector<std::string>> stall_rows;
    for (const auto& [name, cycles] : stallReasonBreakdown(d))
        stall_rows.push_back({name, std::to_string(cycles), formatPercent(cycles, t.stall)});
    out << renderTable({"reason", "cycles", "share"}, stall_rows);

    out << "\ninstruction types\n";
    std::vector<std::vector<std::string>> type_rows;
    for (const auto& [name, cycles] : instructionTypeBreakdown(d))
        type_rows.push_back({name, std::to_string(cycles), formatPercent(cycles, t.total())});
    out << renderTable({"type", "cycles", "share"}, type_rows);

    out << "\ntop 10 ASM lines by exposed cycles\n";
    std::vector<std::vector<std::string>> hot_rows;
    for (const auto& r : hotspot(d, GroupBy::Asm, SortKey::Exposed, 10))
        hot_rows.push_back(
            {std::to_string(r.index),
             r.label,
             std::to_string(r.exposed()),
             std::to_string(r.total()),
             std::to_string(r.stall)}
        );
    out << renderTable({"idx", "instruction", "exposed", "total", "stall"}, hot_rows);

    out << "\nsource attribution: " << d.meta.lines_with_source << "/" << d.meta.total_lines << " ("
        << formatPercent(d.meta.lines_with_source, d.meta.total_lines) << ")";
    if (d.meta.lines_with_source * 10 < d.meta.total_lines) out << " - kernel likely built without -g; use --by asm";
    out << "\n";

    if (!d.occupancy.total.empty())
    {
        const auto peak = *std::max_element(d.occupancy.total.begin(), d.occupancy.total.end());
        double sum = 0;
        for (const double v : d.occupancy.total) sum += v;
        char buf[128];
        std::snprintf(
            buf,
            sizeof(buf),
            "\noccupancy: peak %.1f waves, mean %.1f waves over %d bins\n",
            peak,
            sum / static_cast<double>(d.occupancy.total.size()),
            d.occupancy.bins
        );
        out << buf;
    }

    return out.str();
}

nlohmann::json summaryJson(const Digest& d)
{
    const auto t = totals(d);
    nlohmann::json j;
    j["meta"] = toJson(d)["meta"];
    j["cycles"] = {
        {"issue",   t.issue()  },
        {"stall",   t.stall    },
        {"idle",    t.idle     },
        {"total",   t.total()  },
        {"hidden",  t.hidden   },
        {"exposed", t.exposed()}
    };
    j["stall_reasons"] = stallReasonBreakdown(d);
    j["instruction_types"] = instructionTypeBreakdown(d);

    auto& hot = j["top_lines"] = nlohmann::json::array();
    for (const auto& r : hotspot(d, GroupBy::Asm, SortKey::Exposed, 10))
        hot.push_back({
            {"index",   r.index    },
            {"inst",    r.label    },
            {"exposed", r.exposed()},
            {"total",   r.total()  },
            {"stall",   r.stall    }
        });
    return j;
}

} // namespace rcv
