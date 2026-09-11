#include <gtest/gtest.h>
#include "analysis/hidden_latency.h"
#include "cli/digest.h"
#include "cli/wait_query.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"
#include "data/wavemanager.h"
#include "test_support.h"

using namespace cli_test;
using nlohmann::json;

class CliRegression : public ::testing::Test
{
protected:
    TempDir tmp;
    fs::path trace = tmp.path / "trace with ' quotes";
    fs::path output = tmp.path / "digest.json";
    void SetUp() override
    {
        ASSERT_FALSE(binary().empty()) << "RCV_CLI_BINARY is required";
        fs::copy(fs::path(RCV_CLI_FIXTURE_DIR) / "solo", trace, fs::copy_options::recursive);
    }
    RunResult analyze() { return run({binary(), "analyze", trace.string(), "-o", output.string()}); }
    void makeWaitTrace()
    {
        auto code = json::parse(read(trace / "code.json"));
        code["code"][1][0] = "s_wait_dscnt 0";
        write(trace / "code.json", code.dump());
        auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
        wave["wave"]["instructions"] = {
            {100, 3, 0, 1, 1},
            {110, 6, 0, 1, 2},
            {120, 3, 4, 5, 1},
            {140, 3, 8, 9, 1}
        };
        wave["wave"]["waitcnt"] = {
            {1, {{2, 0}}}
        };
        write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    }
    RunResult waitQuery(std::vector<std::string> extra = {})
    {
        std::vector<std::string> args{
            binary(),
            "wait",
            trace.string(),
            "--line",
            "1",
            "--se",
            "0",
            "--simd",
            "0",
            "--slot",
            "0",
            "--wave",
            "0",
            "--json"
        };
        args.insert(args.end(), extra.begin(), extra.end());
        return run(args);
    }
    json summaryJson()
    {
        auto result = run({binary(), "summary", "-d", output.string(), "--json"});
        EXPECT_EQ(result.signal, 0) << result.error;
        EXPECT_EQ(result.status, 0) << result.error;
        return json::parse(result.output);
    }
    void expectFailure()
    {
        write(output, "previous digest");
        auto result = analyze();
        EXPECT_EQ(result.signal, 0) << result.error;
        EXPECT_EQ(result.status, 1) << result.error;
        EXPECT_NE(result.error.find("error:"), std::string::npos);
        EXPECT_EQ(result.error.find("wrote "), std::string::npos);
        EXPECT_EQ(read(output), "previous digest");
    }
};

TEST_F(CliRegression, RejectsMissingListedWave)
{
    fs::remove(trace / "se0_sm0_sl0_wv0.json");
    expectFailure();
}

