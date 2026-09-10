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
#include <cstdlib>
#include <iostream>
#include "analysis/hidden_latency.h"
#include "cli/digest_builder.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"
#include "test_support.h"

namespace
{
rcv::Digest digestOf(const std::string& path, rcv::ForceFormat force = rcv::ForceFormat::Auto)
{
    DataStore store;
    const auto load = rcv::loadTrace(path, store, force);
    EXPECT_TRUE(load.ok) << path << ": " << load.error;
    return rcv::buildDigest(store, path, 200);
}
} // namespace

// The .att directory and the ui_output directory describe the same dispatch,
// so their per-line hotspot aggregates must agree.
TEST(AttPath, ProducesSameHotspotAggregatesAsJsonPath)
{
    const char* json_dir = std::getenv("RCV_CLI_TEST_TRACE");
    const char* att_dir = std::getenv("RCV_CLI_TEST_ATT_DIR");
    if (!json_dir || !att_dir) GTEST_SKIP() << "RCV_CLI_TEST_TRACE / RCV_CLI_TEST_ATT_DIR not set";

    const auto from_json = digestOf(json_dir);
    const auto from_att = digestOf(att_dir, rcv::ForceFormat::AttFiles);

    ASSERT_EQ(from_att.lines.size(), from_json.lines.size());
    for (size_t i = 0; i < from_json.lines.size(); ++i)
    {
        const auto& a = from_json.lines[i];
        const auto& b = from_att.lines[i];
        EXPECT_EQ(b.addr, a.addr) << "line " << i;
        EXPECT_EQ(b.inst, a.inst) << "line " << i;
        EXPECT_EQ(b.hitcount, a.hitcount) << "line " << i;
        EXPECT_EQ(b.latency, a.latency) << "line " << i;
        EXPECT_EQ(b.stall, a.stall) << "line " << i;
        // NOT idle - see IdleDivergesOnlyWithinDecoderVersionSkew below.
    }
}

// idle is the one field the two paths do not agree on exactly, and the reason is
// external: the ui_output directory was produced at capture time by the decoder
// bundled with ROCm (0.2.0), while the .att path is decoded by the rebuilt
// 0.2.2 that has the comgr disassembly backend. idle is derived from inter-token
// gap attribution, which is exactly the kind of thing that shifts between
// decoder revisions; addr, inst, hitcount, latency and stall are direct
// per-instruction counts and match exactly on all 2521 lines.
//
// Measured on the reference capture: 50 of 2521 lines differ, aggregate
// |delta| 2,849,611 against a total of 47,228,622 cycles, i.e. 6.03%.
//
// The bounds below are deliberately a few times looser than that measurement so
// ordinary version drift does not fail the suite, but tight enough that a real
// regression in idle attribution still does.
TEST(AttPath, IdleDivergesOnlyWithinDecoderVersionSkew)
{
    const char* json_dir = std::getenv("RCV_CLI_TEST_TRACE");
    const char* att_dir = std::getenv("RCV_CLI_TEST_ATT_DIR");
    if (!json_dir || !att_dir) GTEST_SKIP() << "RCV_CLI_TEST_TRACE / RCV_CLI_TEST_ATT_DIR not set";

    const auto from_json = digestOf(json_dir);
    const auto from_att = digestOf(att_dir, rcv::ForceFormat::AttFiles);
    ASSERT_EQ(from_att.lines.size(), from_json.lines.size());

    int differing = 0;
    int64_t delta = 0;
    int64_t total = 0;
    int first_diff = -1;
    for (size_t i = 0; i < from_json.lines.size(); ++i)
    {
        const int64_t ja = from_json.lines[i].idle;
        const int64_t at = from_att.lines[i].idle;
        total += ja;
        if (ja != at)
        {
            if (first_diff < 0) first_diff = static_cast<int>(i);
            ++differing;
            delta += std::llabs(at - ja);
        }
    }

    const double line_share = 100.0 * differing / static_cast<double>(from_json.lines.size());
    const double cycle_share = total ? 100.0 * static_cast<double>(delta) / static_cast<double>(total) : 0.0;
    std::cerr << "[ INFO     ] idle: " << differing << " lines differ (" << line_share << "%), aggregate delta "
              << delta << " of " << total << " (" << cycle_share << "%), first at line " << first_diff << "\n";

    EXPECT_LT(line_share, 10.0) << "far more lines disagree on idle than decoder skew explains";
    EXPECT_LT(cycle_share, 20.0) << "idle attribution has diverged well beyond decoder skew";
}

