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
#include "test_support.h"

using cli_test::binary;
using cli_test::run;

namespace
{
std::string trace()
{
    const char* env = std::getenv("RCV_CLI_TEST_TRACE");
    return env ? env : "";
}

} // namespace

TEST(Smoke, AnalyzeThenQueryChain)
{
    if (binary().empty() || trace().empty()) GTEST_SKIP() << "RCV_CLI_BINARY / RCV_CLI_TEST_TRACE not set";

    cli_test::TempDir tmp;
    const auto digest_path = (tmp.path / "digest.json").string();

    const auto start = std::chrono::steady_clock::now();
    const auto analyze = run({binary(), "analyze", trace(), "-o", digest_path});
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);

    ASSERT_EQ(analyze.signal, 0) << analyze.error;
    ASSERT_EQ(analyze.status, 0) << analyze.error;
    std::cerr << "[ TIMING   ] analyze took " << elapsed.count() << "s\n";

    for (auto args : std::vector<std::vector<std::string>>{
             {"summary"}, {"hotspot", "--top", "5"}, {"occupancy", "--se", "0"}})
    {
        args.insert(args.begin(), binary());
        args.insert(args.end(), {"-d", digest_path});
        const auto r = run(args);
        EXPECT_EQ(r.signal, 0) << r.error;
        EXPECT_EQ(r.status, 0) << r.error;
        EXPECT_FALSE(r.output.empty());
    }

    const auto around = run({binary(), "asm", "-d", digest_path, "--around", "100", "--context", "2"});
    EXPECT_EQ(around.status, 0) << around.output;
    EXPECT_NE(around.output.find("instruction"), std::string::npos) << around.output;
}

// Exit code 0 with an all-zero hidden column is exactly what inheriting
// MainWindow's 200 MB wave-load gate looks like. Every other check in this
// suite passes in that state, so assert on the data itself.
TEST(Smoke, HiddenLatencyIsPresentInTheProducedDigest)
{
    if (binary().empty() || trace().empty()) GTEST_SKIP() << "RCV_CLI_BINARY / RCV_CLI_TEST_TRACE not set";

    cli_test::TempDir tmp;
    const auto digest_path = (tmp.path / "digest.json").string();
    const auto result = run({binary(), "analyze", trace(), "-o", digest_path});
    ASSERT_EQ(result.signal, 0) << result.error;
    ASSERT_EQ(result.status, 0) << result.error;

    std::ifstream in(digest_path);
    ASSERT_TRUE(in.is_open());
    const auto digest = rcv::fromJson(nlohmann::json::parse(in));

    EXPECT_TRUE(digest.meta.hidden_latency_available);
    int64_t hidden = 0;
    for (const auto& l : digest.lines) hidden += l.hidden();
    EXPECT_GT(hidden, 0) << "hidden latency is entirely zero";
}

TEST(Smoke, DistinguishesUsageAndRuntimeExitCodes)
{
    if (binary().empty()) GTEST_SKIP() << "RCV_CLI_BINARY not set";

    cli_test::TempDir tmp;
    const auto digest_path = (tmp.path / "digest.json").string();
    cli_test::write(digest_path, rcv::toJson(rcv::Digest{}).dump());
    EXPECT_EQ(run({binary(), "hotspot", "-d", digest_path, "--sort", "bogus"}).status, 2);
    EXPECT_EQ(run({binary(), "asm", "-d", digest_path}).status, 2);
    EXPECT_EQ(run({binary(), "summary", "-d", (tmp.path / "missing.json").string()}).status, 1);
}
