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

void validateIterations(const IterationRange& range)
{
    if (range && (range->first < 0 || range->second < range->first))
        throw std::invalid_argument("iterations must be an ascending nonnegative inclusive range");
}

bool inRange(size_t iteration, const IterationRange& range)
{
    return !range ||
           (iteration >= static_cast<size_t>(range->first) && iteration <= static_cast<size_t>(range->second));
}

nlohmann::json iterationJson(const IterationRange& range)
{
    return range ? nlohmann::json{range->first, range->second} : nlohmann::json(nullptr);
}

nlohmann::json waitStatistics(std::vector<int64_t> stalls)
{
    using nlohmann::json;
    const size_t count = stalls.size();
    json result = {
        {"count",        count         },
        {"mean",         nullptr       },
        {"stddev",       nullptr       },
        {"p95",          nullptr       },
        {"max",          nullptr       },
        {"stddev_basis", "population"  },
        {"p95_basis",    "nearest_rank"}
    };
    if (!count) return result;
    long double mean = 0, m2 = 0;
    size_t n = 0;
    int64_t maximum = 0;
    for (auto stall : stalls)
    {
        const long double delta = stall - mean;
        mean += delta / ++n;
        m2 += delta * (stall - mean);
        maximum = std::max(maximum, stall);
    }
    const size_t p95_index = count - count / 20 - 1;
    std::nth_element(stalls.begin(), stalls.begin() + p95_index, stalls.end());
    result["mean"] = static_cast<double>(mean);
    result["stddev"] = static_cast<double>(std::sqrt(m2 / count));
    result["p95"] = stalls[p95_index];
    result["max"] = maximum;
    return result;
}

std::string waitInstruction(const DataStore& store, int line)
{
    for (const auto& code : store.code)
    {
        if (!code.line || code.line->index.load() != line) continue;
        const auto& inst = code.line->inst;
        const auto begin = inst.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos || inst.compare(begin, 6, "s_wait") != 0)
            throw std::runtime_error("selected ASM line is not a wait instruction");
        return inst;
    }
    throw std::runtime_error("ASM line not found: " + std::to_string(line));
}
} // namespace

nlohmann::json queryWait(
    DataStore& store, const WaveSelection& selected, int line, int top, int context, IterationRange iterations
)
{
    if (line < 0 || top < 1 || top > 20 || context < 0 || context > 10)
        throw std::invalid_argument("wait query bounds: line >= 0, top 1..20, context 0..10");
    validateIterations(iterations);
    const auto instruction = waitInstruction(store, line);
    const auto& entry = store.wave_hierarchy.at(selected.se).at(selected.simd).at(selected.slot).at(selected.instance);
    auto wave = store.getWave(entry);
    if (!wave || !wave->load_complete) throw std::runtime_error("selected wave is incomplete");
    if (selected.cu >= 0 && selected.cu != wave->cu) throw std::runtime_error("selected wave CU mismatch");
    std::map<int, std::string> instructions;
    for (const auto& code : store.code)
        if (code.line) instructions.emplace(code.line->index.load(), code.line->inst);
    const auto tokens = observations(store, entry);
    std::vector<size_t> hits;
    std::vector<int64_t> stalls;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const auto& token = tokens[i];
        if (token.code_line != line || !inRange(token.iteration, iterations)) continue;
        hits.push_back(i);
        stalls.push_back(token.stall);
    }
    if (hits.empty()) throw std::runtime_error("selected wait line was not executed in this wave/iteration range");
    const size_t count = hits.size();
    const auto statistics = waitStatistics(std::move(stalls));
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
        {"inst",                  instruction                                                  },
        {"iterations",            iterationJson(iterations)                                    },
        {"iteration_basis",       "zero-based occurrences of selected line, inclusive range"   },
        {"wave",
         {{"se", selected.se},
          {"cu", wave->cu},
          {"simd", selected.simd},
          {"slot", selected.slot},
          {"instance", selected.instance},
          {"begin", wave->WaveBegin()},
          {"end", wave->WaveEnd()}}                                                            },
        {"statistics",            statistics                                                   },
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
        << "iterations: " << j["iterations"] << " (null = all; zero-based selected-line occurrences)\n"
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

