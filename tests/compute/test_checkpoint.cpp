#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/compute/checkpoint.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/sha256.hpp"

using zetta::compute::JobKind;
using zetta::Node;
using zetta::NodeConfig;
using zetta::query::QueryService;
using zetta::util::sha256_hex;
using zetta::compute::CheckpointCode;
using zetta::compute::CheckpointStatus;
using zetta::compute::FileCheckpointStore;
using zetta::compute::JobCheckpoint;
using zetta::compute::parse_job_checkpoint;
using zetta::compute::ResumableConfig;
using zetta::compute::ResumableResult;
using zetta::compute::run_resumable_rounds;
using zetta::compute::validate_job_checkpoint;
using zetta::test::TempDir;

namespace {

// A deterministic round step: append the round index to a colon-joined trail, so
// the final state records exactly which rounds ran, in order.
std::string append_round(std::uint64_t round, const std::string& state) {
    return state.empty() ? std::to_string(round)
                         : state + ":" + std::to_string(round);
}

}  // namespace

// ---- JobCheckpoint value + schema boundary ---------------------------------

TEST(JobCheckpoint, MakeStampsHashProgressAndRoundTrips) {
    const JobCheckpoint cp =
        JobCheckpoint::make("job-1", JobKind::Train, 3, 10, "partial-state-bytes");
    EXPECT_EQ(cp.job_id, "job-1");
    EXPECT_EQ(cp.kind, JobKind::Train);
    EXPECT_EQ(cp.sequence, 3u);
    EXPECT_EQ(cp.total, 10u);
    EXPECT_EQ(cp.state_sha256, sha256_hex("partial-state-bytes"));
    EXPECT_DOUBLE_EQ(cp.progress(), 0.3);
    EXPECT_FALSE(cp.created.empty());

    std::string error;
    const std::optional<JobCheckpoint> parsed =
        parse_job_checkpoint(cp.to_json(), &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->job_id, cp.job_id);
    EXPECT_EQ(parsed->kind, cp.kind);
    EXPECT_EQ(parsed->sequence, cp.sequence);
    EXPECT_EQ(parsed->total, cp.total);
    EXPECT_EQ(parsed->state, "partial-state-bytes");  // base64 round-trips the body
    EXPECT_EQ(parsed->state_sha256, cp.state_sha256);
}

TEST(JobCheckpoint, ProgressIsZeroWhenTotalUnknown) {
    const JobCheckpoint cp = JobCheckpoint::make("j", JobKind::Ddf, 5, 0, "s");
    EXPECT_DOUBLE_EQ(cp.progress(), 0.0);
}

TEST(JobCheckpoint, BinarySafeStateRoundTrips) {
    std::string binary;
    for (int b = 0; b < 256; ++b) binary.push_back(static_cast<char>(b));
    const JobCheckpoint cp =
        JobCheckpoint::make("bin", JobKind::Score, 1, 1, binary);
    std::string error;
    const std::optional<JobCheckpoint> parsed =
        parse_job_checkpoint(cp.to_json(), &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->state, binary);
}

TEST(JobCheckpoint, RejectsMalformedShapeAtBoundary) {
    std::string error;
    // Missing required fields.
    EXPECT_FALSE(validate_job_checkpoint(nlohmann::json{{"job_id", "j"}}, &error));
    EXPECT_FALSE(error.empty());
    // Unknown kind.
    nlohmann::json bad = JobCheckpoint::make("j", JobKind::Train, 0, 1, "s").to_json();
    bad["kind"] = "not_a_kind";
    EXPECT_FALSE(validate_job_checkpoint(bad, &error));
    // Unknown property.
    nlohmann::json extra =
        JobCheckpoint::make("j", JobKind::Train, 0, 1, "s").to_json();
    extra["surprise"] = 1;
    EXPECT_FALSE(validate_job_checkpoint(extra, &error));
}

TEST(JobCheckpoint, RejectsHashMismatch) {
    nlohmann::json tampered =
        JobCheckpoint::make("j", JobKind::Train, 2, 4, "real-state").to_json();
    // Shape stays valid, but the recorded hash no longer matches the body.
    tampered["state_sha256"] = sha256_hex("a-different-body");
    EXPECT_TRUE(validate_job_checkpoint(tampered));  // shape ok
    std::string error;
    EXPECT_FALSE(parse_job_checkpoint(tampered, &error));  // integrity fails
    EXPECT_NE(error.find("hash mismatch"), std::string::npos);
}

// ---- FileCheckpointStore ----------------------------------------------------

