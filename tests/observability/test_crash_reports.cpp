// SPDX-License-Identifier: MIT

// Crash reports (docs/architecture/observability.md, #640): a fatal signal
// writes <data_dir>/crashes/<uuid>.json (stacktrace + recent spans + version
// + platform) before the process dies; the files are validated when read back
// and surfaced by the system.crashes view.
#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"
#include "zetta/version.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;
using zetta::observability::CrashHandlerOptions;
using zetta::observability::CrashReport;
using zetta::observability::ScopedSpan;
using zetta::query::QueryService;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

std::string crashes_dir_of(const TempDir& tmp) {
    return (tmp.path() / "crashes").string();
}

}  // namespace

// The full crash contract, end to end: the handler (installed in the death
// statement's child process) writes a report the parent reads back —
// validated, with a stacktrace, the recorded span, and version/platform.
// NOTE: the default "fast" death-test style (fork, no re-exec) is required —
// the child must inherit THIS process's TempDir path and recorder. No threads
// are running here, so the fork is safe.
TEST(CrashReportsDeathTest, FatalSignalWritesAValidatedReport) {
    TempDir tmp;
    const std::string crashes_dir = crashes_dir_of(tmp);

    zetta::observability::SpanRecorder recorder;
    const auto trace = zetta::observability::make_root_trace();
    {
        ScopedSpan span(&recorder, trace, "api:sql");
        span.set_attribute("intent", "the span a crash should preserve");
        span.finish(true);
    }

    EXPECT_DEATH(
        {
            CrashHandlerOptions options;
            options.crashes_dir = crashes_dir;
            options.recorder = &recorder;
            if (zetta::observability::install_crash_handler(options)) {
                std::raise(SIGSEGV);
            }
        },
        "");

    const auto loaded = zetta::observability::load_crash_reports(crashes_dir);
    ASSERT_EQ(loaded.size(), 1u);
    ASSERT_TRUE(loaded[0].report.has_value()) << loaded[0].error;
    const CrashReport& report = *loaded[0].report;
    EXPECT_EQ(report.signal_name, "SIGSEGV");
    EXPECT_EQ(report.signal_number, SIGSEGV);
    EXPECT_EQ(report.version, zetta::kVersion);
    EXPECT_FALSE(report.platform.empty());
    EXPECT_FALSE(report.crashed_at.empty());
    EXPECT_FALSE(report.stacktrace.empty());
    ASSERT_EQ(report.recent_spans.size(), 1u);
    EXPECT_EQ(report.recent_spans[0].at("name"), "api:sql");
    EXPECT_EQ(report.recent_spans[0].at("attributes").at("intent"),
              "the span a crash should preserve");
}

TEST(CrashReports, RoundTripsThroughSchemaValidation) {
    CrashReport report;
    report.id = "test-report";
    report.signal_number = SIGABRT;
    report.signal_name = "SIGABRT";
    report.crashed_at = "2026-07-02T00:00:00Z";
    report.version = zetta::kVersion;
    report.platform = "macos-arm64";
    report.stacktrace = {"frame 0", "frame 1"};

    std::string reason;
    const auto parsed =
        zetta::observability::parse_crash_report(to_json(report), &reason);
    ASSERT_TRUE(parsed.has_value()) << reason;
    EXPECT_EQ(parsed->signal_name, "SIGABRT");
    EXPECT_EQ(parsed->stacktrace.size(), 2u);

    // Malformed input is refused with a path-qualified reason, never accepted.
    const auto rejected = zetta::observability::parse_crash_report(
        nlohmann::json{{"id", "x"}}, &reason);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_FALSE(reason.empty());
}

TEST(CrashReports, SystemCrashesViewSurfacesReportsAndMalformedFiles) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    QueryService svc{node};

    // No crashes directory at all: an empty view, not an error.
    auto empty = svc.run("SELECT * FROM system.crashes;", "");
    ASSERT_TRUE(empty.ok()) << empty.error;
    EXPECT_TRUE(empty.rows.empty());

    const std::filesystem::path crashes_dir = tmp.path() / "crashes";
    std::filesystem::create_directories(crashes_dir);

    CrashReport report;
    report.id = "aaaa";
    report.signal_number = SIGSEGV;
    report.signal_name = "SIGSEGV";
    report.crashed_at = "2026-07-02T00:00:00Z";
    report.version = zetta::kVersion;
    report.platform = "macos-arm64";
    report.stacktrace = {"frame 0"};
    {
        std::ofstream out(crashes_dir / "aaaa.json");
        out << to_json(report).dump(2);
    }
    {
        std::ofstream out(crashes_dir / "bbbb.json");
        out << "{ this is not json";
    }

    auto result = svc.run("SELECT * FROM system.crashes;", "");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 2u);

    const auto& valid_row = result.rows[0];
    EXPECT_TRUE(valid_row.at("valid").get<bool>());
    EXPECT_EQ(valid_row.at("id"), "aaaa");
    EXPECT_EQ(valid_row.at("signal_name"), "SIGSEGV");
    EXPECT_EQ(valid_row.at("frame_count").get<int>(), 1);

    const auto& malformed_row = result.rows[1];
    EXPECT_FALSE(malformed_row.at("valid").get<bool>());
    EXPECT_NE(malformed_row.at("error").get<std::string>().find("bbbb.json"),
              std::string::npos);

    node.stop();
}
