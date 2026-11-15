// SPDX-License-Identifier: MIT

// The activity feed (#622): activity_entries composes ONE chronological view
// from the node's existing sources — the audit trail + runs — with the surface
// (local / console / api) and outcome derived per entry, newest first, and no
// stored copy (RULE-7: every call recomputes the merge).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/pipeline/run.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::AuditAction;
using zetta::observability::ActivityEntry;
using zetta::observability::ActivityOutcome;
using zetta::observability::ActivitySurface;
using zetta::observability::activity_entries;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// The first entry whose action matches, or nullptr.
const ActivityEntry* find_action(const std::vector<ActivityEntry>& entries,
                                 const std::string& action) {
    for (const ActivityEntry& entry : entries) {
        if (entry.action == action) return &entry;
    }
    return nullptr;
}

}  // namespace

TEST(ActivityFeed, EmptyNodeHasNoActivity) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_TRUE(activity_entries(node).empty());
    node.stop();
}

TEST(ActivityFeed, AuditEventsBecomeEntriesWithActorAndOutcome) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    node.record_audit_event({AuditAction::KeyIssued, "alice", "key-1", true,
                             "issued for the burst node", {}});

    const std::vector<ActivityEntry> entries = activity_entries(node);
    ASSERT_EQ(entries.size(), 1u);
    const ActivityEntry& entry = entries.front();
    EXPECT_EQ(entry.action, "key issued");
    EXPECT_EQ(entry.actor, "alice");
    EXPECT_EQ(entry.target, "key-1");
    EXPECT_EQ(entry.surface, ActivitySurface::Local);
    EXPECT_EQ(entry.outcome, ActivityOutcome::Ok);
    EXPECT_EQ(entry.ref_kind, "audit");
    EXPECT_FALSE(entry.ref_id.empty());  // the audit event's id
    EXPECT_FALSE(entry.time.empty());    // the sink stamped it

    node.stop();
}

TEST(ActivityFeed, ApiTargetsClassifyAsApiSurface) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // The shape the capability registry records for every call (#615): a
    // data_access event whose target is the api: resource, actor = acting key.
    node.record_audit_event(
        {AuditAction::DataAccess, "key-1", "api:sql", true, "read call", {}});
    node.record_audit_event({AuditAction::DataAccess, "key-2", "api:create_project",
                             false, "capability api:create_project not granted",
                             {}});

    const std::vector<ActivityEntry> entries = activity_entries(node);
    ASSERT_EQ(entries.size(), 2u);
    for (const ActivityEntry& entry : entries) {
        EXPECT_EQ(entry.surface, ActivitySurface::Api);
        EXPECT_EQ(entry.action, "data access");
    }
    // A denied data access is Denied (governance refused), not Failed.
    const ActivityEntry* denied = nullptr;
    for (const ActivityEntry& entry : entries) {
        if (entry.outcome == ActivityOutcome::Denied) denied = &entry;
    }
    ASSERT_NE(denied, nullptr);
    EXPECT_EQ(denied->actor, "key-2");

    node.stop();
}

TEST(ActivityFeed, ConsoleDetailClassifiesAsConsoleSurface) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // The shape the governed console records per statement (FEAT-9).
    node.record_audit_event({AuditAction::DataAccess, "key-3", "shop", true,
                             "console sql ok · SELECT * FROM items", {}});

    const std::vector<ActivityEntry> entries = activity_entries(node);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().surface, ActivitySurface::Console);

    node.stop();
}

TEST(ActivityFeed, RunsContributeCreatedAndTerminalEntries) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto run = node.create_run(/*project=*/"", /*graph=*/"churn_v1");
    ASSERT_NE(run, nullptr);

    // A pending run contributes one Info entry.
    {
        const std::vector<ActivityEntry> entries = activity_entries(node);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries.front().action, "run created");
        EXPECT_EQ(entries.front().outcome, ActivityOutcome::Info);
        EXPECT_EQ(entries.front().target, run->id());
        EXPECT_EQ(entries.front().ref_kind, "run");
        EXPECT_EQ(entries.front().ref_id, run->id());
        EXPECT_NE(entries.front().detail.find("churn_v1"), std::string::npos);
    }

    // Running is not terminal: still just the created entry.
    run->set_status(zetta::pipeline::RunStatus::Running);
    EXPECT_EQ(activity_entries(node).size(), 1u);

    // A failed run adds a "run failed" entry carrying the engine's last error
    // log line verbatim.
    run->add_log({"", "error", "step 'train' failed: no rows"});
    run->set_status(zetta::pipeline::RunStatus::Failed);
    const std::vector<ActivityEntry> entries = activity_entries(node);
    ASSERT_EQ(entries.size(), 2u);
    const ActivityEntry* failed = find_action(entries, "run failed");
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->outcome, ActivityOutcome::Failed);
    EXPECT_NE(failed->detail.find("step 'train' failed: no rows"),
              std::string::npos);

    node.stop();
}