TEST(FileCheckpointStore, SaveLatestSupersedeClear) {
    TempDir tmp;
    FileCheckpointStore store(tmp.path() / "checkpoints");

    // No checkpoint yet → NotFound (start fresh).
    CheckpointStatus status;
    EXPECT_FALSE(store.latest("job-A", &status));
    EXPECT_EQ(status.code, CheckpointCode::NotFound);

    ASSERT_TRUE(store.save(JobCheckpoint::make("job-A", JobKind::Train, 1, 5, "s1")));
    ASSERT_TRUE(store.save(JobCheckpoint::make("job-A", JobKind::Train, 2, 5, "s2")));
    ASSERT_TRUE(store.save(JobCheckpoint::make("job-A", JobKind::Train, 3, 5, "s3")));

    // Latest is the highest sequence.
    const std::optional<JobCheckpoint> latest = store.latest("job-A");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->sequence, 3u);
    EXPECT_EQ(latest->state, "s3");

    // job_ids recovers the original key from the (hashed) folder.
    const std::vector<std::string> ids = store.job_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "job-A");

    EXPECT_TRUE(store.clear("job-A"));
    EXPECT_FALSE(store.latest("job-A"));
    EXPECT_TRUE(store.clear("job-A"));  // idempotent
    EXPECT_TRUE(store.job_ids().empty());
}

TEST(FileCheckpointStore, FallsBackPastACorruptHeadCheckpoint) {
    TempDir tmp;
    const std::filesystem::path root = tmp.path() / "checkpoints";
    FileCheckpointStore store(root);
    ASSERT_TRUE(store.save(JobCheckpoint::make("job-B", JobKind::Train, 1, 3, "good")));
    ASSERT_TRUE(store.save(JobCheckpoint::make("job-B", JobKind::Train, 2, 3, "good2")));

    // Corrupt the highest-sequence file (a torn write), as an eviction mid-save
    // would leave it.
    const std::filesystem::path dir = root / sha256_hex("job-B");
    const std::filesystem::path head =
        dir / "00000000000000000002.json";
    ASSERT_TRUE(std::filesystem::exists(head));
    { std::ofstream(head, std::ios::trunc) << "{ this is not valid json"; }

    // latest() skips the corrupt head and returns the previous good checkpoint.
    CheckpointStatus status;
    const std::optional<JobCheckpoint> latest = store.latest("job-B", &status);
    ASSERT_TRUE(latest) << status.message;
    EXPECT_EQ(latest->sequence, 1u);
    EXPECT_EQ(latest->state, "good");
}

TEST(FileCheckpointStore, EmptyJobIdIsRejected) {
    TempDir tmp;
    FileCheckpointStore store(tmp.path() / "cp");
    EXPECT_EQ(store.save(JobCheckpoint::make("", JobKind::Train, 1, 1, "s")).code,
              CheckpointCode::InvalidArgument);
    CheckpointStatus status;
    EXPECT_FALSE(store.latest("", &status));
    EXPECT_EQ(status.code, CheckpointCode::InvalidArgument);
}

// ---- run_resumable_rounds (the checkpoint / restart capability) -------------

TEST(RunResumableRounds, FreshRunCompletesEveryRound) {
    TempDir tmp;
    FileCheckpointStore store(tmp.path() / "cp");
    ResumableConfig config;
    config.total_rounds = 6;

    const ResumableResult result = run_resumable_rounds(
        "fresh", JobKind::Train, store, config, "", append_round);

    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.resumed_from, 0u);
    EXPECT_EQ(result.rounds_executed, 6u);
    EXPECT_EQ(result.state, "0:1:2:3:4:5");
    // clear_on_complete (default) discards the checkpoints once done.
    EXPECT_FALSE(store.latest("fresh"));
}

// The headline acceptance: a job evicted partway resumes from its last checkpoint
// and never recomputes the rounds already committed; the result is identical to an
// uninterrupted run.
TEST(RunResumableRounds, ResumesAfterEvictionWithoutRecompute) {
    TempDir tmp;
    FileCheckpointStore store(tmp.path() / "cp");
    ResumableConfig config;
    config.total_rounds = 10;
    config.clear_on_complete = false;  // keep checkpoints so we can inspect them

    // Track every round that actually executes across BOTH invocations.
    std::vector<std::uint64_t> executed;
    const auto step = [&](std::uint64_t round, const std::string& state) {
        executed.push_back(round);
        return append_round(round, state);
    };

    // First attempt: a spot-eviction notice arrives after 4 rounds → drain. The
    // predicate is checked once per completed round, so draining on the 4th check
    // stops with exactly 4 rounds committed.
    int rounds_this_attempt = 0;
    const auto evict_after_4 = [&] { return ++rounds_this_attempt >= 4; };
    const ResumableResult first = run_resumable_rounds(
        "evicted", JobKind::Train, store, config, "", step, evict_after_4);
    EXPECT_FALSE(first.completed);
    EXPECT_EQ(first.resumed_from, 0u);
    EXPECT_EQ(first.rounds_executed, 4u);
    // The drain checkpointed progress at round 4.
    const std::optional<JobCheckpoint> mid = store.latest("evicted");
    ASSERT_TRUE(mid);
    EXPECT_EQ(mid->sequence, 4u);

    // Restart: a fresh driver (a surviving / replacement node) over the SAME store.
    const ResumableResult second = run_resumable_rounds(
        "evicted", JobKind::Train, store, config, "", step);
    EXPECT_TRUE(second.completed);
    EXPECT_EQ(second.resumed_from, 4u);          // picked up where it left off
    EXPECT_EQ(second.rounds_executed, 6u);       // only the remaining rounds
    EXPECT_EQ(second.state, "0:1:2:3:4:5:6:7:8:9");

    // No round was recomputed: each of 0..9 ran exactly once across both attempts.
    std::vector<std::uint64_t> expected = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    EXPECT_EQ(executed, expected);

    // The result matches an uninterrupted run.
    FileCheckpointStore fresh_store(tmp.path() / "cp-fresh");
    const ResumableResult uninterrupted = run_resumable_rounds(
        "control", JobKind::Train, fresh_store, config, "", append_round);
    EXPECT_EQ(second.state, uninterrupted.state);
}

