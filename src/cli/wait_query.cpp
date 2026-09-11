// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#include "cli/wait_query.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "cli/format.h"
#include "data/datastore.h"
#include "data/wavemanager.h"

namespace rcv
{
namespace
{
struct Observation
{
    int64_t clock = 0, stall = 0, cycles = 0;
    int code_line = -1;
    size_t iteration = 0;
};

// The display Token narrows stall to 16 bits. Query the original instruction
// records instead; the decoder supports 24-bit stalls. This keeps full-width
// query storage local to one wave without enlarging all GUI/analysis tokens.
std::vector<Observation> observations(const DataStore& store, const WaveEntry& entry)
{
    std::vector<Observation> result;
    if (const auto rec = store.wave_records.find(entry.id); rec != store.wave_records.end())
    {
        result.reserve(rec->second.instructions.size());
        for (const auto& i : rec->second.instructions)
            result.push_back({i.time, i.stall, std::max(i.stall, i.duration), i.line_number});
    }
    else
    {
        std::ifstream in(store.ui_dir + entry.id);
        if (!in) throw std::runtime_error("cannot read selected wave instructions");
        const auto data = nlohmann::json::parse(in);
        for (const auto& i : data.at("wave").at("instructions"))
        {
            const auto stall = i.at(2).get<int64_t>();
            result.push_back(
                {i.at(0).get<int64_t>() + entry.time_offset,
                 stall,
                 std::max(stall, i.at(3).get<int64_t>()),
                 i.at(4).get<int>()}
            );
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.clock < b.clock; });
    std::map<int, size_t> iterations;
    for (auto& i : result)
    {
        if (i.stall < 0 || i.cycles < 0) throw std::runtime_error("negative instruction stall/duration");
        i.iteration = iterations[i.code_line]++;
    }
    return result;
}
} // namespace

nlohmann::json queryWait(DataStore& store, const WaveSelection& selected, int line, int top, int context)
{
    if (line < 0 || top < 1 || top > 20 || context < 0 || context > 10)
        throw std::invalid_argument("wait query bounds: line >= 0, top 1..20, context 0..10");
    const auto& entry = store.wave_hierarchy.at(selected.se).at(selected.simd).at(selected.slot).at(selected.instance);
    auto wave = store.getWave(entry);
    if (!wave || !wave->load_complete) throw std::runtime_error("selected wave is incomplete");
    if (selected.cu >= 0 && selected.cu != wave->cu) throw std::runtime_error("selected wave CU mismatch");
    std::map<int, std::string> instructions;
    for (const auto& code : store.code)
        if (code.line) instructions.emplace(code.line->index.load(), code.line->inst);
    const auto found = instructions.find(line);
    if (found == instructions.end()) throw std::runtime_error("ASM line not found: " + std::to_string(line));
    const auto begin = found->second.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || found->second.compare(begin, 6, "s_wait") != 0)
        throw std::runtime_error("selected ASM line is not a wait instruction");