TEST_F(CliRegression, CompareMatchesOpcodesAcrossMovedLinesAndReportsSignedChanges)
{
    const auto after = tmp.path / "after.json";
    write(output, R"({"version":2,"meta":{"hidden_latency_available":true},"waves":[{}],"lines":[
      {"i":1,"inst":" s_wait_dscnt 0","hit":2,"lat":12,"stall":10,"idle":3,"hid":[9,4,0]},
      {"i":2,"inst":"s_wait_dscnt 1","hit":3,"lat":8,"stall":6,"hid":[0,1,0]},
      {"i":3,"inst":"removed_op","hit":1,"lat":2}
    ]})");
    write(after, R"({"version":2,"meta":{"hidden_latency_available":true},"waves":[{},{}],"lines":[
      {"i":99,"inst":"s_wait_dscnt 0","hit":10,"lat":16,"stall":12,"hid":[0,6,0]},
      {"i":1,"inst":"added_op","hit":2,"lat":4}
    ],"occupancy":{"bins":2,"total":[0,2]}})");
    const auto result = run({binary(), "compare", output.string(), after.string(), "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["total_opcodes"], 3);
    EXPECT_EQ(j["opcodes"][0]["opcode"], "s_wait_dscnt");
    const auto& row = j["opcodes"][0];
    EXPECT_EQ(row["metrics"]["exposed"]["before"], 15);
    EXPECT_EQ(row["metrics"]["exposed"]["after"], 10);
    EXPECT_EQ(row["metrics"]["exposed"]["delta"], -5);
    EXPECT_EQ(row["metrics"]["hitcount"]["delta"], 5);
    EXPECT_EQ(row["per_traced_wave"]["exposed"]["after"], 5);
    EXPECT_EQ(j["coverage"]["instruction_traced_waves"]["delta"], 1);
    EXPECT_TRUE(j["occupancy"]["mean_waves"]["delta"].is_null());
    EXPECT_EQ(j["comparability"]["status"], "unverified");
    EXPECT_TRUE(j["before"]["workload"].is_null());
    EXPECT_TRUE(j["after"]["decoder"].is_null());
    EXPECT_FALSE(j["comparability"]["warnings"].empty());
    const auto& added = j["opcodes"][1];
    EXPECT_EQ(added["presence"], "added");
    EXPECT_TRUE(added["metrics"]["exposed"]["percent"].is_null());
    EXPECT_EQ(j["opcodes"][2]["presence"], "removed");
}

TEST_F(CliRegression, CompareAggregatesBeforeTruncationAndPreservesUnavailable)
{
    json d = {
        {"version", 1             },
        {"meta",    json::object()},
        {"lines",   json::array() }
    };
    for (int i = 0; i < 1005; ++i)
        d["lines"].push_back({
            {"i",     i                       },
            {"inst",  "op" + std::to_string(i)},
            {"lat",   10                      },
            {"stall", 10                      }
        });
    write(output, d.dump());
    d["lines"][1004]["stall"] = 0;
    const auto after = tmp.path / "after.json";
    write(after, d.dump());
    const auto result = run({binary(), "compare", output.string(), after.string(), "--top", "1", "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["total_opcodes"], 1005);
    EXPECT_EQ(j["truncated"], true);
    ASSERT_EQ(j["opcodes"].size(), 1);
    EXPECT_EQ(j["opcodes"][0]["opcode"], "op1004");
    EXPECT_EQ(j["sort_metric"], "stall");
    EXPECT_TRUE(j["opcodes"][0]["metrics"]["exposed"]["delta"].is_null());
    EXPECT_TRUE(j["opcodes"][0]["per_traced_wave"]["stall"]["after"].is_null());
    EXPECT_EQ(run({binary(), "compare", "missing", "missing", "--top", "0"}).status, 2);
}

TEST_F(CliRegression, WaitSummaryFiltersIterationsAndBoundsLoadedWaves)
{
    makeWaitTrace();
    auto manifest = json::parse(read(trace / "filenames.json"));
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["instructions"] = {
        {100, 3, 0,     1,     1},
        {120, 3, 70000, 70001, 1}
    };
    write(trace / "se0_sm0_sl0_wv1.json", wave.dump());
    manifest["wave_filenames"]["0"]["0"]["0"]["1"] = {"se0_sm0_sl0_wv1.json", 0, 1000};
    // Third wave is deliberately missing: the bound must apply before loading it.
    manifest["wave_filenames"]["0"]["0"]["0"]["2"] = {"se0_sm0_sl0_wv2.json", 0, 1000};
    write(trace / "filenames.json", manifest.dump());
    const auto result = run(
        {binary(), "wait-summary", trace.string(), "--line", "1", "--max-waves", "2", "--iterations", "1-2", "--json"}
    );
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["matching_waves"], 3);
    EXPECT_EQ(j["selected_waves"], 2);
    EXPECT_EQ(j["truncated"], true);
    ASSERT_EQ(j["waves"].size(), 2);
    EXPECT_EQ(j["waves"][0]["statistics"]["count"], 2);
    EXPECT_EQ(j["waves"][0]["statistics"]["mean"], 6);
    EXPECT_EQ(j["waves"][0]["statistics"]["stddev"], 2);
    EXPECT_EQ(j["waves"][0]["statistics"]["p95"], 8);
    EXPECT_EQ(j["waves"][1]["statistics"]["count"], 1);
    EXPECT_EQ(j["waves"][1]["statistics"]["max"], 70000);
    EXPECT_EQ(j["waves"][1]["wave"]["instance"], 1);
    EXPECT_EQ(run({binary(), "wait-summary", trace.string(), "--line", "1", "--max-waves", "3"}).status, 1);
    expectFailure();
}

TEST_F(CliRegression, WaitSummaryDistinguishesNoExecutionsFromMeasuredZero)
{
    makeWaitTrace();
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["instructions"] = {
        {100, 3, 0, 1, 1}
    };
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    for (const auto& range : {"0-0", "1-2"})
    {
        const auto result =
            run({binary(), "wait-summary", trace.string(), "--line", "1", "--iterations", range, "--json"});
        ASSERT_EQ(result.status, 0) << result.error;
        const auto row = json::parse(result.output)["waves"][0];
        if (std::string(range) == "0-0")
        {
            EXPECT_EQ(row["available"], true);
            EXPECT_EQ(row["statistics"]["mean"], 0);
        }
        else
        {
            EXPECT_EQ(row["available"], false);
            EXPECT_EQ(row["statistics"]["count"], 0);
            EXPECT_TRUE(row["statistics"]["mean"].is_null());
        }
    }
    EXPECT_EQ(run({binary(), "wait-summary", trace.string(), "--line", "2"}).status, 1);
    EXPECT_EQ(run({binary(), "wait-summary", trace.string(), "--line", "1", "--se", "99"}).status, 1);
    for (const auto& range : {"2-1", "-1-0", "0-", "0-2x"})
        EXPECT_EQ(run({binary(), "wait-summary", "missing", "--line", "1", "--iterations", range}).status, 2);
    EXPECT_EQ(run({binary(), "wait-summary", "missing", "--line", "1", "--max-waves", "129"}).status, 2);
    EXPECT_EQ(run({binary(), "wait-summary", "missing"}).status, 2);
}

TEST_F(CliRegression, WaitIterationRangeRetainsOriginalOccurrenceNumbers)
{
    makeWaitTrace();
    const auto result = waitQuery({"--iterations", "1-1", "--context", "0"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["statistics"]["count"], 1);
    EXPECT_EQ(j["statistics"]["mean"], 4);
    EXPECT_EQ(j["occurrences"][0]["iteration"], 1);
    EXPECT_EQ(j["occurrences"][0]["clock"], 120);
    EXPECT_EQ(waitQuery({"--iterations", "3-4"}).status, 1);
}

TEST_F(CliRegression, WaitSummaryFiltersCoordinatesBeforeLoadingAndReportsUnexecuted)
{
    makeWaitTrace();
    auto manifest = json::parse(read(trace / "filenames.json"));
    // The original, now missing wave must be excluded before loading.
    fs::remove(trace / "se0_sm0_sl0_wv0.json");
    auto wave = json::parse(read(fs::path(RCV_CLI_FIXTURE_DIR) / "solo/se0_sm0_sl0_wv0.json"));
    wave["wave"]["instructions"] = {
        {100, 6, 0, 1, 2}
    };
    write(trace / "se2_sm3_sl4_wv5.json", wave.dump());
    manifest["wave_filenames"]["2"]["3"]["4"]["5"] = {"se2_sm3_sl4_wv5.json", 0, 1000};
    write(trace / "filenames.json", manifest.dump());
    const auto result = run(
        {binary(),
         "wait-summary",
         trace.string(),
         "--line",
         "1",
         "--se",
         "2",
         "--simd",
         "3",
         "--slot",
         "4",
         "--wave",
         "5",
         "--json"}
    );
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["matching_waves"], 1);
    EXPECT_EQ(j["waves"][0]["wave"]["instance"], 5);
    EXPECT_EQ(j["waves"][0]["status"], "not_executed");
    EXPECT_TRUE(j["waves"][0]["statistics"]["p95"].is_null());
}

TEST_F(CliRegression, CompareIdenticalDigestsAndAvailableOccupancy)
{
    ASSERT_EQ(analyze().status, 0);
    auto d = json::parse(read(output));
    d["occupancy"]["bins"] = 2;
    d["occupancy"]["total"] = {0, 2};
    write(output, d.dump());
    auto result = run({binary(), "compare", output.string(), output.string(), "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    auto j = json::parse(result.output);
    for (const auto& value : j["cycles"]) EXPECT_EQ(value["delta"], 0);
    EXPECT_EQ(j["occupancy"]["mean_waves"]["before"], 1);
    EXPECT_EQ(j["occupancy"]["peak_waves"]["before"], 2);
    EXPECT_EQ(j["occupancy"]["mean_waves"]["delta"], 0);
    d["occupancy"]["total"] = {0, 0};
    const auto after = tmp.path / "after.json";
    write(after, d.dump());
    result = run({binary(), "compare", output.string(), after.string(), "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    j = json::parse(result.output);
    EXPECT_EQ(j["occupancy"]["mean_waves"]["delta"], -1);
    EXPECT_EQ(j["occupancy"]["mean_waves"]["percent"], -100);
}

TEST_F(CliRegression, OpcodeGroupingSumsClampedLineCostsAndSkipsComments)
{
    write(output, R"({"version":1,"meta":{},"lines":[
      {"i":1,"addr":100,"inst":"  s_wait_dscnt 0","hit":2,"lat":12,"stall":10,"idle":3,"hid":[9,4,0]},
      {"i":2,"addr":200,"inst":"\ts_wait_dscnt 1","hit":3,"lat":8,"stall":6,"hid":[0,1,0]},
      {"i":3,"inst":" ; comment","lat":999},{"i":4,"inst":"  ","lat":999}
    ]})");
    auto result = run({binary(), "hotspot", "-d", output.string(), "--by", "opcode", "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto rows = json::parse(result.output);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0]["key"], "s_wait_dscnt");
    EXPECT_EQ(rows[0]["hitcount"], 5);
    EXPECT_EQ(rows[0]["stall"], 16);
    EXPECT_EQ(rows[0]["total"], 23);
    EXPECT_EQ(rows[0]["hidden"], 8);
    EXPECT_EQ(rows[0]["exposed"], 15);
}

TEST_F(CliRegression, CoverageBoundsRowsAndDoesNotInventOldInstanceOrCu)
{
    write(output, R"({"version":1,"meta":{},"lines":[],"waves":[
      {"se":0,"cu":1,"simd":3,"slot":0},
      {"se":0,"cu":1,"simd":3,"slot":1},
      {"se":1,"simd":3,"slot":0}
    ]})");
    auto result = run({binary(), "coverage", "-d", output.string(), "--top", "1", "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["instruction_traced_waves"], 3);
    EXPECT_EQ(j["known_cu_waves"], 2);
    EXPECT_EQ(j["unknown_cu_waves"], 1);
    EXPECT_EQ(j["total_groups"], 2);
    EXPECT_EQ(j["truncated"], true);
    ASSERT_EQ(j["groups"].size(), 1);
    EXPECT_EQ(j["groups"][0]["wave_count"], 2);
    EXPECT_TRUE(j["groups"][0]["instance_min"].is_null());
    EXPECT_TRUE(j["occupancy_wave_starts"].is_null());
    EXPECT_EQ(summaryJson()["coverage"]["instruction_traced_waves"], 3);
}

TEST_F(CliRegression, NewDigestPreservesWaveInstanceForCoverage)
{
    ASSERT_EQ(analyze().status, 0);
    const auto d = json::parse(read(output));
    EXPECT_EQ(d["version"], 2);
    ASSERT_EQ(d["waves"].size(), 1);
    EXPECT_EQ(d["waves"][0]["instance"], 0);
    auto result = run({binary(), "coverage", "-d", output.string(), "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["groups"][0]["instance_min"], 0);
    EXPECT_EQ(j["groups"][0]["instance_max"], 0);
}

TEST_F(CliRegression, CoverageDisclosesSlotLocalInstanceRanges)
{
    write(output, R"({"version":2,"meta":{},"lines":[],"waves":[
      {"se":0,"cu":1,"simd":3,"slot":0,"instance":0},
      {"se":0,"cu":1,"simd":3,"slot":0,"instance":1},
      {"se":0,"cu":1,"simd":3,"slot":1,"instance":0},
      {"se":0,"cu":1,"simd":3,"slot":1,"instance":1}
    ]})");
    auto result = run({binary(), "coverage", "-d", output.string(), "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    ASSERT_EQ(j["groups"].size(), 1);
    EXPECT_EQ(j["groups"][0]["wave_count"], 4);
    EXPECT_EQ(j["groups"][0]["slot_count"], 2);
    EXPECT_EQ(j["groups"][0]["instance_min"], 0);
    EXPECT_EQ(j["groups"][0]["instance_max"], 1);
    EXPECT_EQ(j["instance_range_scope"], "slot_local_values_aggregated");
    EXPECT_NE(j["instance_range_note"].get<std::string>().find("wave_count"), std::string::npos);
}

TEST_F(CliRegression, GroupedOccupancyExactStatsAndEncodedIdentity)
{
    write(trace / "occupancy.json", R"({"0":[[0,129,3,0,1,0],[250,129,3,1,1,0],
        [750,129,3,0,0,0],[1000,129,3,1,0,0]]})");
    ASSERT_EQ(analyze().status, 0);
    for (const auto& by : {"cu", "simd"})
    {
        auto result =
            run({binary(), "occupancy", "-d", output.string(), "--by", by, "--se", "0", "--cu", "129", "--json"});
        ASSERT_EQ(result.status, 0) << result.error;
        const auto j = json::parse(result.output);
        EXPECT_EQ(j["available"], true);
        ASSERT_EQ(j["groups"].size(), 1);
        EXPECT_EQ(j["groups"][0]["cu"], 129);
        EXPECT_EQ(j["groups"][0]["available"], true);
        EXPECT_EQ(j["groups"][0]["peak_waves"], 2);
        EXPECT_EQ(j["groups"][0]["mean_waves"], 1.5);
        EXPECT_EQ(j["groups"][0]["wave_starts"], 2);
    }
    EXPECT_EQ(summaryJson()["coverage"]["occupancy_wave_starts"], 2);
}

TEST_F(CliRegression, GroupedOccupancyTiesPreWindowZeroAndInvalidStreams)
{
    // Analyze window [100,1000). Activity before it sets initial concurrency.
    auto manifest = json::parse(read(trace / "filenames.json"));
    manifest["wave_filenames"]["0"]["0"]["0"]["0"][1] = 100;
    write(trace / "filenames.json", manifest.dump());
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["begin"] = 100;
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    write(trace / "occupancy.json", R"({"0":[
        [0,1,0,0,1,0],[500,1,0,1,1,0],[500,1,0,0,0,0],[1000,1,0,1,0,0],
        [1000,1,0,2,1,0],[1100,1,0,2,0,0],
        [200,2,0,0,1,0],[200,2,0,0,0,0],
        [200,3,0,0,0,0],[300,3,0,0,1,0],
        [200,4,0,0,1,0]]})");
    ASSERT_EQ(analyze().status, 0);
    auto result = run({binary(), "occupancy", "-d", output.string(), "--by", "simd", "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto rows = json::parse(result.output)["groups"];
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[0]["peak_waves"], 1);
    EXPECT_EQ(rows[0]["mean_waves"], 1.0);
    EXPECT_EQ(rows[0]["wave_starts"], 1);
    EXPECT_EQ(rows[1]["available"], true);
    EXPECT_EQ(rows[1]["peak_waves"], 0);
    EXPECT_EQ(rows[1]["mean_waves"], 0.0);
    EXPECT_EQ(rows[2]["available"], false);
    EXPECT_TRUE(rows[2]["mean_waves"].is_null());
    EXPECT_EQ(rows[3]["available"], false);
    EXPECT_TRUE(rows[3]["peak_waves"].is_null());
    auto bounded = run({binary(), "occupancy", "-d", output.string(), "--by", "cu", "--top", "1", "--json"});
    ASSERT_EQ(bounded.status, 0) << bounded.error;
    const auto j = json::parse(bounded.output);
    EXPECT_EQ(j["total_groups"], 4);
    EXPECT_EQ(j["truncated"], true);
    EXPECT_EQ(j["groups"].size(), 1);
    EXPECT_EQ(summaryJson()["coverage"]["occupancy_wave_starts"], 6);
    EXPECT_EQ(summaryJson()["coverage"]["occupancy_wave_starts_in_window"], 4);
}

TEST_F(CliRegression, CuOccupancyCombinesSimdsAndSimdFilterStaysLocal)
{
    write(trace / "occupancy.json", R"({"0":[[0,1,0,0,1,0],[0,1,1,0,1,0],
        [500,1,0,0,0,0],[1000,1,1,0,0,0]]})");
    ASSERT_EQ(analyze().status, 0);
    auto cu = run({binary(), "occupancy", "-d", output.string(), "--by", "cu", "--json"});
    ASSERT_EQ(cu.status, 0) << cu.error;
    const auto cu_rows = json::parse(cu.output)["groups"];
    ASSERT_EQ(cu_rows.size(), 1);
    EXPECT_EQ(cu_rows[0]["peak_waves"], 2);
    EXPECT_EQ(cu_rows[0]["mean_waves"], 1.5);
    auto simd = run({binary(), "occupancy", "-d", output.string(), "--by", "simd", "--simd", "0", "--json"});
    ASSERT_EQ(simd.status, 0) << simd.error;
    const auto simd_rows = json::parse(simd.output)["groups"];
    ASSERT_EQ(simd_rows.size(), 1);
    EXPECT_EQ(simd_rows[0]["peak_waves"], 1);
    EXPECT_EQ(simd_rows[0]["mean_waves"], 0.5);
}

TEST_F(CliRegression, OldDigestGranularOccupancyIsUnavailableNotZero)
{
    write(output, R"({"version":1,"meta":{},"lines":[],"occupancy":{"bins":1,"t0":0,"t1":100,"total":[1]}})");
    auto result = run({binary(), "occupancy", "-d", output.string(), "--by", "cu", "--json"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["available"], false);
    EXPECT_TRUE(j["groups"].empty());
    EXPECT_NE(j["note"].get<std::string>().find("analyze"), std::string::npos);
}

TEST_F(CliRegression, NewAggregateInvalidOptionsFailBeforeDigestAccess)
{
    for (auto args : std::vector<std::vector<std::string>>{
             {"coverage", "--top", "0"},
             {"coverage", "--top", "1001"},
             {"occupancy", "--cu", "1"},
             {"occupancy", "--simd", "0"},
             {"occupancy", "--by", "cu", "--simd", "0"},
             {"occupancy", "--by", "nope"},
             {"occupancy", "--by", "simd", "--top", "1001"},
             {"occupancy", "--by", "simd", "--cu", "1junk"}
    })
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        args.insert(args.begin(), binary());
        args.insert(args.end(), {"-d", "/no/such/digest"});
        const auto result = run(args);
        EXPECT_EQ(result.status, 2) << result.error;
        EXPECT_EQ(result.error.find("cannot read digest"), std::string::npos);
    }
}

TEST_F(CliRegression, WaitStatisticsIncludeZeroAndBoundDynamicContext)
{
    makeWaitTrace();
    auto result = waitQuery({"--top", "1", "--context", "0", "--cu", "0"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["metric"], "observed wait stall");
    EXPECT_EQ(j["unit"], "cycles");
    EXPECT_EQ(j["statistics"]["count"], 3);
    EXPECT_DOUBLE_EQ(j["statistics"]["mean"].get<double>(), 4.0);
    EXPECT_NEAR(j["statistics"]["stddev"].get<double>(), 3.265986323710904, 1e-12);
    EXPECT_EQ(j["statistics"]["p95"], 8);
    EXPECT_EQ(j["statistics"]["max"], 8);
    EXPECT_EQ(j["occurrences_truncated"], true);
    ASSERT_EQ(j["occurrences"].size(), 1);
    EXPECT_EQ(j["occurrences"][0]["clock"], 140);
    EXPECT_EQ(j["occurrences"][0]["iteration"], 2);
    ASSERT_EQ(j["occurrences"][0]["context"].size(), 1);
    EXPECT_EQ(j["occurrences"][0]["context"][0]["selected"], true);
    EXPECT_EQ(j["dependencies"]["provenance"], "recorded_json");
    ASSERT_EQ(j["dependencies"]["references"].size(), 1);
    EXPECT_EQ(j["dependencies"]["references"][0]["line"], 2);
    EXPECT_EQ(j["dependencies"]["references"][0]["iteration"], 0);
    auto context = waitQuery({"--top", "1", "--context", "2"});
    ASSERT_EQ(context.status, 0) << context.error;
    const auto rows = json::parse(context.output)["occurrences"][0]["context"];
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows[0]["line"], 2);
    EXPECT_EQ(rows[0]["clock"], 110);
}

TEST_F(CliRegression, WaitOnlyLoadsSelectedWaveButAnalyzeRemainsStrict)
{
    makeWaitTrace();
    auto manifest = json::parse(read(trace / "filenames.json"));
    manifest["wave_filenames"]["0"]["0"]["0"]["1"] = {"se0_sm0_sl0_wv1.json", 0, 1000};
    write(trace / "filenames.json", manifest.dump());
    // An unrecognized optional info value causes a diagnostic, but does not
    // invalidate the wave. Malformed occupancy is intentionally still fatal.
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["info"] = {
        {"extra_label",       "not numeric"},
        {"extra_label_stall", 0            }
    };
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    const auto result = waitQuery();
    ASSERT_EQ(result.status, 0) << result.error;
    EXPECT_EQ(json::parse(result.output)["statistics"]["count"], 3);
    EXPECT_NE(result.error.find("Invalid param"), std::string::npos);
    expectFailure();
}

TEST_F(CliRegression, WaitRejectsWrongIdentityNonWaitMissingAndUnexecutedLines)
{
    makeWaitTrace();
    for (auto args : std::vector<std::vector<std::string>>{
             {"--cu",   "1"  },
             {"--wave", "1"  },
             {"--se",   "1"  },
             {"--simd", "1"  },
             {"--slot", "1"  },
             {"--line", "2"  },
             {"--line", "999"}
    })
    {
        const auto result = waitQuery(args);
        EXPECT_EQ(result.status, 1) << result.error;
        EXPECT_TRUE(result.output.empty());
    }
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["instructions"] = {
        {100, 6, 0, 1, 2}
    };
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    EXPECT_EQ(waitQuery().status, 1);
    wave["wave"]["instructions"] = {
        {100, 3, 0, 1, 1}
    };
    wave["wave"].erase("waitcnt");
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    const auto result = waitQuery();
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["statistics"]["mean"], 0);
    EXPECT_EQ(j["statistics"]["count"], 1);
    EXPECT_EQ(j["dependencies"]["available"], false);
    EXPECT_TRUE(j["dependencies"]["references"].empty());
}

TEST_F(CliRegression, WaitDependencyOutputIsBounded)
{
    makeWaitTrace();
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    auto& deps = wave["wave"]["waitcnt"][0][1] = json::array();
    for (int i = 0; i < 40; ++i) deps.push_back({2, i});
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    auto result = waitQuery();
    ASSERT_EQ(result.status, 0) << result.error;
    const auto deps_out = json::parse(result.output)["dependencies"];
    EXPECT_EQ(deps_out["total_references"], 40);
    EXPECT_EQ(deps_out["truncated"], true);
    EXPECT_EQ(deps_out["references"].size(), 32);
}

TEST_F(CliRegression, WaitPreservesFullWidthStallsFromJson)
{
    makeWaitTrace();
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["end"] = 200000;
    wave["wave"]["instructions"] = {
        {100,   3, 65536, 65537, 1},
        {70000, 3, 70000, 70001, 1}
    };
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    const auto result = waitQuery({"--top", "1", "--context", "1"});
    ASSERT_EQ(result.status, 0) << result.error;
    const auto j = json::parse(result.output);
    EXPECT_EQ(j["statistics"]["mean"], 67768);
    EXPECT_EQ(j["statistics"]["max"], 70000);
    EXPECT_EQ(j["occurrences"][0]["stall"], 70000);
    EXPECT_EQ(j["occurrences"][0]["context"][0]["stall"], 65536);
}

TEST_F(CliRegression, WaitPreservesFullWidthDecodedRecordStalls)
{
    makeWaitTrace();
    DataStore store;
    const rcv::WaveSelection selected{0, 0, 0, 0, 0};
    ASSERT_TRUE(rcv::loadTrace(trace.string(), store, rcv::ForceFormat::JsonDir, selected).ok);
    auto& entry = store.wave_hierarchy.at(0).at(0).at(0).at(0);
    wave_record_t record{};
    record.id = entry.id;
    record.cu = 0;
    record.begin = 0;
    record.end = 200000;
    record.instructions = {
        {100,   3, 65536, 65537, 1},
        {70000, 3, 70000, 70001, 1}
    };
    store.wave_records[entry.id] = record;
    const auto j = rcv::queryWait(store, selected, 1, 1, 1);
    EXPECT_EQ(j["statistics"]["mean"], 67768);
    EXPECT_EQ(j["statistics"]["max"], 70000);
    EXPECT_EQ(j["occurrences"][0]["stall"], 70000);
    EXPECT_EQ(j["occurrences"][0]["context"][0]["stall"], 65536);
}

TEST_F(CliRegression, WaitAppliesSelectedJsonWaveTimeOffset)
{
    makeWaitTrace();
    DataStore store;
    const rcv::WaveSelection selected{0, 0, 0, 0, 0};
    ASSERT_TRUE(rcv::loadTrace(trace.string(), store, rcv::ForceFormat::JsonDir, selected).ok);
    store.applyTimeOffsets({
        {0, 500}
    });
    const auto j = rcv::queryWait(store, selected, 1, 1, 0);
    EXPECT_EQ(j["occurrences"][0]["clock"], 640);
    EXPECT_EQ(j["wave"]["begin"], 500);
    EXPECT_EQ(j["statistics"]["mean"], 4);
}

TEST_F(CliRegression, WaitNearestRankAndMaximumContextRows)
{
    makeWaitTrace();
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    for (int count : {20, 21})
    {
        wave["wave"]["instructions"] = json::array();
        for (int i = 0; i < count + 20; ++i)
            wave["wave"]["instructions"].push_back(
                {i * 30, 3, i >= 10 && i < count + 10 ? i - 10 : 0, 25, i >= 10 && i < count + 10 ? 1 : 2}
            );
        wave["wave"]["end"] = 2000;
        write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
        auto result = waitQuery({"--top", "20", "--context", "10"});
        ASSERT_EQ(result.status, 0) << result.error;
        const auto j = json::parse(result.output);
        EXPECT_EQ(j["statistics"]["p95"], count == 20 ? 18 : 19);
        ASSERT_EQ(j["occurrences"].size(), 20);
        size_t rows = 0;
        for (const auto& hit : j["occurrences"]) rows += hit["context"].size();
        EXPECT_EQ(rows, 420);
    }
}

TEST_F(CliRegression, WaitValidatesRequiredSelectorsAndBoundsBeforeTraceAccess)
{
    const auto original = trace;
    trace = "/no/such/trace";
    for (const auto& option : {"--line", "--se", "--simd", "--slot", "--wave", "--cu", "--context", "--top"})
        for (const auto& value : {"-1", "1junk", "2147483648"})
        {
            const auto result = waitQuery({option, value});
            EXPECT_EQ(result.status, 2) << result.error;
            EXPECT_EQ(result.error.find("input path"), std::string::npos);
        }
    for (auto args : std::vector<std::vector<std::string>>{
             {"--top",     "0" },
             {"--top",     "21"},
             {"--context", "11"}
    })
        EXPECT_EQ(waitQuery(args).status, 2);
    const std::vector<std::string> selectors{"--line", "--se", "--simd", "--slot", "--wave"};
    for (const auto& missing : selectors)
    {
        std::vector<std::string> args{binary(), "wait", trace.string()};
        for (const auto& option : selectors)
            if (option != missing) args.insert(args.end(), {option, "0"});
        const auto result = run(args);
        EXPECT_EQ(result.status, 2) << result.error;
        EXPECT_EQ(result.error.find("input path"), std::string::npos);
    }
    trace = original;
}

TEST_F(CliRegression, RejectsMalformedListedWave)
{
    for (const auto& contents :
         {std::string("{"), std::string("{}"), std::string(R"({"wave":{"begin":0,"end":1000,"id":0}})")})
    {
        SCOPED_TRACE(contents);
        write(trace / "se0_sm0_sl0_wv0.json", contents);
        expectFailure();
    }
}

TEST_F(CliRegression, RejectsIncompleteRequiredMetadata)
{
    auto manifest = json::parse(read(trace / "filenames.json"));
    for (const auto& field : {"gfxip", "wave_filenames"})
    {
        auto broken = manifest;
        broken.erase(field);
        write(trace / "filenames.json", broken.dump());
        expectFailure();
    }
    manifest["wave_filenames"]["0"]["0"]["0"]["1"] = {"missing.json"};
    write(trace / "filenames.json", manifest.dump());
    expectFailure();
}

TEST_F(CliRegression, RejectsPartiallyParsedCode)
{
    auto code = json::parse(read(trace / "code.json"));
    code["code"][1][7] = "invalid latency";
    write(trace / "code.json", code.dump());
    expectFailure();
}

TEST_F(CliRegression, RejectsDuplicateCodeLineIdentityAndPreservesOutput)
{
    auto code = json::parse(read(trace / "code.json"));
    auto duplicate = code["code"][1];
    duplicate[2] = 1;
    code["code"].push_back(duplicate);
    write(trace / "code.json", code.dump());
    expectFailure();
}

TEST_F(CliRegression, ValidSoloWaveMayHaveZeroHiddenLatency)
{
    auto result = analyze();
    ASSERT_EQ(result.status, 0) << result.error;
    auto digest = rcv::fromJson(json::parse(read(output)));
    EXPECT_TRUE(digest.meta.hidden_latency_available);
    EXPECT_EQ(digest.meta.wave_count, 1);
    int64_t hidden = 0;
    for (const auto& line : digest.lines) hidden += line.hidden();
    EXPECT_EQ(hidden, 0);
}

TEST_F(CliRegression, FailedAnalysisDoesNotMarkPartialResultsAvailable)
{
    DataStore store;
    ASSERT_TRUE(rcv::loadTrace(trace.string(), store).ok);
    // Simulate a wave lost between load and analysis, including cache eviction.
    fs::remove(trace / "se0_sm0_sl0_wv0.json");
    WaveInstance::InvalidadeCache();
    EXPECT_FALSE(HiddenLatencyAnalysis::analyze(store, true));
    EXPECT_FALSE(store.hidden_latency_analyzed);
    EXPECT_TRUE(store.hidden_latency_by_line.empty());
}

TEST_F(CliRegression, ExplicitlyEmptyWaveInstructionsAreNotALoadFailure)
{
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["instructions"] = json::array();
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    EXPECT_EQ(analyze().status, 0);
}

TEST_F(CliRegression, EmptyWaveManifestIsReportedAsUnsupported)
{
    auto manifest = json::parse(read(trace / "filenames.json"));
    manifest["wave_filenames"] = json::object();
    write(trace / "filenames.json", manifest.dump());
    expectFailure();
}

TEST_F(CliRegression, WriteFailurePreservesExistingDigestAndCleansTemporaryFile)
{
    // Keep this fixture's serialized digest above the injected file limit.
    // Missing occupancy now correctly stays empty rather than contributing
    // thousands of invented zero bins.
    write(trace / "occupancy.json", R"({"0":[[100,0,0,0,1,0],[100,0,0,0,0,0]]})");
    write(output, "previous valid digest");
    auto result = run({binary(), "analyze", trace.string(), "-o", output.string(), "--bins", "2000"}, {}, 2048);
    EXPECT_EQ(result.signal, 0) << result.error;
    EXPECT_EQ(result.status, 1) << result.error;
    EXPECT_NE(result.error.find("error:"), std::string::npos);
    EXPECT_EQ(result.error.find("wrote "), std::string::npos);
    EXPECT_EQ(read(output), "previous valid digest");
    EXPECT_EQ(std::distance(fs::directory_iterator(tmp.path), fs::directory_iterator{}), 2);
}

TEST_F(CliRegression, RefusesSymlinkDestinationWithoutTouchingItsTarget)
{
    auto target = tmp.path / "target.json";
    write(target, "keep target");
    fs::create_symlink(target, output);
    auto result = analyze();
    EXPECT_EQ(result.status, 1) << result.error;
    EXPECT_TRUE(fs::is_symlink(output));
    EXPECT_EQ(read(target), "keep target");
}

TEST_F(CliRegression, ReplacesRegularDigestOnlyWithCompleteJson)
{
    write(output, "old digest");
    auto result = analyze();
    ASSERT_EQ(result.status, 0) << result.error;
    EXPECT_EQ(json::parse(read(output)).at("meta").at("wave_count"), 1);
    EXPECT_EQ(std::distance(fs::directory_iterator(tmp.path), fs::directory_iterator{}), 2);
}

TEST_F(CliRegression, ForcedAttWithoutAttFilesReportsMissingInputs)
{
    auto result = run({binary(), "analyze", trace.string(), "--format", "att", "-o", output.string()});
    EXPECT_EQ(result.status, 1);
    EXPECT_EQ(result.signal, 0);
    EXPECT_NE(result.error.find("no .att"), std::string::npos) << result.error;
    EXPECT_FALSE(fs::exists(output));
}

TEST_F(CliRegression, MalformedNumbersAreUsageErrorsBeforeInputAccess)
{
    for (const auto& value : {"abc", "10junk", "2147483648", "999999999999999999999", "-1", "+1", "1.5", " 1", ""})
    {
        for (auto args : std::vector<std::vector<std::string>>{
                 {"analyze", "/nonexistent/trace", "--bins", value},
                 {"hotspot", "--top", value},
                 {"asm", "--around", value},
                 {"asm", "--around", "1", "--context", value},
                 {"occupancy", "--se", value},
                 {"asm", "--range", std::string(value) + "-2"},
                 {"asm", "--range", std::string("0-") + value}
        })
        {
            SCOPED_TRACE(::testing::PrintToString(args));
            const std::string command = args.front();
            args.insert(args.begin(), binary());
            if (command != "analyze") args.insert(args.end(), {"-d", (tmp.path / "missing.json").string()});
            auto result = run(args);
            EXPECT_EQ(result.signal, 0) << result.error;
            EXPECT_EQ(result.status, 2) << result.error;
            EXPECT_NE(result.error.find("--"), std::string::npos) << result.error;
            EXPECT_EQ(result.error.find("cannot read digest"), std::string::npos);
            EXPECT_EQ(result.error.find("input path does not exist"), std::string::npos);
        }
    }
}

TEST_F(CliRegression, NumericBoundsDoNotEnableUnboundedOutput)
{
    ASSERT_EQ(analyze().status, 0);
    for (auto args : std::vector<std::vector<std::string>>{
             {"analyze", trace.string(), "--bins", "0"},
             {"analyze", trace.string(), "--bins", "4097"},
             {"hotspot", "--top", "0"},
             {"hotspot", "--top", "1001"},
             {"asm", "--around", "1", "--context", "500"},
             {"asm", "--range", "2-1"},
             {"asm", "--range", "0-1000"},
             {"asm", "--range", "0-2147483647"}
    })
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        const auto command = args.front();
        args.insert(args.begin(), binary());
        args.insert(args.end(), {command == "analyze" ? "-o" : "-d", output.string()});
        auto result = run(args);
        EXPECT_EQ(result.signal, 0);
        EXPECT_EQ(result.status, 2) << result.error;
        EXPECT_TRUE(result.output.empty());
    }
}

TEST_F(CliRegression, NumericBoundaryValuesRemainUsable)
{
    ASSERT_EQ(analyze().status, 0);
    for (auto args : std::vector<std::vector<std::string>>{
             {"hotspot", "--top", "1"},
             {"hotspot", "--top", "1000"},
             {"asm", "--around", "0", "--context", "0"},
             {"asm", "--around", "2147483647", "--context", "499"},
             {"asm", "--range", "2147483647-2147483647"},
             {"asm", "--range", "0-999"},
             {"occupancy", "--se", "0"},
             {"occupancy", "--se", "2147483647"}
    })
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        args.insert(args.begin(), binary());
        args.insert(args.end(), {"-d", output.string(), "--json"});
        auto result = run(args);
        EXPECT_EQ(result.status, 0) << result.error;
        EXPECT_EQ(result.signal, 0);
    }
}

