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

#include "cli/digest.h"

namespace rcv
{

nlohmann::json toJson(const Digest& d)
{
    nlohmann::json j;
    j["version"] = d.version;
    j["meta"] = {
        {"gfxip",                    d.meta.gfxip                   },
        {"gfxv",                     d.meta.gfxv                    },
        {"trace_path",               d.meta.trace_path              },
        {"kernel_name",              d.meta.kernel_name             },
        {"wave_count",               d.meta.wave_count              },
        {"se_count",                 d.meta.se_count                },
        {"trace_begin",              d.meta.trace_begin             },
        {"trace_end",                d.meta.trace_end               },
        {"lines_with_source",        d.meta.lines_with_source       },
        {"total_lines",              d.meta.total_lines             },
        {"hidden_latency_available", d.meta.hidden_latency_available},
    };
    j["type_names"] = d.type_names;
    j["stall_reason_names"] = d.stall_reason_names;

    auto& lines = j["lines"] = nlohmann::json::array();
    for (const auto& l : d.lines)
        lines.push_back({
            {"i",     l.index                                                               },
            {"addr",  l.addr                                                                },
            {"obj",   l.codeobj_id                                                          },
            {"inst",  l.inst                                                                },
            {"src",   l.cppline                                                             },
            {"type",  l.type                                                                },
            {"hit",   l.hitcount                                                            },
            {"lat",   l.latency                                                             },
            {"stall", l.stall                                                               },
            {"idle",  l.idle                                                                },
            {"hid",   nlohmann::json::array({l.hidden_idle, l.hidden_stall, l.hidden_issue})},
            {"sr",    l.stallreasons                                                        },
        });

    j["occupancy"] = {
        {"bins",   d.occupancy.bins  },
        {"t0",     d.occupancy.t0    },
        {"t1",     d.occupancy.t1    },
        {"per_se", d.occupancy.per_se},
        {"total",  d.occupancy.total },
    };

    auto& waves = j["waves"] = nlohmann::json::array();
    for (const auto& w : d.waves)
        waves.push_back({
            {"se",    w.se   },
            {"cu",    w.cu   },
            {"simd",  w.simd },
            {"slot",  w.slot },
            {"begin", w.begin},
            {"end",   w.end  }
        });

    return j;
}

Digest fromJson(const nlohmann::json& j)
{
    Digest d;
    d.version = j.value("version", 1);

    const auto& m = j.at("meta");
    d.meta.gfxip = m.value("gfxip", 0);
    d.meta.gfxv = m.value("gfxv", std::string{});
    d.meta.trace_path = m.value("trace_path", std::string{});
    d.meta.kernel_name = m.value("kernel_name", std::string{});
    d.meta.wave_count = m.value("wave_count", 0);
    d.meta.se_count = m.value("se_count", 0);
    d.meta.trace_begin = m.value("trace_begin", int64_t{0});
    d.meta.trace_end = m.value("trace_end", int64_t{0});
    d.meta.lines_with_source = m.value("lines_with_source", 0);
    d.meta.total_lines = m.value("total_lines", 0);
    d.meta.hidden_latency_available = m.value("hidden_latency_available", false);

    d.type_names = j.value("type_names", std::vector<std::string>{});
    d.stall_reason_names = j.value("stall_reason_names", std::vector<std::string>{});

    for (const auto& e : j.at("lines"))
    {
        LineDigest l;
        l.index = e.value("i", 0);
        l.addr = e.value("addr", int64_t{0});
        l.codeobj_id = e.value("obj", int64_t{0});
        l.inst = e.value("inst", std::string{});
        l.cppline = e.value("src", std::string{});
        l.type = e.value("type", 0);
        l.hitcount = e.value("hit", int64_t{0});
        l.latency = e.value("lat", int64_t{0});
        l.stall = e.value("stall", int64_t{0});
        l.idle = e.value("idle", int64_t{0});
        const auto hid = e.value("hid", std::vector<int64_t>{0, 0, 0});
        if (hid.size() == 3)
        {
            l.hidden_idle = hid[0];
            l.hidden_stall = hid[1];
            l.hidden_issue = hid[2];
        }
        l.stallreasons = e.value("sr", std::vector<int64_t>{});
        d.lines.push_back(std::move(l));
    }

    if (j.contains("occupancy"))
    {
        const auto& o = j.at("occupancy");
        d.occupancy.bins = o.value("bins", 0);
        d.occupancy.t0 = o.value("t0", int64_t{0});
        d.occupancy.t1 = o.value("t1", int64_t{0});
        d.occupancy.per_se = o.value("per_se", std::map<int, std::vector<double>>{});
        d.occupancy.total = o.value("total", std::vector<double>{});
    }

    if (j.contains("waves"))
        for (const auto& e : j.at("waves"))
            d.waves.push_back(
                {e.value("se", 0),
                 e.value("cu", 0),
                 e.value("simd", 0),
                 e.value("slot", 0),
                 e.value("begin", int64_t{0}),
                 e.value("end", int64_t{0})}
            );

    return d;
}

} // namespace rcv