    const auto tokens = observations(store, entry);
    std::vector<size_t> hits;
    std::vector<int64_t> stalls;
    long double mean = 0, m2 = 0;
    int64_t maximum = 0;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const auto& token = tokens[i];
        if (token.code_line != line) continue;
        hits.push_back(i);
        stalls.push_back(token.stall);
        const long double delta = token.stall - mean;
        mean += delta / hits.size();
        m2 += delta * (token.stall - mean);
        maximum = std::max(maximum, token.stall);
    }
    if (hits.empty()) throw std::runtime_error("selected wait line was not executed in this wave");
    const size_t count = hits.size();
    const size_t p95_index = count - count / 20 - 1; // ceil(0.95*N)-1 without floating rounding
    std::nth_element(stalls.begin(), stalls.begin() + p95_index, stalls.end());
    const auto p95 = stalls[p95_index];
    const size_t shown = std::min(count, static_cast<size_t>(top));
    std::partial_sort(
        hits.begin(),
        hits.begin() + shown,
        hits.end(),
        [&](size_t a, size_t b)
        { return tokens[a].stall == tokens[b].stall ? a < b : tokens[a].stall > tokens[b].stall; }
    );
    auto tokenJson = [&](size_t i)
    {
        const auto& t = tokens[i];
        const auto inst = instructions.find(t.code_line);
        return nlohmann::json{
            {"token_index", i                                             },
            {"line",        t.code_line                                   },
            {"iteration",   t.iteration                                   },
            {"clock",       t.clock                                       },
            {"end_clock",   t.clock + t.cycles                            },
            {"stall",       t.stall                                       },
            {"cycles",      t.cycles                                      },
            {"inst",        inst == instructions.end() ? "" : inst->second}
        };
    };
    auto occurrences = nlohmann::json::array();
    for (size_t h = 0; h < shown; ++h)
    {
        const auto i = hits[h];
        auto occurrence = tokenJson(i);
        auto rows = nlohmann::json::array();
        const auto first = i > static_cast<size_t>(context) ? i - context : 0;
        const auto last = std::min(tokens.size(), i + context + 1);
        for (size_t k = first; k < last; ++k)
        {
            auto row = tokenJson(k);
            row["selected"] = k == i;
            rows.push_back(std::move(row));
        }
        occurrence["context"] = std::move(rows);
        occurrences.push_back(std::move(occurrence));
    }

    const bool from_att = store.wave_records.contains(entry.id);
    if (from_att) wave->buildWaitcnt(store.gfxip);
    // These references describe the static wait site, not a dynamic pairing
    // with each occurrence above. Inferred ATT iteration fields are placeholders.
    std::set<std::pair<int, int>> references;
    for (const auto& wait : wave->waitcnt)
        if (wait.code_line == line) references.insert(wait.sources.begin(), wait.sources.end());
    auto deps = nlohmann::json::array();
    for (const auto& [dep, iteration] : references)
    {
        if (deps.size() >= 32) break;
        const auto inst = instructions.find(dep);
        deps.push_back({
            {"line",      dep                                           },
            {"iteration", iteration                                     },
            {"inst",      inst == instructions.end() ? "" : inst->second}
        });
    }
    return {
        {"metric",                "observed wait stall"                                        },
        {"unit",                  "cycles"                                                     },
        {"clock_basis",           "aligned trace cycles"                                       },
        {"line",                  line                                                         },
        {"inst",                  found->second                                                },
        {"wave",
         {{"se", selected.se},
          {"cu", wave->cu},
          {"simd", selected.simd},
          {"slot", selected.slot},
          {"instance", selected.instance},
          {"begin", wave->WaveBegin()},
          {"end", wave->WaveEnd()}}                                                            },
        {"statistics",
         {{"count", count},
          {"mean", static_cast<double>(mean)},
          {"stddev", static_cast<double>(std::sqrt(m2 / count))},
          {"p95", p95},
          {"max", maximum},
          {"stddev_basis", "population"},
          {"p95_basis", "nearest_rank"}}                                                       },
        {"occurrences_truncated", count > shown                                                },
        {"occurrences",           occurrences                                                  },
        {"dependencies",
         {{"provenance", from_att ? "inferred_att_waitcnt" : "recorded_json"},
          {"available", !references.empty()},
          {"total_references", references.size()},
          {"truncated", references.size() > deps.size()},
          {"references", deps},
          {"note",
           "Static line/iteration references, not per-occurrence dependency matches; "
           "ATT iteration fields are placeholders. Empty means unavailable/empty."}}           },
        {"note",
         "Observed wait stalls are not measured memory-completion latency or predicted speedup. "
         "Contexts are time-ordered, preserving recorded order at tied timestamps. "
         "Wave instance is local to SE/SIMD/slot; input and ASM listing must remain unchanged."}
    };
}

std::string renderWait(const nlohmann::json& j)
{
    std::ostringstream out;
    out << "line " << j["line"] << ": " << j["inst"].get<std::string>() << "\nwave: " << j["wave"].dump()
        << "\nobserved wait stall (cycles), aligned timestamps\n"
        << j["statistics"].dump() << "\n"
        << j["note"].get<std::string>() << "\n";
    for (const auto& hit : j["occurrences"])
    {
        out << "\niteration " << hit["iteration"] << ", clock " << hit["clock"] << ", stall " << hit["stall"] << "\n";
        std::vector<std::vector<std::string>> rows;
        for (const auto& t : hit["context"])
            rows.push_back(
                {t["selected"].get<bool>() ? ">" : "",
                 t["line"].dump(),
                 t["iteration"].dump(),
                 t["clock"].dump(),
                 t["end_clock"].dump(),
                 t["stall"].dump(),
                 t["inst"].get<std::string>()}
            );
        out << renderTable({"", "line", "iter", "clock", "end", "stall", "instruction"}, rows);
    }
    out << "occurrences truncated: " << j["occurrences_truncated"]
        << "\ndependencies: " << j["dependencies"]["provenance"].get<std::string>() << "\n";
    std::vector<std::vector<std::string>> rows;
    for (const auto& d : j["dependencies"]["references"])
        rows.push_back({d["line"].dump(), d["iteration"].dump(), d["inst"].get<std::string>()});
    out << renderTable({"line", "iteration_ref", "instruction"}, rows)
        << "references: " << j["dependencies"]["total_references"] << "; truncated: " << j["dependencies"]["truncated"]
        << "\n"
        << j["dependencies"]["note"].get<std::string>() << "\n";
    return out.str();
}
} // namespace rcv