TEST_F(CliRegression, AsmRejectsConflictingSelectorsBeforeDigestAccess)
{
    for (auto args : std::vector<std::vector<std::string>>{
             {"--range", "0-2", "--around", "1"},
             {"--around", "1", "--range", "0-2"},
             {"--range", "0-2", "--context", "1"},
             {"--context", "1"},
             {}
    })
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        args.insert(args.begin(), {binary(), "asm", "-d", (tmp.path / "missing.json").string()});
        auto result = run(args);
        EXPECT_EQ(result.status, 2) << result.error;
        EXPECT_EQ(result.signal, 0);
        EXPECT_EQ(result.error.find("cannot read digest"), std::string::npos) << result.error;
    }
}

TEST_F(CliRegression, AsmPreservesInclusiveRangeAndAroundContext)
{
    ASSERT_EQ(analyze().status, 0);
    auto range = run({binary(), "asm", "-d", output.string(), "--range", "1-2", "--json"});
    ASSERT_EQ(range.status, 0) << range.error;
    auto rows = json::parse(range.output);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["index"], 1);
    EXPECT_EQ(rows[1]["index"], 2);
    auto around = run({binary(), "asm", "-d", output.string(), "--context", "0", "--around", "1", "--json"});
    ASSERT_EQ(around.status, 0) << around.error;
    rows = json::parse(around.output);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["index"], 1);
}

