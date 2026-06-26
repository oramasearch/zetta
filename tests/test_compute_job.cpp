#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/replication.hpp"
#include "zetta/uuid.hpp"

using zetta::AuditAction;
using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::ComputeJob;
using zetta::ComputeJobValue;
using zetta::JobKind;
using zetta::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::query::QueryService;
using zetta::ReplicationManager;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A sample job value with every part populated, used to assert it round-trips
// through disk + the CRDT.
ComputeJobValue sample_value() {
    ComputeJobValue value;
    value.kind = JobKind::Train;
    value.status = JobStatus::Succeeded;
    value.project = "project-uuid";
    value.graph = "graph-1";
    value.run = "run-uuid";
    value.scaling_policy = "policy-uuid";
    value.detail = "trained on 12 nodes";
    return value;
}

// Tick both nodes until `done` holds (or we give up). Mirrors the run / project
// replication tests: capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

TEST(ComputeJob, CreateUpdateQuery) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // Create: starts Queued with create == update, holds the kind + references.
    auto job = node.create_job(JobKind::Train, "project-uuid", "graph-1", {},
                               "policy-uuid");
    ASSERT_TRUE(job);
    EXPECT_EQ(job->kind(), JobKind::Train);
    EXPECT_EQ(job->status(), JobStatus::Queued);
    EXPECT_EQ(job->project(), "project-uuid");
    EXPECT_EQ(job->graph(), "graph-1");
    EXPECT_EQ(job->scaling_policy(), "policy-uuid");
    EXPECT_TRUE(job->run().empty());
    EXPECT_FALSE(job->created().empty());
    EXPECT_EQ(job->created(), job->updated());

    // Update: drive the lifecycle through to a terminal state.
    job->set_status(JobStatus::Running);
    EXPECT_EQ(job->status(), JobStatus::Running);
    job->set_status(JobStatus::Succeeded, "12 nodes, 4.2s");
    EXPECT_EQ(job->status(), JobStatus::Succeeded);
    EXPECT_EQ(job->detail(), "12 nodes, 4.2s");
    EXPECT_GE(job->updated(), job->created());  // bumped by the updates

    // Query: by id; jobs are keyed by id (no human name).
    auto found = node.find_job(job->id());
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id(), job->id());
    EXPECT_EQ(found->status(), JobStatus::Succeeded);
    EXPECT_EQ(found->detail(), "12 nodes, 4.2s");

    EXPECT_EQ(node.job_count(), 1u);
    EXPECT_FALSE(node.find_job("nope"));

    EXPECT_TRUE(node.drop_job(job->id()));
    EXPECT_EQ(node.job_count(), 0u);
    EXPECT_FALSE(node.drop_job(job->id()));  // already gone

    node.stop();
}

TEST(ComputeJob, CreateAndFinishEmitAuditEvents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.audit_event_count(), 0u);

    auto job = node.create_job(JobKind::Score);
    ASSERT_TRUE(job);
    // The create is audited immediately as job_created.
    ASSERT_EQ(node.audit_event_count(), 1u);
    EXPECT_EQ(node.audit_events().back()->action(), AuditAction::JobCreated);
    EXPECT_EQ(node.audit_events().back()->target(), job->id());
    EXPECT_TRUE(node.audit_events().back()->allowed());

    // Non-terminal transitions don't audit.
    job->set_status(JobStatus::Running);
    EXPECT_EQ(node.audit_event_count(), 1u);

    // The first transition into a terminal state audits job_finished once.
    job->set_status(JobStatus::Failed, "ran out of memory");
    ASSERT_EQ(node.audit_event_count(), 2u);
    auto finished = node.audit_events().back();
    EXPECT_EQ(finished->action(), AuditAction::JobFinished);
    EXPECT_EQ(finished->target(), job->id());
    EXPECT_FALSE(finished->allowed());  // Failed -> not allowed
    EXPECT_NE(finished->detail().find("ran out of memory"), std::string::npos);

    // A redundant set to another terminal state does not re-audit.
    job->set_status(JobStatus::Succeeded);
    EXPECT_EQ(node.audit_event_count(), 2u);

    node.stop();
}

TEST(ComputeJob, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto job = node.create_job(JobKind::Featurize);
        ASSERT_TRUE(job);
        id = job->id();
        job->apply_value(sample_value());
        node.stop();
    }

    // A fresh standalone node reads the job purely from disk (job.json).
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.job_count(), 1u);
    auto job = reloaded.find_job(id);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->kind(), JobKind::Train);
    EXPECT_EQ(job->status(), JobStatus::Succeeded);
    EXPECT_EQ(job->project(), "project-uuid");
    EXPECT_EQ(job->run(), "run-uuid");
    EXPECT_EQ(job->scaling_policy(), "policy-uuid");
    EXPECT_EQ(job->detail(), "trained on 12 nodes");
    reloaded.stop();
}

TEST(ComputeJob, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto job = node.create_job(JobKind::Explain, "project-uuid");
        ASSERT_TRUE(job);
        id = job->id();
        job->set_status(JobStatus::Succeeded);
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the job
    // is in the replicated document, and the projection is rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int job_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("job:", 0) == 0) {
            ++job_keys;
            EXPECT_EQ(value.value("status", std::string{}), "succeeded");
            EXPECT_EQ(value.value("kind", std::string{}), "explain");
        }
    }
    EXPECT_EQ(job_keys, 1);

    auto job = node.find_job(id);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->kind(), JobKind::Explain);
    EXPECT_EQ(job->status(), JobStatus::Succeeded);
    node.stop();
}

