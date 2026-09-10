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
