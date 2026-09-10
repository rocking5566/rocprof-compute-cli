#include <gtest/gtest.h>
#include "test_support.h"
#include "analysis/hidden_latency.h"
#include "cli/digest.h"
#include "data/datastore.h"
#include "data/shaderdata.h"
#include "data/trace_loader.h"
#include "data/wavemanager.h"

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

TEST_F(CliRegression, RejectsMalformedListedWave)
{
    for (const auto& contents : {std::string("{"), std::string("{}"),
                              std::string(R"({"wave":{"begin":0,"end":1000,"id":0}})")})
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
                 {"hotspot", "--top", value}, {"asm", "--around", value},
                 {"asm", "--around", "1", "--context", value}, {"occupancy", "--se", value},
                 {"asm", "--range", std::string(value) + "-2"},
                 {"asm", "--range", std::string("0-") + value}})
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
             {"analyze", trace.string(), "--bins", "0"}, {"analyze", trace.string(), "--bins", "4097"},
             {"hotspot", "--top", "0"}, {"hotspot", "--top", "1001"},
             {"asm", "--around", "1", "--context", "500"}, {"asm", "--range", "2-1"},
             {"asm", "--range", "0-1000"}, {"asm", "--range", "0-2147483647"}})
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
             {"hotspot", "--top", "1"}, {"hotspot", "--top", "1000"},
             {"asm", "--around", "0", "--context", "0"},
             {"asm", "--around", "2147483647", "--context", "499"},
             {"asm", "--range", "2147483647-2147483647"}, {"asm", "--range", "0-999"},
             {"occupancy", "--se", "0"}, {"occupancy", "--se", "2147483647"}})
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
             {"--range", "0-2", "--around", "1"}, {"--around", "1", "--range", "0-2"},
             {"--range", "0-2", "--context", "1"}, {"--context", "1"}, {}})
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
        if (cu >= 0) wave["wave"]["cu"] = cu;
        else wave["wave"].erase("cu");
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
