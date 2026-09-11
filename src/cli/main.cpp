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

#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include "analysis/hidden_latency.h"
#include "cli/compare.h"
#include "cli/digest.h"
#include "cli/digest_builder.h"
#include "cli/format.h"
#include "cli/queries.h"
#include "cli/wait_query.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"

namespace
{
struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

int integer(const std::string& option, const std::string& text, int minimum, int maximum)
{
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw UsageError(
            option + " expects an integer in [" + std::to_string(minimum) + ", " + std::to_string(maximum) + "]: '" +
            text + "'"
        );
    return value;
}

void validateDestination(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        throw std::runtime_error("cannot inspect output: " + ec.message());
    if (status.type() != std::filesystem::file_type::not_found && !std::filesystem::is_regular_file(status))
        throw std::runtime_error("output must be a regular file, not a symlink, directory or special file");
}

rcv::IterationRange iterationRange(const std::string& text)
{
    const auto dash = text.find('-');
    if (dash == std::string::npos) throw UsageError("--iterations expects A-B (inclusive, zero-based)");
    const int first = integer("--iterations start", text.substr(0, dash), 0, std::numeric_limits<int>::max());
    const int last = integer("--iterations end", text.substr(dash + 1), 0, std::numeric_limits<int>::max());
    if (last < first) throw UsageError("--iterations must be ascending (inclusive)");
    return std::pair{first, last};
}

void writeAtomically(const std::filesystem::path& path, const std::string& contents)
{
    validateDestination(path);
    struct TemporaryFile
    {
        std::string path;
        int fd = -1;
        ~TemporaryFile()
        {
            if (fd >= 0) close(fd);
            if (!path.empty())
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } temp;
    auto pattern = (path.parent_path() / ".rcv-digest-XXXXXX").string();
    temp.fd = mkstemp(pattern.data());
    if (temp.fd < 0) throw std::runtime_error("cannot create output temporary file: " + std::string(strerror(errno)));
    temp.path = std::move(pattern);
    size_t offset = 0;
    while (offset < contents.size())
    {
        const auto written = write(temp.fd, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error("digest write failed: " + std::string(strerror(errno)));
        offset += static_cast<size_t>(written);
    }
    if (fsync(temp.fd) != 0) throw std::runtime_error("digest flush failed: " + std::string(strerror(errno)));
    const int fd = temp.fd;
    temp.fd = -1;
    if (close(fd) != 0) throw std::runtime_error("digest close failed: " + std::string(strerror(errno)));
    validateDestination(path);
    std::filesystem::rename(temp.path, path);
    temp.path.clear();
}

int usage()
{
    std::cerr << "usage: rcv-cli <command> [options]\n"
                 "  analyze <trace_path> [-o digest.json] [--bins N] [--format json|att]\n"
                 "  summary   [-d digest.json] [--json]\n"
                 "  hotspot   [-d digest.json] [--by asm|source|opcode] "
                 "[--sort exposed|total|stall|idle] [--top N] [--json]\n"
                 "  asm       [-d digest.json] (--range A-B | --around N [--context N]) [--json]\n"
                 "  occupancy [-d digest.json] [--se N] [--json]\n"
                 "            [--by cu|simd [--cu N] [--simd N] [--top N]]\n"
                 "  coverage  [-d digest.json] [--top N] [--json]\n"
                 "  compare <before.json> <after.json> [--top N] [--json]\n"
                 "  wait <trace_path> --line N --se N --simd N --slot N --wave N\n"
                 "       [--cu N] [--format json|att] [--top N] [--context N] [--iterations A-B] [--json]\n"
                 "  wait-summary <trace_path> --line N [--se N] [--simd N] [--slot N] [--wave N]\n"
                 "       [--max-waves N] [--iterations A-B] [--format json|att] [--json]\n";
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
    rcv::ForceFormat force = rcv::ForceFormat::Auto;
    for (size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "-o" && i + 1 < args.size())
            out_path = args[++i];
        else if (args[i] == "--bins" && i + 1 < args.size())
            bins = integer("--bins", args[++i], 1, 4096);
        else if (args[i] == "--format" && i + 1 < args.size())
        {
            const std::string f = args[++i];
            if (f == "json")
                force = rcv::ForceFormat::JsonDir;
            else if (f == "att")
                force = rcv::ForceFormat::AttFiles;
            else
                return usage();
        }
        else
            return usage();
    }

    DataStore store;
    const auto load = rcv::loadTrace(trace_path, store, force);
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

    if (!HiddenLatencyAnalysis::analyze(store, true))
    {
        std::cerr << "error: hidden latency analysis failed\n";
        return 1;
    }

    const auto digest = rcv::buildDigest(store, trace_path, bins);

    try
    {
        writeAtomically(out_path, rcv::toJson(digest).dump());
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: cannot write " << out_path << ": " << e.what() << "\n";
        return 1;
    }

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

int cmdCompare(const std::vector<std::string>& args)
{
    if (args.size() < 2) return usage();
    int top = 20;
    bool as_json = false;
    for (size_t i = 2; i < args.size(); ++i)
    {
        if (args[i] == "--top" && i + 1 < args.size())
            top = integer("--top", args[++i], 1, rcv::MaxHotspotRows);
        else if (args[i] == "--json")
            as_json = true;
        else
            return usage();
    }
    rcv::Digest before, after;
    std::string error;
    if (!readDigest(args[0], before, error) || !readDigest(args[1], after, error)) throw std::runtime_error(error);
    auto result = rcv::compareDigests(before, after, top);
    result["before"]["digest_path"] = args[0];
    result["after"]["digest_path"] = args[1];
    std::cout << (as_json ? result.dump(2) + "\n" : rcv::renderCompare(result));
    return 0;
}

int cmdHotspot(const std::vector<std::string>& args)
{
    std::string digest_path = "digest.json";
    std::string by = "asm";
    std::string sort = "exposed";
    int top = 20;
    bool as_json = false;

    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-d" && i + 1 < args.size())
            digest_path = args[++i];
        else if (args[i] == "--by" && i + 1 < args.size())
            by = args[++i];
        else if (args[i] == "--sort" && i + 1 < args.size())
            sort = args[++i];
        else if (args[i] == "--top" && i + 1 < args.size())
            top = integer("--top", args[++i], 1, rcv::MaxHotspotRows);
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

    try
    {
        const auto rows = rcv::hotspot(digest, rcv::parseGroupBy(by), rcv::parseSortKey(sort), top);
        if (rows.empty() && by == "source")
            std::cerr << "note: no source-attributed lines (" << digest.meta.lines_with_source << "/"
                      << digest.meta.total_lines << "); the kernel was likely built without -g\n";
        if (as_json)
            std::cout << rcv::hotspotJson(rows).dump(2) << "\n";
        else
            std::cout << rcv::renderHotspot(digest, rows);
    }
    catch (const std::invalid_argument& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    return 0;
}

int cmdAsm(const std::vector<std::string>& args)
{
    std::string digest_path = "digest.json";
    int first = -1, last = -1, around = -1, context = 10;
    bool as_json = false;
    bool has_range = false, has_around = false, has_context = false;

    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-d" && i + 1 < args.size())
            digest_path = args[++i];
        else if (args[i] == "--range" && i + 1 < args.size())
        {
            has_range = true;
            const std::string spec = args[++i];
            const auto dash = spec.find('-', 1);
            if (dash == std::string::npos)
            {
                std::cerr << "error: --range expects A-B\n";
                return 2;
            }
            first = integer("--range start", spec.substr(0, dash), 0, std::numeric_limits<int>::max());
            last = integer("--range end", spec.substr(dash + 1), 0, std::numeric_limits<int>::max());
            if (last < first || int64_t(last) - first + 1 > 1000)
                throw UsageError("--range must be ascending and contain at most 1000 indices (inclusive)");
        }
        else if (args[i] == "--around" && i + 1 < args.size())
        {
            has_around = true;
            around = integer("--around", args[++i], 0, std::numeric_limits<int>::max());
        }
        else if (args[i] == "--context" && i + 1 < args.size())
        {
            has_context = true;
            context = integer("--context", args[++i], 0, 499);
        }
        else if (args[i] == "--json")
            as_json = true;
        else
            return usage();
    }

    if (has_range == has_around) throw UsageError("asm requires exactly one of --range A-B or --around N");
    if (has_context && !has_around) throw UsageError("--context requires --around and cannot be used with --range");

    rcv::Digest digest;
    std::string error;
    if (!readDigest(digest_path, digest, error))
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    const auto lines = around >= 0 ? rcv::asmAround(digest, around, context) : rcv::asmRange(digest, first, last);
    if (as_json)
        std::cout << rcv::asmJson(digest, lines).dump(2) << "\n";
    else
        std::cout << rcv::renderAsm(digest, lines);
    return 0;
}

// Keep even C-library decoder diagnostics off the JSON output channel.
class DiagnosticsToStderr
{
    int saved = -1;

public:
    DiagnosticsToStderr()
    {
        std::cout.flush();
        std::fflush(stdout);
        saved = dup(STDOUT_FILENO);
        if (saved < 0) throw std::runtime_error("cannot save stdout");
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
        {
            close(saved);
            throw std::runtime_error("cannot redirect loader diagnostics");
        }
    }
    ~DiagnosticsToStderr()
    {
        std::cout.flush();
        std::fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    DiagnosticsToStderr(const DiagnosticsToStderr&) = delete;
    DiagnosticsToStderr& operator=(const DiagnosticsToStderr&) = delete;
};

int cmdWait(const std::vector<std::string>& args, bool summary = false)
{
    if (args.empty()) return usage();
    rcv::WaveSelection selected;
    int line = -1, top = 3, context = 3;
    int max_waves = 16;
    rcv::IterationRange iterations;
    bool as_json = false;
    auto force = rcv::ForceFormat::Auto;
    for (size_t i = 1; i < args.size(); ++i)
    {
        const auto& option = args[i];
        if (option == "--json")
        {
            as_json = true;
            continue;
        }
        if (i + 1 >= args.size()) return usage();
        const auto& value = args[++i];
        if (option == "--format")
        {
            if (value == "json")
                force = rcv::ForceFormat::JsonDir;
            else if (value == "att")
                force = rcv::ForceFormat::AttFiles;
            else
                throw UsageError("--format expects json|att");
        }
        else if (option == "--iterations")
            iterations = iterationRange(value);
        else if (option == "--max-waves" && summary)
            max_waves = integer(option, value, 1, 128);
        else if (option == "--top" && !summary)
            top = integer(option, value, 1, 20);
        else if (option == "--context" && !summary)
            context = integer(option, value, 0, 10);
        else
        {
            int* target = nullptr;
            if (option == "--line")
                target = &line;
            else if (option == "--se")
                target = &selected.se;
            else if (option == "--simd")
                target = &selected.simd;
            else if (option == "--slot")
                target = &selected.slot;
            else if (option == "--wave")
                target = &selected.instance;
            else if (option == "--cu" && !summary)
                target = &selected.cu;
            else
                return usage();
            *target = integer(option, value, 0, std::numeric_limits<int>::max());
        }
    }
    if (summary && line < 0) throw UsageError("wait-summary requires --line");
    if (!summary && (line < 0 || selected.se < 0 || selected.simd < 0 || selected.slot < 0 || selected.instance < 0))
        throw UsageError("wait requires --line, --se, --simd, --slot and --wave (instance)");
    nlohmann::json result;
    {
        DiagnosticsToStderr redirect;
        DataStore store;
        const auto load = summary ? rcv::loadTrace(args[0], store, force, std::nullopt, rcv::WaveLoadMode::Deferred)
                                  : rcv::loadTrace(args[0], store, force, selected);
        if (!load.ok) throw std::runtime_error(load.error);
        for (const auto& warning : load.warnings) std::cerr << "warning: " << warning << "\n";
        result = summary ? rcv::queryWaitSummary(store, selected, line, max_waves, iterations)
                         : rcv::queryWait(store, selected, line, top, context, iterations);
    }
    std::cout << (as_json ? result.dump(2) + "\n" : summary ? rcv::renderWaitSummary(result) : rcv::renderWait(result));
    return 0;
}

int cmdCoverage(const std::vector<std::string>& args)
{
    std::string path = "digest.json";
    int top = 20;
    bool as_json = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-d" && i + 1 < args.size())
            path = args[++i];
        else if (args[i] == "--top" && i + 1 < args.size())
            top = integer("--top", args[++i], 1, 1000);
        else if (args[i] == "--json")
            as_json = true;
        else
            return usage();
    }
    rcv::Digest digest;
    std::string error;
    if (!readDigest(path, digest, error)) throw std::runtime_error(error);
    std::cout << (as_json ? rcv::coverageJson(digest, top).dump(2) + "\n" : rcv::renderCoverage(digest, top));
    return 0;
}

int cmdOccupancy(const std::vector<std::string>& args)
{
    std::string digest_path = "digest.json";
    std::string by;
    int se = -1, cu = -1, simd = -1, top = 20;
    bool as_json = false, has_top = false;

    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-d" && i + 1 < args.size())
            digest_path = args[++i];
        else if (args[i] == "--se" && i + 1 < args.size())
            se = integer("--se", args[++i], 0, std::numeric_limits<int>::max());
        else if (args[i] == "--cu" && i + 1 < args.size())
            cu = integer("--cu", args[++i], 0, std::numeric_limits<int>::max());
        else if (args[i] == "--simd" && i + 1 < args.size())
            simd = integer("--simd", args[++i], 0, std::numeric_limits<int>::max());
        else if (args[i] == "--by" && i + 1 < args.size())
            by = args[++i];
        else if (args[i] == "--top" && i + 1 < args.size())
        {
            has_top = true;
            top = integer("--top", args[++i], 1, 1000);
        }
        else if (args[i] == "--json")
            as_json = true;
        else
            return usage();
    }

    if (!by.empty() && by != "cu" && by != "simd") throw UsageError("--by expects cu|simd");
    if (by.empty() && (cu >= 0 || simd >= 0 || has_top)) throw UsageError("--cu/--simd/--top require --by cu|simd");
    if (simd >= 0 && by != "simd") throw UsageError("--simd requires --by simd");

    rcv::Digest digest;
    std::string error;
    if (!readDigest(digest_path, digest, error))
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (!by.empty())
    {
        const auto j = rcv::groupedOccupancyJson(digest, by == "simd", se, cu, simd, top);
        std::cout << (as_json ? j.dump(2) + "\n" : rcv::renderGroupedOccupancy(j));
    }
    else if (as_json)
        std::cout << rcv::occupancyJson(digest, se).dump(2) << "\n";
    else
        std::cout << rcv::renderOccupancy(digest, se);
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    try
    {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.empty()) return usage();

        const std::string command = args[0];
        const std::vector<std::string> rest(args.begin() + 1, args.end());

        if (command == "analyze") return cmdAnalyze(rest);
        if (command == "summary") return cmdSummary(rest);
        if (command == "compare") return cmdCompare(rest);
        if (command == "hotspot") return cmdHotspot(rest);
        if (command == "asm") return cmdAsm(rest);
        if (command == "occupancy") return cmdOccupancy(rest);
        if (command == "coverage") return cmdCoverage(rest);
        if (command == "wait") return cmdWait(rest);
        if (command == "wait-summary") return cmdWait(rest, true);

        return usage();
    }
    catch (const UsageError& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
