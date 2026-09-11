// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#include "cli/compare.h"

#include <cmath>
#include <map>
#include <sstream>
#include <tuple>
#include "cli/format.h"

namespace rcv
{
namespace
{
using nlohmann::json;

json change(const json& before, const json& after)
{
    json delta = nullptr, percent = nullptr;
    if (before.is_number() && after.is_number())
    {
        if (before.is_number_integer() && after.is_number_integer())
            delta = after.get<int64_t>() - before.get<int64_t>();
        else
            delta = after.get<double>() - before.get<double>();
        if (before.get<double>() != 0) percent = 100.0 * delta.get<double>() / before.get<double>();
    }
    return {
        {"before",  before },
        {"after",   after  },
        {"delta",   delta  },
        {"percent", percent}
    };
}

json metrics(const HotspotRow& row, bool hidden_available)
{
    return {
        {"hitcount", row.hitcount                                          },
        {"issue",    row.issue()                                           },
        {"stall",    row.stall                                             },
        {"idle",     row.idle                                              },
        {"total",    row.total()                                           },
        {"hidden",   hidden_available ? json(row.hidden) : json(nullptr)   },
        {"exposed",  hidden_available ? json(row.exposed()) : json(nullptr)}
    };
}

json changes(const json& before, const json& after)
{
    auto result = json::object();
    for (const auto& [key, value] : before.items()) result[key] = change(value, after.at(key));
    return result;
}

json perWave(json values, size_t count)
{
    for (auto& value : values) value = count && value.is_number() ? json(value.get<double>() / count) : json(nullptr);
    return values;
}

auto population(const Digest& d)
{
    std::map<std::tuple<int, int, int, int>, size_t> result;
    for (const auto& w : d.waves) ++result[{w.se, w.cu, w.simd, w.slot}];
    return result;
}
} // namespace

nlohmann::json compareDigests(const Digest& before, const Digest& after, int top)
{
    if (top < 1 || top > MaxHotspotRows) throw std::invalid_argument("compare top must be in [1, 1000]");
    const auto b = summaryJson(before), a = summaryJson(after);
    json warnings =
        json::array({"Workload and decoder provenance are not stored in these digests; comparability is unverified."});
    if (before.meta.gfxip != after.meta.gfxip || before.meta.gfxv != after.meta.gfxv)
        warnings.push_back("GPU metadata differs.");
    if (before.meta.kernel_name != after.meta.kernel_name)
        warnings.push_back("Kernel names differ; opcode grouping does not establish equivalent work.");
    if (population(before) != population(after))
        warnings.push_back("Instruction-traced wave counts or SE/CU/SIMD/slot populations differ.");
    if (b["coverage"]["unknown_cu_waves"] != 0 || a["coverage"]["unknown_cu_waves"] != 0)
        warnings.push_back("Some traced waves have unknown CU identity.");
    if (before.occupancy.bins != after.occupancy.bins)
        warnings.push_back("Occupancy bin counts differ; binned peaks have different resolution.");
    const bool hidden = before.meta.hidden_latency_available && after.meta.hidden_latency_available;
    const std::string sort_metric = hidden ? "exposed" : "stall";
    if (!hidden)
        warnings.push_back("Hidden/exposed costs are unavailable on at least one side; ranking uses stall changes.");

    struct Pair
    {
        HotspotRow before, after;
        bool has_before = false, has_after = false;
    };
    std::map<std::string, Pair> pairs;
    for (const auto& r : aggregateHotspots(before, GroupBy::Opcode))
    {
        pairs[r.key].before = r;
        pairs[r.key].has_before = true;
    }
    for (const auto& r : aggregateHotspots(after, GroupBy::Opcode))
    {
        pairs[r.key].after = r;
        pairs[r.key].has_after = true;
    }
    std::vector<json> rows;
    for (const auto& [opcode, pair] : pairs)
    {
        const auto bm = metrics(pair.before, before.meta.hidden_latency_available);
        const auto am = metrics(pair.after, after.meta.hidden_latency_available);
        rows.push_back({
            {"opcode", opcode},
            {"presence",
             !pair.has_before  ? "added"
             : !pair.has_after ? "removed"
                               : "both"},
            {"metrics", changes(bm, am)},
            {"per_traced_wave", changes(perWave(bm, before.waves.size()), perWave(am, after.waves.size()))}
        });
    }
    // Map iteration supplies a stable opcode tie-breaker. Match ALL opcodes
    // before selecting the largest changes, never just each side's top list.
    std::stable_sort(
        rows.begin(),
        rows.end(),
        [&](const auto& x, const auto& y)
        {
            return std::abs(x.at("metrics").at(sort_metric).at("delta").template get<double>()) >
                   std::abs(y.at("metrics").at(sort_metric).at("delta").template get<double>());
        }
    );
    if (rows.size() > static_cast<size_t>(top)) rows.resize(top);

    auto bc = b["cycles"], ac = a["cycles"];
    if (!before.meta.hidden_latency_available) bc["hidden"] = bc["exposed"] = nullptr;
    if (!after.meta.hidden_latency_available) ac["hidden"] = ac["exposed"] = nullptr;
    auto coverage = json::object();
    for (const auto* key :
         {"instruction_traced_waves",
          "known_cu_waves",
          "unknown_cu_waves",
          "known_cu_count",
          "total_groups",
          "occupancy_wave_starts",
          "occupancy_wave_starts_in_window",
          "occupancy_simd_groups"})
        coverage[key] = change(b["coverage"][key], a["coverage"][key]);
    return {
        {"group_by", "opcode"},
        {"delta_direction", "after - before"},
        {"unit", "cycles (hitcount is executions)"},
        {"before",
         {{"meta", b["meta"]},
          {"digest_version", before.version},
          {"workload", nullptr},
          {"decoder", nullptr},
          {"occupancy_window", {before.occupancy.t0, before.occupancy.t1}}}},
        {"after",
         {{"meta", a["meta"]},
          {"digest_version", after.version},
          {"workload", nullptr},
          {"decoder", nullptr},
          {"occupancy_window", {after.occupancy.t0, after.occupancy.t1}}}},
        {"comparability", {{"status", "unverified"}, {"warnings", warnings}}},
        {"cycles", changes(bc, ac)},
        {"cycles_per_traced_wave", changes(perWave(bc, before.waves.size()), perWave(ac, after.waves.size()))},
        {"coverage", coverage},
        {"occupancy",
         {{"basis", "binned_total_concurrency"},
          {"before_available", b["occupancy"]["available"]},
          {"after_available", a["occupancy"]["available"]},
          {"bins", change(b["occupancy"]["bins"], a["occupancy"]["bins"])},
          {"peak_waves", change(b["occupancy"]["peak_waves"], a["occupancy"]["peak_waves"])},
          {"mean_waves", change(b["occupancy"]["mean_waves"], a["occupancy"]["mean_waves"])}}},
        {"sort_metric", sort_metric},
        {"sort_basis", "absolute raw delta descending; opcode ascending on ties"},
        {"total_opcodes", pairs.size()},
        {"truncated", pairs.size() > rows.size()},
        {"opcodes", rows},
        {"note",
         "Attributed cycle differences are not kernel speedup. Per-traced-wave values normalize only the recorded "
         "wave count, not workload or sampling bias. Occupancy describes a separate population; its peak is a bin "
         "mean, not an instantaneous peak. Missing metrics and percentages with a zero baseline are null."}
    };
}

std::string renderCompare(const nlohmann::json& j)
{
    std::ostringstream out;
    out << "A/B comparison: after - before; cycles unless labeled hits\n";
    for (const auto* side : {"before", "after"}) out << side << ": " << j[side].dump() << '\n';
    for (const auto& warning : j["comparability"]["warnings"]) out << "note: " << warning.get<std::string>() << '\n';
    auto table = [&](const std::string& title, const json& values)
    {
        out << '\n' << title << '\n';
        std::vector<std::vector<std::string>> rows;
        for (const auto& [key, value] : values.items())
            rows.push_back(
                {key, value["before"].dump(), value["after"].dump(), value["delta"].dump(), value["percent"].dump()}
            );
        out << renderTable({"metric", "before", "after", "delta", "change %"}, rows);
    };
    table("Attributed cycles", j["cycles"]);
    table("Cycles per instruction-traced wave", j["cycles_per_traced_wave"]);
    table("Coverage", j["coverage"]);
    table(
        "Occupancy (binned concurrency)",
        {
            {"bins",       j["occupancy"]["bins"]      },
            {"peak_waves", j["occupancy"]["peak_waves"]},
            {"mean_waves", j["occupancy"]["mean_waves"]}
    }
    );
    out << "\nOpcodes ranked by absolute " << j["sort_metric"].get<std::string>() << " delta\n";
    std::vector<std::vector<std::string>> rows;
    for (const auto& r : j["opcodes"])
    {
        const auto& m = r["metrics"];
        rows.push_back(
            {r["opcode"],
             r["presence"],
             m["hitcount"]["delta"].dump(),
             m["stall"]["delta"].dump(),
             m["idle"]["delta"].dump(),
             m["exposed"]["before"].dump(),
             m["exposed"]["after"].dump(),
             m["exposed"]["delta"].dump(),
             r["per_traced_wave"]["exposed"]["delta"].dump()}
        );
    }
    out << renderTable(
               {"opcode",
                "presence",
                "hits delta",
                "stall delta",
                "idle delta",
                "exposed before",
                "exposed after",
                "exposed delta",
                "exposed/wave delta"},
               rows
           )
        << "opcodes: " << j["total_opcodes"] << "; truncated: " << j["truncated"] << '\n'
        << j["note"].get<std::string>() << '\n';
    return out.str();
}
} // namespace rcv
