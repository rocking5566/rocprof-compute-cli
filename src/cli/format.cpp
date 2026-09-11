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
#include <set>
#include <sstream>
#include <tuple>
#include "cli/queries.h"

namespace rcv
{

namespace
{
// Statistics of the stored bin means, not instantaneous concurrency peaks.
std::pair<double, double> occupancyStats(const OccupancyDigest& occupancy)
{
    if (occupancy.total.empty()) return {0.0, 0.0};
    const auto peak = *std::max_element(occupancy.total.begin(), occupancy.total.end());
    double sum = 0;
    for (double value : occupancy.total) sum += value;
    return {peak, sum / static_cast<double>(occupancy.total.size())};
}

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
    const auto coverage = coverageJson(d, 1);
    out << "instruction-traced coverage: " << coverage["known_cu_count"] << " known SE/CU pairs, "
        << coverage["total_groups"] << " SE/CU/SIMD groups, " << coverage["unknown_cu_waves"]
        << " waves with unknown CU (not hardware utilization)\n";
    out << "occupancy population: " << coverage["occupancy_wave_starts"] << " recorded starts; "
        << coverage["occupancy_wave_starts_in_window"] << " in digest window (null = unavailable)\n";
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
    if (stall_rows.empty())
    {
        // Stall reasons come from PC sampling, not from the thread trace. A
        // pure SQTT capture has none, so say why rather than print an empty
        // table and let the reader assume there were no stalls.
        out << "  none recorded - stall reasons require PC sampling, and this is a "
               "thread-trace-only capture.\n";
        out << "  " << t.stall << " stall cycles (" << formatPercent(t.stall, t.total())
            << ") are still attributed per line; use 'hotspot --sort stall'.\n";
    }
    else
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
        const auto [peak, mean] = occupancyStats(d.occupancy);
        char buf[128];
        std::snprintf(
            buf,
            sizeof(buf),
            "\noccupancy: peak %.1f waves, mean %.1f waves over %d bins (binned means)\n",
            peak,
            mean,
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
    j["coverage"] = coverageJson(d, 1);
    j["coverage"].erase("groups");
    j["coverage"].erase("truncated");
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
    const auto [peak, mean] = occupancyStats(d.occupancy);
    const bool available = !d.occupancy.total.empty();
    j["occupancy"] = {
        {"basis", "binned_total_concurrency"},
        {"available", available},
        {"bins", d.occupancy.bins},
        {"peak_waves", available ? nlohmann::json(peak) : nlohmann::json(nullptr)},
        {"mean_waves", available ? nlohmann::json(mean) : nlohmann::json(nullptr)}
    };

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

int dominantStallReason(const LineDigest& l)
{
    int best = -1;
    int64_t best_value = 0;
    for (size_t i = 0; i < l.stallreasons.size(); ++i)
        if (l.stallreasons[i] > best_value)
        {
            best_value = l.stallreasons[i];
            best = static_cast<int>(i);
        }
    return best;
}

namespace
{
std::string stallReasonName(const Digest& d, const LineDigest& l)
{
    const int idx = dominantStallReason(l);
    if (idx < 0) return "-";
    if (static_cast<size_t>(idx) < d.stall_reason_names.size()) return d.stall_reason_names[idx];
    return "reason_" + std::to_string(idx);
}

std::string typeName(const Digest& d, int type)
{
    const auto t = static_cast<size_t>(type);
    if (t < d.type_names.size()) return d.type_names[t];
    return "type_" + std::to_string(type);
}
// Source keys are whole demangled signatures running to thousands of
// characters. Printing one unabridged costs more context than the rest of the
// report put together, which defeats the point of the tool.
constexpr size_t kMaxLabel = 70;

std::string abbreviate(const std::string& s)
{
    if (s.size() <= kMaxLabel) return s;
    return s.substr(0, kMaxLabel - 3) + "...";
}
} // namespace

std::string renderHotspot(const Digest& d, const std::vector<HotspotRow>& rows)
{
    const auto t = totals(d);
    std::vector<std::vector<std::string>> table;
    size_t ordinal = 0;
    for (const auto& r : rows)
        // Grouped by source there is no ASM index and the key *is* the label,
        // so show a row ordinal instead of repeating the signature twice.
        table.push_back(
            {r.index >= 0 ? std::to_string(r.index) : std::to_string(ordinal++),
             abbreviate(r.label),
             std::to_string(r.exposed()),
             formatPercent(r.exposed(), t.total()),
             std::to_string(r.total()),
             std::to_string(r.stall),
             std::to_string(r.idle),
             std::to_string(r.hidden),
             std::to_string(r.hitcount)}
        );
    return renderTable({"idx", "what", "exposed", "share", "total", "stall", "idle", "hidden", "hits"}, table);
}

nlohmann::json hotspotJson(const std::vector<HotspotRow>& rows)
{
    auto j = nlohmann::json::array();
    for (const auto& r : rows)
        j.push_back({
            {"index",    r.index    },
            {"key",      r.key      },
            {"label",    r.label    },
            {"exposed",  r.exposed()},
            {"total",    r.total()  },
            {"latency",  r.latency  },
            {"stall",    r.stall    },
            {"idle",     r.idle     },
            {"hidden",   r.hidden   },
            {"hitcount", r.hitcount }
        });
    return j;
}

std::string renderAsm(const Digest& d, const std::vector<LineDigest>& lines)
{
    std::vector<std::vector<std::string>> table;
    for (const auto& l : lines)
        table.push_back(
            {std::to_string(l.index),
             std::to_string(l.addr),
             l.inst,
             typeName(d, l.type),
             std::to_string(l.hitcount),
             std::to_string(l.exposed()),
             std::to_string(l.total()),
             std::to_string(l.issue()),
             std::to_string(l.stall),
             std::to_string(l.idle),
             std::to_string(l.hidden()),
             stallReasonName(d, l)}
        );
    return renderTable(
        {"idx",
         "addr",
         "instruction",
         "type",
         "hits",
         "exposed",
         "total",
         "issue",
         "stall",
         "idle",
         "hidden",
         "stall_reason"},
        table
    );
}

nlohmann::json asmJson(const Digest& d, const std::vector<LineDigest>& lines)
{
    auto j = nlohmann::json::array();
    for (const auto& l : lines)
        j.push_back({
            {"index", l.index},
            {"addr", l.addr},
            {"inst", l.inst},
            {"type", typeName(d, l.type)},
            {"source", l.cppline},
            {"hitcount", l.hitcount},
            {"exposed", l.exposed()},
            {"total", l.total()},
            {"issue", l.issue()},
            {"stall", l.stall},
            {"idle", l.idle},
            {"hidden", l.hidden()},
            {"stall_reason", stallReasonName(d, l)},
            {"stall_reasons", l.stallreasons}
        });
    return j;
}

std::string renderOccupancy(const Digest& d, int se)
{
    const std::vector<double>* series = nullptr;
    std::string label = "all";
    if (se < 0)
        series = &d.occupancy.total;
    else if (const auto it = d.occupancy.per_se.find(se); it != d.occupancy.per_se.end())
    {
        series = &it->second;
        label = "SE" + std::to_string(se);
    }

    if (!series || series->empty())
        return "no occupancy data for " + (se < 0 ? std::string("any SE") : "SE" + std::to_string(se)) + "\n";

    const int64_t span = d.occupancy.t1 - d.occupancy.t0;
    const double bin_width = d.occupancy.bins > 0 ? static_cast<double>(span) / d.occupancy.bins : 0.0;

    std::vector<std::vector<std::string>> table;
    for (size_t i = 0; i < series->size(); ++i)
    {
        char value[32];
        std::snprintf(value, sizeof(value), "%.1f", (*series)[i]);
        table.push_back({std::to_string(i), std::to_string(d.occupancy.t0 + static_cast<int64_t>(i * bin_width)), value}
        );
    }
    return label + " concurrency over " + std::to_string(d.occupancy.bins) + " bins\n" +
           renderTable({"bin", "t_start", "waves"}, table);
}

nlohmann::json occupancyJson(const Digest& d, int se)
{
    nlohmann::json j;
    j["bins"] = d.occupancy.bins;
    j["t0"] = d.occupancy.t0;
    j["t1"] = d.occupancy.t1;
    if (se < 0)
        j["series"] = d.occupancy.total;
    else if (const auto it = d.occupancy.per_se.find(se); it != d.occupancy.per_se.end())
        j["series"] = it->second;
    else
        j["series"] = nlohmann::json::array();
    j["se"] = se;
    return j;
}

nlohmann::json coverageJson(const Digest& d, int top)
{
    if (top < 1 || top > MaxHotspotRows) throw std::invalid_argument("coverage top must be in [1, 1000]");
    struct Group
    {
        size_t waves = 0, unknown_instances = 0;
        int first = -1, last = -1;
        std::set<int> slots;
    };
    std::map<std::tuple<int, int, int>, Group> groups;
    std::set<std::pair<int, int>> cus;
    size_t known = 0;
    for (const auto& w : d.waves)
    {
        auto& g = groups[{w.se, w.cu, w.simd}];
        ++g.waves;
        g.slots.insert(w.slot);
        if (w.cu >= 0)
        {
            ++known;
            cus.emplace(w.se, w.cu);
        }
        if (w.instance < 0)
            ++g.unknown_instances;
        else
        {
            g.first = g.first < 0 ? w.instance : std::min(g.first, w.instance);
            g.last = std::max(g.last, w.instance);
        }
    }
    auto rows = nlohmann::json::array();
    for (const auto& [key, g] : groups)
    {
        if (rows.size() == static_cast<size_t>(top)) break;
        const auto [se, cu, simd] = key;
        rows.push_back({
            {"se",                     se                                                             },
            {"cu",                     cu < 0 ? nlohmann::json(nullptr) : nlohmann::json(cu)          },
            {"simd",                   simd                                                           },
            {"wave_count",             g.waves                                                        },
            {"slot_count",             g.slots.size()                                                 },
            {"instance_min",           g.first < 0 ? nlohmann::json(nullptr) : nlohmann::json(g.first)},
            {"instance_max",           g.last < 0 ? nlohmann::json(nullptr) : nlohmann::json(g.last)  },
            {"unknown_instance_waves", g.unknown_instances                                            }
        });
    }
    int64_t starts = 0, recorded_starts = 0;
    bool recorded_available = !d.occupancy.per_simd.empty();
    for (const auto& g : d.occupancy.per_simd)
    {
        starts += g.wave_starts;
        if (g.recorded_wave_starts < 0)
            recorded_available = false;
        else
            recorded_starts += g.recorded_wave_starts;
    }
    return {
        {"basis",                           "instruction_traced_waves; not hardware utilization"                          },
        {"instance_range_scope",            "slot_local_values_aggregated"                                                },
        {"instance_range_note",
         "Instance min/max aggregate slot-local values across slots; use wave_count for total traced waves."              },
        {"instruction_traced_waves",        d.waves.size()                                                                },
        {"known_cu_waves",                  known                                                                         },
        {"unknown_cu_waves",                d.waves.size() - known                                                        },
        {"known_cu_count",                  cus.size()                                                                    },
        {"total_groups",                    groups.size()                                                                 },
        {"truncated",                       groups.size() > rows.size()                                                   },
        {"occupancy_wave_starts",           recorded_available ? nlohmann::json(recorded_starts) : nlohmann::json(nullptr)},
        {"occupancy_wave_starts_in_window",
         d.occupancy.per_simd.empty() ? nlohmann::json(nullptr) : nlohmann::json(starts)                                  },
        {"occupancy_simd_groups",           d.occupancy.per_simd.size()                                                   },
        {"occupancy_window",                {d.occupancy.t0, d.occupancy.t1}                                              },
        {"groups",                          rows                                                                          }
    };
}

std::string renderCoverage(const Digest& d, int top)
{
    const auto j = coverageJson(d, top);
    std::vector<std::vector<std::string>> rows;
    for (const auto& g : j["groups"])
        rows.push_back(
            {g["se"].dump(),
             g["cu"].dump(),
             g["simd"].dump(),
             g["wave_count"].dump(),
             g["slot_count"].dump(),
             g["instance_min"].dump(),
             g["instance_max"].dump(),
             g["unknown_instance_waves"].dump()}
        );
    return "instruction-traced waves: " + j["instruction_traced_waves"].dump() +
           "; groups: " + j["total_groups"].dump() + "; truncated: " + j["truncated"].dump() +
           "\nNot hardware utilization; null identity/range means unavailable. Instance min/max aggregate slot-local "
           "values across slots; use waves for the total.\n" +
           "Separate occupancy population: " + j["occupancy_wave_starts"].dump() + " recorded starts; " +
           j["occupancy_wave_starts_in_window"].dump() + " in [t0,t1), " + j["occupancy_simd_groups"].dump() +
           " SE/CU/SIMD groups.\n" +
           renderTable(
               {"SE", "CU", "SIMD", "waves", "slots", "instance_min", "instance_max", "unknown_instances"}, rows
           );
}

nlohmann::json groupedOccupancyJson(const Digest& d, bool by_simd, int se, int cu, int simd, int top)
{
    if (top < 1 || top > MaxHotspotRows) throw std::invalid_argument("occupancy top must be in [1, 1000]");
    auto rows = nlohmann::json::array();
    size_t matched = 0;
    const auto& groups = by_simd ? d.occupancy.per_simd : d.occupancy.per_cu;
    for (const auto& g : groups)
    {
        if ((se >= 0 && se != g.se) || (cu >= 0 && cu != g.cu) || (simd >= 0 && simd != g.simd)) continue;
        ++matched;
        if (rows.size() >= static_cast<size_t>(top)) continue;
        rows.push_back({
            {"se",          g.se                                                                     },
            {"cu",          g.cu                                                                     },
            {"simd",        by_simd ? nlohmann::json(g.simd) : nlohmann::json(nullptr)               },
            {"available",   g.available                                                              },
            {"wave_starts", g.wave_starts                                                            },
            {"peak_waves",  g.available ? nlohmann::json(g.peak_waves) : nlohmann::json(nullptr)     },
            {"mean_waves",  g.available ? nlohmann::json(g.mean_waves) : nlohmann::json(nullptr)     },
            {"note",        g.available ? "" : "invalid/unbalanced event stream or empty time window"}
        });
    }
    const bool available = d.occupancy.granular_present && !groups.empty();
    return {
        {"basis",        "recorded resident waves; exact peak and time-weighted mean, not utilization"},
        {"by",           by_simd ? "simd" : "cu"                                                      },
        {"available",    available                                                                    },
        {"note",
         !d.occupancy.granular_present ? "granular data unavailable; rerun analyze"
         : groups.empty()              ? "no occupancy events recorded"
                                       : "CU ids retain capture encoding; window is [t0,t1)"          },
        {"t0",           d.occupancy.t0                                                               },
        {"t1",           d.occupancy.t1                                                               },
        {"total_groups", matched                                                                      },
        {"truncated",    matched > rows.size()                                                        },
        {"groups",       rows                                                                         }
    };
}

std::string renderGroupedOccupancy(const nlohmann::json& j)
{
    std::vector<std::vector<std::string>> rows;
    for (const auto& g : j["groups"])
        rows.push_back(
            {g["se"].dump(),
             g["cu"].dump(),
             g["simd"].dump(),
             g["available"].dump(),
             g["peak_waves"].dump(),
             g["mean_waves"].dump(),
             g["wave_starts"].dump()}
        );
    return j["basis"].get<std::string>() + "\n" + j["note"].get<std::string>() + "\nwindow: [" + j["t0"].dump() + ", " +
           j["t1"].dump() + "); groups: " + j["total_groups"].dump() + "; truncated: " + j["truncated"].dump() + "\n" +
           renderTable({"SE", "CU", "SIMD", "available", "peak_waves", "mean_waves", "starts"}, rows);
}

} // namespace rcv