TEST(ActivityFeed, JobsEnterThroughTheirAuditEvents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto job = node.create_job(zetta::compute::JobKind::Train);
    ASSERT_NE(job, nullptr);

    // create_job records a job_created audit event; the feed links it to the
    // Jobs surface through ref_kind/ref_id (the audit target is the job id).
    const std::vector<ActivityEntry> entries = activity_entries(node);
    const ActivityEntry* queued = find_action(entries, "job queued");
    ASSERT_NE(queued, nullptr);
    EXPECT_EQ(queued->ref_kind, "job");
    EXPECT_EQ(queued->ref_id, job->id());

    // A terminal job adds a job_finished audit event; a failed one is Failed.
    job->set_status(zetta::compute::JobStatus::Failed, "device lost");
    const std::vector<ActivityEntry> after = activity_entries(node);
    const ActivityEntry* finished = find_action(after, "job finished");
    ASSERT_NE(finished, nullptr);
    EXPECT_EQ(finished->outcome, ActivityOutcome::Failed);

    node.stop();
}

TEST(ActivityFeed, EntriesAreNewestFirst) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    node.record_audit_event(
        {AuditAction::KeyIssued, "alice", "key-1", true, "first", {}});
    auto run = node.create_run();
    ASSERT_NE(run, nullptr);
    run->set_status(zetta::pipeline::RunStatus::Succeeded);
    node.record_audit_event(
        {AuditAction::KeyRevoked, "alice", "key-1", true, "last", {}});

    const std::vector<ActivityEntry> entries = activity_entries(node);
    ASSERT_GE(entries.size(), 4u);  // 2 audit + run created + run succeeded
    for (std::size_t i = 1; i < entries.size(); ++i) {
        EXPECT_GE(entries[i - 1].time, entries[i].time)
            << "entry " << i << " is newer than the one above it";
    }

    node.stop();
}

TEST(ActivityFeed, WireNamesAreStable) {
    EXPECT_EQ(to_string(ActivitySurface::Local), "local");
    EXPECT_EQ(to_string(ActivitySurface::Console), "console");
    EXPECT_EQ(to_string(ActivitySurface::Api), "api");
    EXPECT_EQ(to_string(ActivityOutcome::Ok), "ok");
    EXPECT_EQ(to_string(ActivityOutcome::Denied), "denied");
    EXPECT_EQ(to_string(ActivityOutcome::Failed), "failed");
    EXPECT_EQ(to_string(ActivityOutcome::Info), "info");
}

TEST(ActivityFeed, DatasetAddAndCloudInspectEnterTheFeed) {
    // #624: the two backgrounded actions surface their completion here — a
    // dataset add via its dataset_added audit event (deep-linkable to the
    // Datasets view) and a consent-approved cloud inspect via cloud_inspected.
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    zetta::governance::AuditEventValue dataset_added;
    dataset_added.action = AuditAction::DatasetAdded;
    dataset_added.target = "table-uuid-1";
    dataset_added.detail = "dataset 'events' added: 12 row(s), 8 partition(s), csv";
    node.record_audit_event(dataset_added);

    zetta::governance::AuditEventValue cloud_inspected;
    cloud_inspected.action = AuditAction::CloudInspected;
    cloud_inspected.target = "aws";
    cloud_inspected.allowed = false;  // the inspect ran but never reached AWS
    cloud_inspected.detail = "aws CLI not found";
    node.record_audit_event(cloud_inspected);

    const std::vector<ActivityEntry> entries = activity_entries(node);

    const ActivityEntry* dataset = find_action(entries, "dataset added");
    ASSERT_NE(dataset, nullptr);
    EXPECT_EQ(dataset->ref_kind, "dataset");
    EXPECT_EQ(dataset->ref_id, "table-uuid-1");
    EXPECT_EQ(dataset->outcome, ActivityOutcome::Ok);

    const ActivityEntry* inspect = find_action(entries, "cloud inspected");
    ASSERT_NE(inspect, nullptr);
    EXPECT_EQ(inspect->ref_kind, "cloud");
    EXPECT_EQ(inspect->ref_id, "aws");
    EXPECT_EQ(inspect->outcome, ActivityOutcome::Failed);
    EXPECT_EQ(inspect->detail, "aws CLI not found");

    node.stop();
}