TEST_F(CliRegression, WaveDigestPreservesKnownCuAndLeavesUnknownCuUnset)
{
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    for (int cu : {7, -1})
    {
        if (cu >= 0)
            wave["wave"]["cu"] = cu;
        else
            wave["wave"].erase("cu");
        write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
        auto result = analyze();
        ASSERT_EQ(result.status, 0) << result.error;
        auto digest = rcv::fromJson(json::parse(read(output)));
        ASSERT_EQ(digest.waves.size(), 1u);
        const auto& actual = digest.waves[0];
        EXPECT_EQ(actual.cu, cu);
        EXPECT_EQ(actual.se, 0);
        EXPECT_EQ(actual.simd, 0);
        EXPECT_EQ(actual.slot, 0);
        EXPECT_EQ(actual.begin, 0);
        EXPECT_EQ(actual.end, 1000);
    }
}

TEST_F(CliRegression, RejectsWaveReferencingMissingCodeLine)
{
    auto wave = json::parse(read(trace / "se0_sm0_sl0_wv0.json"));
    wave["wave"]["instructions"][0][4] = 999;
    write(trace / "se0_sm0_sl0_wv0.json", wave.dump());
    expectFailure();
}

TEST_F(CliRegression, RejectsManifestCoordinatesThatWouldOverwriteAWave)
{
    auto manifest = json::parse(read(trace / "filenames.json"));
    manifest["wave_filenames"]["00"] = manifest["wave_filenames"]["0"];
    manifest["wave_filenames"]["0"]["0"]["0"]["0"][0] = "missing.json";
    write(trace / "filenames.json", manifest.dump());
    expectFailure();
}