nlohmann::json queryWaitSummary(
    DataStore& store, const WaveSelection& filters, int line, int max_waves, IterationRange iterations
)
{
    if (line < 0 || max_waves < 1 || max_waves > 128 || filters.cu != -1)
        throw std::invalid_argument("wait-summary requires line >= 0, max-waves 1..128; CU filtering is unsupported");
    validateIterations(iterations);
    const auto instruction = waitInstruction(store, line);
    auto rows = nlohmann::json::array();
    size_t matching = 0, executed = 0;
    store.forEachWave(
        [&](const DataStore::WaveCoordinate& coord, const WaveEntry& entry)
        {
            if ((filters.se >= 0 && coord.hwid.se != filters.se) ||
                (filters.simd >= 0 && coord.hwid.simd != filters.simd) ||
                (filters.slot >= 0 && coord.hwid.slot != filters.slot) ||
                (filters.instance >= 0 && coord.instance != filters.instance))
                return;
            ++matching;
            if (rows.size() >= static_cast<size_t>(max_waves)) return;
            const auto wave = store.getWave(entry);
            if (!wave || !wave->load_complete) throw std::runtime_error("could not load complete wave: " + entry.id);
            const auto tokens = observations(store, entry);
            std::vector<int64_t> stalls;
            size_t total_occurrences = 0;
            for (const auto& token : tokens)
            {
                if (token.code_line != line) continue;
                ++total_occurrences;
                if (inRange(token.iteration, iterations)) stalls.push_back(token.stall);
            }
            const bool available = !stalls.empty();
            if (available) ++executed;
            rows.push_back({
                {"wave",
                 {{"se", coord.hwid.se},
                  {"cu", wave->cu < 0 ? nlohmann::json(nullptr) : nlohmann::json(wave->cu)},
                  {"simd", coord.hwid.simd},
                  {"slot", coord.hwid.slot},
                  {"instance", coord.instance},
                  {"begin", wave->WaveBegin()},
                  {"end", wave->WaveEnd()}}                            },
                {"available",         available                        },
                {"status",
                 available           ? "observed"
                 : total_occurrences ? "no_occurrences_in_range"
                                     : "not_executed"                  },
                {"total_occurrences", total_occurrences                },
                {"statistics",        waitStatistics(std::move(stalls))}
            });
        }
    );
    if (!matching) throw std::runtime_error("no waves match the SE/SIMD/slot/instance filters");
    return {
        {"metric",                  "observed wait stall"                                                                               },
        {"unit",                    "cycles"                                                                                            },
        {"line",                    line                                                                                                },
        {"inst",                    instruction                                                                                         },
        {"iterations",              iterationJson(iterations)                                                                           },
        {"iteration_basis",         "zero-based occurrences of selected line in each wave; inclusive, not source-loop IDs"              },
        {"selection_order",         "SE/SIMD/slot/instance ascending; first matching waves, not a random sample"                        },
        {"filters",                 {{"se", filters.se}, {"simd", filters.simd}, {"slot", filters.slot}, {"instance", filters.instance}}
        },
        {"max_waves",               max_waves                                                                                           },
        {"matching_waves",          matching                                                                                            },
        {"selected_waves",          rows.size()                                                                                         },
        {"waves_with_observations", executed                                                                                            },
        {"truncated",               matching > rows.size()                                                                              },
        {"waves",                   rows                                                                                                },
        {"note",
         "Statistics cover only the selected waves and iteration range. Empty ranges/unexecuted lines have "
         "null statistics, not measured zero. Observed stalls are not memory-completion latency or predicted "
         "speedup. Wave instance is local to SE/SIMD/slot. Raw ATT is decoded capture-wide once per command; "
         "output and wave materialization are bounded, raw decoding is not."                                                            }
    };
}

std::string renderWaitSummary(const nlohmann::json& j)
{
    std::ostringstream out;
    out << "line " << j["line"] << ": " << j["inst"].get<std::string>() << "\nobserved wait stall (cycles)\n"
        << "iterations: " << j["iterations"] << " (null = all); " << j["iteration_basis"].get<std::string>() << '\n'
        << "selection: " << j["selection_order"].get<std::string>() << '\n';
    std::vector<std::vector<std::string>> rows;
    for (const auto& row : j["waves"])
    {
        const auto& w = row["wave"];
        const auto& s = row["statistics"];
        rows.push_back(
            {w["se"].dump(),
             w["cu"].dump(),
             w["simd"].dump(),
             w["slot"].dump(),
             w["instance"].dump(),
             s["count"].dump(),
             s["mean"].dump(),
             s["stddev"].dump(),
             s["p95"].dump(),
             s["max"].dump(),
             row["status"]}
        );
    }
    out << renderTable({"SE", "CU", "SIMD", "slot", "wave", "count", "mean", "stddev", "P95", "max", "status"}, rows)
        << "selected " << j["selected_waves"] << '/' << j["matching_waves"]
        << " matching waves; truncated: " << j["truncated"] << "; with observations: " << j["waves_with_observations"]
        << '\n'
        << j["note"].get<std::string>() << '\n';
    return out.str();
}
} // namespace rcv