TEST(RunResumableRounds, AlreadyCompleteJobDoesNotRerun) {
    TempDir tmp;
    FileCheckpointStore store(tmp.path() / "cp");
    // A checkpoint that says all 3 rounds are done.
    ASSERT_TRUE(store.save(JobCheckpoint::make("done", JobKind::Train, 3, 3, "final")));
    ResumableConfig config;
    config.total_rounds = 3;
    config.clear_on_complete = false;

    bool ran = false;
    const ResumableResult result = run_resumable_rounds(
        "done", JobKind::Train, store, config, "ignored",
        [&](std::uint64_t, const std::string& s) { ran = true; return s; });
    EXPECT_TRUE(result.completed);
    EXPECT_FALSE(ran);  // nothing left to do
    EXPECT_EQ(result.state, "final");
}

TEST(RunResumableRounds, CorruptCheckpointFailsHardRatherThanResumingFromGarbage) {
    TempDir tmp;
    const std::filesystem::path root = tmp.path() / "cp";
    FileCheckpointStore store(root);
    ASSERT_TRUE(store.save(JobCheckpoint::make("c", JobKind::Train, 2, 5, "s2")));
    // Tamper with the only checkpoint's body so its hash no longer matches.
    const std::filesystem::path file =
        root / sha256_hex("c") / "00000000000000000002.json";
    nlohmann::json value;
    { std::ifstream in(file); in >> value; }
    value["state"] = "dGFtcGVyZWQ=";  // base64("tampered") — hash now mismatches
    { std::ofstream(file, std::ios::trunc) << value.dump(); }

    ResumableConfig config;
    config.total_rounds = 5;
    const ResumableResult result = run_resumable_rounds(
        "c", JobKind::Train, store, config, "", append_round);
    EXPECT_FALSE(result.completed);
    EXPECT_EQ(result.status.code, CheckpointCode::Corrupt);
}

TEST(RunResumableRounds, PeriodicCheckpointEveryNRounds) {
    TempDir tmp;
    FileCheckpointStore store(tmp.path() / "cp");
    ResumableConfig config;
    config.total_rounds = 7;
    config.checkpoint_every = 3;
    config.clear_on_complete = false;

    const ResumableResult result = run_resumable_rounds(
        "p", JobKind::Train, store, config, "", append_round);
    ASSERT_TRUE(result.completed);
    // Checkpoints at 3, 6, and the final 7 → latest is the completed 7.
    const std::optional<JobCheckpoint> latest = store.latest("p");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->sequence, 7u);
}

// ---- system.checkpoints (RULE-7 observability view) ------------------------

TEST(SystemCheckpoints, ExposesNodeCheckpointsAsAReadOnlyTable) {
    TempDir tmp;
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    Node node{cfg};
    node.start();

    // A node owns a local checkpoint store under <data_dir>/checkpoints.
    ASSERT_TRUE(node.checkpoint_store().save(
        JobCheckpoint::make("job-x", JobKind::Train, 3, 10, "state-x")));
    ASSERT_TRUE(node.checkpoint_store().save(
        JobCheckpoint::make("job-y", JobKind::Ddf, 1, 4, "state-y")));

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.checkpoints;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 2u);
    // checkpoints() is sorted by job id (job-x before job-y).
    EXPECT_EQ(sel.rows[0]["job_id"].get<std::string>(), "job-x");
    EXPECT_EQ(sel.rows[0]["kind"].get<std::string>(), "train");
    EXPECT_EQ(sel.rows[0]["sequence"].get<int>(), 3);
    EXPECT_EQ(sel.rows[0]["total"].get<int>(), 10);
    EXPECT_DOUBLE_EQ(sel.rows[0]["progress"].get<double>(), 0.3);
    EXPECT_EQ(sel.rows[1]["kind"].get<std::string>(), "ddf");

    // Projection + WHERE work over the computed rows like any system table.
    auto filtered =
        svc.run("SELECT job_id FROM system.checkpoints WHERE kind = 'ddf';", "");
    ASSERT_TRUE(filtered.ok()) << filtered.error;
    ASSERT_EQ(filtered.rows.size(), 1u);
    EXPECT_EQ(filtered.rows[0]["job_id"].get<std::string>(), "job-y");

    node.stop();
}