TEST_F(CliRegression, RejectsMalformedOccupancyWhenPresent)
{
    for (const auto& contents : {std::string("{"), std::string(R"({"0":[[100,1]]})")})
    {
        write(trace / "occupancy.json", contents);
        expectFailure();
    }
}

TEST_F(CliRegression, MissingAndEmptyOccupancyRemainUnavailable)
{
    for (const auto& occupancy : {std::string(), std::string("{}")})
    {
        SCOPED_TRACE(occupancy.empty() ? "missing occupancy" : "empty occupancy");
        if (occupancy.empty())
            fs::remove(trace / "occupancy.json");
        else
            write(trace / "occupancy.json", occupancy);
        auto result = analyze();
        ASSERT_EQ(result.status, 0) << result.error;
        const auto digest = json::parse(read(output));
        EXPECT_TRUE(digest.at("occupancy").at("total").empty());
        const auto summary = summaryJson().at("occupancy");
        EXPECT_EQ(summary.at("available"), false);
        EXPECT_TRUE(summary.at("peak_waves").is_null());
        EXPECT_TRUE(summary.at("mean_waves").is_null());
    }
}

TEST_F(CliRegression, MeasuredZeroOccupancyRemainsAvailable)
{
    write(trace / "occupancy.json", R"({"0":[[100,0,0,0,1,0],[100,0,0,0,0,0]]})");
    auto result = analyze();
    ASSERT_EQ(result.status, 0) << result.error;
    const auto digest = json::parse(read(output));
    ASSERT_EQ(digest.at("occupancy").at("total").size(), 200u);
    for (const auto& value : digest.at("occupancy").at("total")) EXPECT_EQ(value, 0.0);
    const auto summary = summaryJson().at("occupancy");
    EXPECT_EQ(summary.at("available"), true);
    EXPECT_EQ(summary.at("peak_waves"), 0.0);
    EXPECT_EQ(summary.at("mean_waves"), 0.0);
}