TEST(AttPath, LoadsWavesAndOccupancy)
{
    const char* att_dir = std::getenv("RCV_CLI_TEST_ATT_DIR");
    if (!att_dir) GTEST_SKIP() << "RCV_CLI_TEST_ATT_DIR not set";

    const auto d = digestOf(att_dir, rcv::ForceFormat::AttFiles);
    EXPECT_GT(d.meta.wave_count, 0);
    EXPECT_GT(d.meta.se_count, 0);
    // se0_sm3_sl0_wv0.json records cu=1 for this exact reference wave.
    bool found = false;
    for (const auto& wave : d.waves)
        if (wave.se == 0 && wave.simd == 3 && wave.slot == 0)
        {
            EXPECT_EQ(wave.cu, 1);
            found = true;
        }
    EXPECT_TRUE(found);
}

TEST(AttPath, RejectsPartialDecode)
{
    const char* att_dir = std::getenv("RCV_CLI_TEST_ATT_DIR");
    if (!att_dir || !*att_dir) GTEST_SKIP() << "RCV_CLI_TEST_ATT_DIR not set";
    cli_test::TempDir tmp;
    for (const auto& entry : std::filesystem::directory_iterator(att_dir))
        std::filesystem::create_symlink(entry.path(), tmp.path / entry.path().filename());
    cli_test::write(tmp.path / "999_29410_shader_engine_99_19.att", "broken ATT input");
    ASSERT_FALSE(cli_test::binary().empty());
    const auto cli = cli_test::run({cli_test::binary(), "analyze", tmp.path.string(), "--format", "att",
                                    "-o", (tmp.path / "digest.json").string()});
    EXPECT_EQ(cli.status, 1) << cli.error;
    EXPECT_EQ(cli.signal, 0);
    EXPECT_FALSE(std::filesystem::exists(tmp.path / "digest.json"));
    DataStore store;
    const auto load = rcv::loadTrace(tmp.path.string(), store, rcv::ForceFormat::AttFiles);
    EXPECT_FALSE(load.ok);
    EXPECT_NE(load.error.find("99"), std::string::npos) << load.error;
}

TEST(AttPath, ForcedAttDiscoversInputsInMixedFormatDirectory)
{
    const char* att_dir = std::getenv("RCV_CLI_TEST_ATT_DIR");
    if (!att_dir || !*att_dir) GTEST_SKIP() << "RCV_CLI_TEST_ATT_DIR not set";
    cli_test::TempDir tmp;
    std::filesystem::copy(std::filesystem::path(RCV_CLI_FIXTURE_DIR) / "solo", tmp.path,
                          std::filesystem::copy_options::recursive);
    for (const auto& entry : std::filesystem::directory_iterator(att_dir))
    {
        auto ext = entry.path().extension();
        if (ext == ".att" || ext == ".out" || ext == ".hsaco")
            std::filesystem::create_symlink(entry.path(), tmp.path / entry.path().filename());
    }
    const auto from_json = digestOf(tmp.path.string());
    ASSERT_EQ(from_json.meta.wave_count, 1);
    const auto from_att = digestOf(tmp.path.string(), rcv::ForceFormat::AttFiles);
    EXPECT_EQ(from_att.meta.wave_count, 128);
    EXPECT_GT(from_att.lines.size(), from_json.lines.size());
    int64_t latency = 0;
    for (const auto& line : from_att.lines) latency += line.latency;
    EXPECT_GT(latency, 204) << "must decode instructions, not just preseed sidecar code";
}
