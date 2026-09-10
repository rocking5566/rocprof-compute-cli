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

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "analysis/hidden_latency.h"
#include "cli/digest.h"
#include "cli/digest_builder.h"
#include "cli/format.h"
#include "cli/queries.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"

namespace
{
int usage()
{
    std::cerr << "usage: rcv-cli <command> [options]\n"
                 "  analyze <trace_path> [-o digest.json] [--bins N]\n"
                 "  summary [-d digest.json] [--json]\n";
    return 2;
}

bool readDigest(const std::string& path, rcv::Digest& out, std::string& error)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        error = "cannot read digest " + path + " (run 'rcv-cli analyze' first)";
        return false;
    }
    try
    {
        out = rcv::fromJson(nlohmann::json::parse(in));
    }
    catch (const std::exception& e)
    {
        error = std::string("malformed digest: ") + e.what();
        return false;
    }
    return true;
}

int cmdAnalyze(const std::vector<std::string>& args)
{
    if (args.empty()) return usage();

    std::string trace_path = args[0];
    std::string out_path = "digest.json";
    int bins = 200;
    for (size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "-o" && i + 1 < args.size())
            out_path = args[++i];
        else if (args[i] == "--bins" && i + 1 < args.size())
            bins = std::stoi(args[++i]);
        else
            return usage();
    }

    DataStore store;
    const auto load = rcv::loadTrace(trace_path, store);
    if (!load.ok)
    {
        std::cerr << "error: " << load.error << "\n";
        return 1;
    }
    for (const auto& w : load.warnings) std::cerr << "warning: " << w << "\n";

    if (!rcv::tokenTypesSupportHiddenLatency())
    {
        std::cerr << "error: the active instruction-type list has no VALU or MATRIX entry, so "
                     "hidden latency cannot be computed. A token_def.json in the current "
                     "directory has replaced the built-in defaults; run from elsewhere or "
                     "remove it.\n";
        return 1;
    }

    if (!HiddenLatencyAnalysis::analyze(store))
        std::cerr << "warning: hidden latency analysis failed; hidden columns will be zero\n";

    const auto digest = rcv::buildDigest(store, trace_path, bins);

    std::ofstream out(out_path);
    if (!out.is_open())
    {
        std::cerr << "error: cannot write " << out_path << "\n";
        return 1;
    }
    out << rcv::toJson(digest).dump();

    std::cerr << "wrote " << out_path << ": " << digest.lines.size() << " lines, " << digest.waves.size() << " waves\n";
    return 0;
}

int cmdSummary(const std::vector<std::string>& args)
{
    std::string digest_path = "digest.json";
    bool as_json = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-d" && i + 1 < args.size())
            digest_path = args[++i];
        else if (args[i] == "--json")
            as_json = true;
        else
            return usage();
    }

    rcv::Digest digest;
    std::string error;
    if (!readDigest(digest_path, digest, error))
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (as_json)
        std::cout << rcv::summaryJson(digest).dump(2) << "\n";
    else
        std::cout << rcv::renderSummary(digest);
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();

    const std::string command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "analyze") return cmdAnalyze(rest);
    if (command == "summary") return cmdSummary(rest);

    return usage();
}
