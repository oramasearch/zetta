#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::pipeline::Run;
using zetta::pipeline::RunLineage;
using zetta::pipeline::RunLogEntry;
using zetta::pipeline::RunOutput;
using zetta::pipeline::RunStatus;
using zetta::pipeline::RunValue;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A sample run value with every part populated (status / logs / outputs /
// lineage), used to assert it round-trips through disk + the CRDT.
RunValue sample_value() {
    RunValue value;
    value.status = RunStatus::Succeeded;
    value.outputs = {{"dataset", "churn_features", "tbl:abc"},
                     {"index", "churn_index", "idx:def"}};
    value.lineage.sources = {"events.jsonl", "crm.csv"};
    value.lineage.steps = {"connect", "clean", "model", "index"};
    value.lineage.freshness = "2026-06-25T00:00:00.000Z";
    value.lineage.confidence = 0.87;
    return value;
}

// Tick both nodes until `done` holds (or we give up). Mirrors the project /
// data replication tests: capture -> cluster gossip -> peer merge -> reconcile.
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

TEST(Run, CreateUpdateQuery) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // Create: starts Pending with create == update, holds the references.
    auto run = node.create_run("project-uuid", "graph-1");
    ASSERT_TRUE(run);
    EXPECT_EQ(run->project(), "project-uuid");
    EXPECT_EQ(run->graph(), "graph-1");
    EXPECT_EQ(run->status(), RunStatus::Pending);
    EXPECT_FALSE(run->created().empty());
    EXPECT_EQ(run->created(), run->updated());
    EXPECT_TRUE(run->logs().empty());
    EXPECT_TRUE(run->outputs().empty());

    // Update: status, a log line, outputs, and lineage.
    run->set_status(RunStatus::Running);
    run->add_log({"2026-06-25T14:00:00.000Z", "info", "run started"});
    run->set_status(RunStatus::Succeeded);
    run->add_log({"2026-06-25T14:05:00.000Z", "info", "run succeeded"});
    run->set_outputs({{"model", "churn_model", "lo:xyz"}});
    RunLineage lineage;
    lineage.sources = {"events.jsonl"};
    lineage.steps = {"connect", "model"};
    lineage.confidence = 0.5;
    run->set_lineage(lineage);

    EXPECT_GE(run->updated(), run->created());  // bumped by the updates

    // Query: by id; runs are keyed by id (no human name).
    auto found = node.find_run(run->id());
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id(), run->id());
    EXPECT_EQ(found->status(), RunStatus::Succeeded);
    ASSERT_EQ(found->logs().size(), 2u);
    EXPECT_EQ(found->logs()[1].message, "run succeeded");
    ASSERT_EQ(found->outputs().size(), 1u);
    EXPECT_EQ(found->outputs()[0].kind, "model");
    EXPECT_EQ(found->lineage().steps.size(), 2u);
    EXPECT_DOUBLE_EQ(found->lineage().confidence, 0.5);

    EXPECT_EQ(node.run_count(), 1u);
    EXPECT_FALSE(node.find_run("nope"));

    EXPECT_TRUE(node.drop_run(run->id()));
    EXPECT_EQ(node.run_count(), 0u);
    EXPECT_FALSE(node.drop_run(run->id()));  // already gone

    node.stop();
}

TEST(Run, ConfidenceIsClampedTo01) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto run = node.create_run();
    RunLineage hot;
    hot.confidence = 1.5;  // out of range
    run->set_lineage(hot);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 1.0);

    RunLineage cold;
    cold.confidence = -0.3;
    run->set_lineage(cold);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 0.0);

    node.stop();
}

TEST(RunFeaturePlan, FeatureReasonsSurviveRestartAndRejectMalformedUsage) {
    TempDir directory;
    std::string run_id;
    const nlohmann::json usage = {
        {"used", {{{"name", "spend"}, {"source", {"spend"}},
                    {"transform", "numeric carry-through"}, {"why", "pre-outcome input"}}}},
        {"excluded", {{{"name", "label"}, {"why", "target only"}},
                       {{"name", "entity"}, {"why", "row key only"}}}}};
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        const auto run = node.create_run();
        ASSERT_TRUE(run);
        run_id = run->id();
        std::string error;
        ASSERT_TRUE(run->set_features(usage, &error)) << error;
        auto malformed = usage;
        malformed["used"][0].erase("why");
        EXPECT_FALSE(run->set_features(malformed, &error));
        EXPECT_EQ(run->value().features, usage);
        node.stop();
    }
    Node restored{config_for(directory)};
    ASSERT_TRUE(restored.start());
    ASSERT_TRUE(restored.find_run(run_id));
    EXPECT_EQ(restored.find_run(run_id)->value().features, usage);
    restored.stop();
}