TEST(ComputeJob, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43340;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43341;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43340"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create + complete a job on node A only.
    auto created = a.create_job(JobKind::Train, "project-uuid", "graph-1");
    ASSERT_TRUE(created);
    created->set_status(JobStatus::Succeeded, "done");

    // Node B converges to the same job (kind + status), versions match.
    const bool converged = converge(a, b, [&] {
        auto job = b.find_job(created->id());
        return job && job->status() == JobStatus::Succeeded &&
               job->kind() == JobKind::Train &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto replicated = b.find_job(created->id());
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->project(), "project-uuid");
    EXPECT_EQ(replicated->graph(), "graph-1");
    EXPECT_EQ(replicated->detail(), "done");

    // A further update on A also replicates to B.
    created->set_status(JobStatus::Failed);
    const bool updated = converge(a, b, [&] {
        auto job = b.find_job(created->id());
        return job && job->status() == JobStatus::Failed;
    });
    EXPECT_TRUE(updated);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

TEST(ComputeJob, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    // A well-formed value passes.
    EXPECT_TRUE(zetta::validate_compute_job_value(
        zetta::to_json(sample_value()), &error))
        << error;

    // Unknown kind enum.
    EXPECT_FALSE(zetta::validate_compute_job_value(
        json{{"kind", "teleport"}, {"status", "queued"}}, &error));
    // Unknown status enum.
    EXPECT_FALSE(zetta::validate_compute_job_value(
        json{{"kind", "train"}, {"status", "exploded"}}, &error));
    // Missing the required kind field.
    EXPECT_FALSE(zetta::validate_compute_job_value(
        json{{"status", "queued"}}, &error));
    // Missing the required status field.
    EXPECT_FALSE(zetta::validate_compute_job_value(
        json{{"kind", "train"}}, &error));
    // Wrong type for a field.
    EXPECT_FALSE(zetta::validate_compute_job_value(
        json{{"kind", "train"}, {"status", "queued"}, {"project", 7}}, &error));
    // A stray top-level field (additionalProperties: false).
    EXPECT_FALSE(zetta::validate_compute_job_value(
        json{{"kind", "train"}, {"status", "queued"}, {"surprise", 1}},
        &error));

    // parse_compute_job_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::parse_compute_job_value(
        json{{"kind", "teleport"}, {"status", "queued"}}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(ComputeJob, InvalidJobJsonOnDiskIsSkipped) {
    TempDir tmp;
    // Plant a folder with a malformed job.json (bad kind) before start().
    const std::filesystem::path bad = tmp.path() / zetta::new_uuid();
    std::filesystem::create_directories(bad);
    {
        std::ofstream out(bad / "job.json");
        out << R"({"id":"x","kind":"teleport","status":"queued"})" << '\n';
    }

    Node node{config_for(tmp)};
    node.start();
    // The malformed job is skipped at the boundary, not loaded.
    EXPECT_EQ(node.job_count(), 0u);
    node.stop();
}

// ---- queryable via the system.zetta_jobs table -----------------------------

TEST(ComputeJob, QueryableViaSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // A finished train job (with a reward / scaling-policy ref) + a queued score.
    auto train = node.create_job(JobKind::Train, "project-uuid", "graph-1", {},
                                 "policy-uuid");
    ASSERT_TRUE(train);
    train->set_status(JobStatus::Running);
    train->set_status(JobStatus::Succeeded, "12 nodes, 4.2s");
    auto score = node.create_job(JobKind::Score);
    ASSERT_TRUE(score);

    QueryService svc{node};  // no cluster needed: jobs come from the node

    auto all = svc.run("SELECT * FROM system.zetta_jobs;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    ASSERT_EQ(all.rows.size(), 2u);
    // jobs() is ordered (created, then id); the train job was created first.
    const auto& first = all.rows[0];
    EXPECT_EQ(first["job_id"], train->id());
    EXPECT_EQ(first["kind"], "train");
    EXPECT_EQ(first["status"], "succeeded");
    EXPECT_EQ(first["project"], "project-uuid");
    EXPECT_EQ(first["graph"], "graph-1");
    EXPECT_EQ(first["scaling_policy"], "policy-uuid");
    EXPECT_EQ(first["detail"], "12 nodes, 4.2s");
    EXPECT_FALSE(std::string(first["created"]).empty());
    EXPECT_EQ(first["node"], node.node_id());  // the observing node

    // Projection + WHERE equality work exactly as for a regular table.
    auto running = svc.run(
        "SELECT job_id, kind FROM system.zetta_jobs WHERE status = 'queued';",
        "");
    ASSERT_TRUE(running.ok()) << running.error;
    ASSERT_EQ(running.rows.size(), 1u);
    EXPECT_EQ(running.rows[0]["job_id"], score->id());
    EXPECT_EQ(running.rows[0]["kind"], "score");

    // The bare alias is reserved for zetta_nodes only; jobs is system.<table>.
    EXPECT_FALSE(svc.run("SELECT * FROM zetta_jobs;", "").ok());
    node.stop();
}
