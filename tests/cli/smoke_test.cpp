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

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "cli/digest.h"
#include "json/include/nlohmann/json.hpp"

namespace
{
struct RunResult
{
    int status = -1;
    std::string output;
};

RunResult run(const std::string& command)
{
    RunResult r;
    std::array<char, 4096> buf{};
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) return r;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) r.output += buf.data();
    r.status = pclose(pipe);
    return r;
}

std::string binary()
{
    const char* env = std::getenv("RCV_CLI_BINARY");
    return env ? env : "";
}

std::string trace()
{
    const char* env = std::getenv("RCV_CLI_TEST_TRACE");
    return env ? env : "";
}

std::string digestPath() { return "/tmp/rcv-smoke-digest.json"; }
} // namespace

TEST(Smoke, AnalyzeThenQueryChain)
{
    if (binary().empty() || trace().empty()) GTEST_SKIP() << "RCV_CLI_BINARY / RCV_CLI_TEST_TRACE not set";

    const auto start = std::chrono::steady_clock::now();
    const auto analyze = run(binary() + " analyze " + trace() + " -o " + digestPath());
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);

    ASSERT_EQ(analyze.status, 0) << analyze.output;
    std::cerr << "[ TIMING   ] analyze took " << elapsed.count() << "s\n";

    for (const std::string& cmd :
         {std::string("summary"), std::string("hotspot --top 5"), std::string("occupancy --se 0")})
    {
        const auto r = run(binary() + " " + cmd + " -d " + digestPath());
        EXPECT_EQ(r.status, 0) << cmd << ": " << r.output;
        EXPECT_FALSE(r.output.empty()) << cmd;
    }

    const auto around = run(binary() + " asm -d " + digestPath() + " --around 100 --context 2");
    EXPECT_EQ(around.status, 0) << around.output;
    EXPECT_NE(around.output.find("instruction"), std::string::npos) << around.output;
}

// Exit code 0 with an all-zero hidden column is exactly what inheriting
// MainWindow's 200 MB wave-load gate looks like. Every other check in this
// suite passes in that state, so assert on the data itself.
TEST(Smoke, HiddenLatencyIsPresentInTheProducedDigest)
{
    if (binary().empty() || trace().empty()) GTEST_SKIP() << "RCV_CLI_BINARY / RCV_CLI_TEST_TRACE not set";

    ASSERT_EQ(run(binary() + " analyze " + trace() + " -o " + digestPath()).status, 0);

    std::ifstream in(digestPath());
    ASSERT_TRUE(in.is_open());
    const auto digest = rcv::fromJson(nlohmann::json::parse(in));

    EXPECT_TRUE(digest.meta.hidden_latency_available);
    int64_t hidden = 0;
    for (const auto& l : digest.lines) hidden += l.hidden();
    EXPECT_GT(hidden, 0) << "hidden latency is entirely zero";
}

TEST(Smoke, RejectsBadArgumentsWithNonZeroExit)
{
    if (binary().empty()) GTEST_SKIP() << "RCV_CLI_BINARY not set";

    EXPECT_NE(run(binary() + " hotspot -d " + digestPath() + " --sort bogus").status, 0);
    EXPECT_NE(run(binary() + " asm -d " + digestPath()).status, 0);
    EXPECT_NE(run(binary() + " summary -d /nonexistent/digest.json").status, 0);
}