TEST(Run, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto run = node.create_run("project-uuid", "graph-1");
        ASSERT_TRUE(run);
        id = run->id();
        run->apply_value([] {
            RunValue value = sample_value();
            value.project = "project-uuid";
            value.graph = "graph-1";
            return value;
        }());
        node.stop();
    }

    // A fresh standalone node reads the run purely from disk (run.json).
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.run_count(), 1u);
    auto run = reloaded.find_run(id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), RunStatus::Succeeded);
    EXPECT_EQ(run->project(), "project-uuid");
    ASSERT_EQ(run->outputs().size(), 2u);
    EXPECT_EQ(run->outputs()[0].name, "churn_features");
    EXPECT_EQ(run->lineage().sources.size(), 2u);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 0.87);
    reloaded.stop();
}

TEST(Run, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto run = node.create_run("project-uuid", "graph-1");
        ASSERT_TRUE(run);
        id = run->id();
        run->set_status(RunStatus::Succeeded);
        run->set_outputs({{"index", "churn_index", "idx:def"}});
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the run
    // is in the replicated document, and the projection is rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int run_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("run:", 0) == 0) {
            ++run_keys;
            EXPECT_EQ(value.value("status", std::string{}), "succeeded");
        }
    }
    EXPECT_EQ(run_keys, 1);

    auto run = node.find_run(id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), RunStatus::Succeeded);
    ASSERT_EQ(run->outputs().size(), 1u);
    EXPECT_EQ(run->outputs()[0].kind, "index");
    node.stop();
}

TEST(Run, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43320;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43321;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43320"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create + complete a run on node A only.
    auto created = a.create_run("project-uuid", "graph-1");
    ASSERT_TRUE(created);
    created->set_status(RunStatus::Succeeded);
    created->set_outputs({{"dataset", "churn_features", "tbl:abc"}});

    // Node B converges to the same run (status + outputs), versions match.
    const bool converged = converge(a, b, [&] {
        auto run = b.find_run(created->id());
        return run && run->status() == RunStatus::Succeeded &&
               run->outputs().size() == 1u &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto replicated = b.find_run(created->id());
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->project(), "project-uuid");
    EXPECT_EQ(replicated->outputs()[0].name, "churn_features");

    // A further update on A also replicates to B.
    created->set_status(RunStatus::Failed);
    const bool updated = converge(a, b, [&] {
        auto run = b.find_run(created->id());
        return run && run->status() == RunStatus::Failed;
    });
    EXPECT_TRUE(updated);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

TEST(Run, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    // A well-formed value passes.
    EXPECT_TRUE(zetta::pipeline::validate_run_value(zetta::pipeline::to_json(sample_value()), &error))
        << error;

    // Unknown status enum.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "exploded"}}, &error));
    // Missing the required status field.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(json{{"project", "p"}}, &error));
    // Wrong type for a field.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"}, {"logs", "not-an-array"}}, &error));
    // Confidence out of range.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"}, {"lineage", {{"confidence", 2.0}}}},
        &error));
    // An unknown output kind.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"},
             {"outputs", json::array({{{"kind", "spreadsheet"}}})}},
        &error));
    // A stray top-level field (additionalProperties: false).
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"}, {"surprise", 1}}, &error));

    // parse_run_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::pipeline::parse_run_value(json{{"status", "exploded"}}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(Run, InvalidRunJsonOnDiskIsSkipped) {
    TempDir tmp;
    // Plant a folder with a malformed run.json (bad status) before start().
    const std::filesystem::path bad = tmp.path() / zetta::util::new_uuid();
    std::filesystem::create_directories(bad);
    {
        std::ofstream out(bad / "run.json");
        out << R"({"id":"x","status":"exploded"})" << '\n';
    }

    Node node{config_for(tmp)};
    node.start();
    // The malformed run is skipped at the boundary, not loaded.
    EXPECT_EQ(node.run_count(), 0u);
    node.stop();
}
