#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/usecases/churn/churn_step_executor.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/storage/json_store.hpp"

// The real churn StepExecutor (#396): run_graph over the churn graph, behind the
// existing StepExecutor seam, produces a Run whose outputs reference a REAL Model +
// ranked at-risk list + drivers — connect->load-to-DDF, clean->featurize,
// model->train+calibrate, index->score/rank, serve->explain (TreeSHAP),
// act->retention playbook, each heavy step a recorded ComputeJob. Single-node here
// (the underlying jobs' single-node == multi-node equivalence is proven in the
// per-op test suites).
namespace {

namespace fs = std::filesystem;

using zetta::usecases::churn::ChurnPipelineConfig;
using zetta::usecases::churn::ChurnStepExecutor;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::pipeline::ExecutionResult;
using zetta::pipeline::GraphExecutor;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::ml::Model;
using zetta::Node;
using zetta::NodeConfig;
using zetta::usecases::churn::RetentionAction;
using zetta::pipeline::RunOutput;
using zetta::pipeline::RunStatus;
using zetta::compute::CpuDevice;
using zetta::compute::JobCoordinator;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::FeaturizeSpec;
using zetta::ddf::WindowedFeature;
using zetta::ml::EntityScore;
using zetta::ml::GbdtParams;
using zetta::test::TempDir;
using DataType = zetta::compute::DataType;
using Json = nlohmann::json;

// --- The churn fixture, encoded to numeric DDF input shards ------------------
// The encoding + the standard featurize spec / GBDT params are the canonical
// zetta::churn_solution.hpp surface (encode_churn_dataset, churn_events_schema /
// churn_accounts_schema, churn_featurize_spec, default_churn_gbdt_params) — the
// same the GBDT / TreeSHAP suites share.

zetta::storage::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::storage::JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

// A single-node loopback compute node: a Node + a one-node Cluster + a started
// JobCoordinator over it (the harness the per-op job suites use).
struct ComputeNode {
    explicit ComputeNode(std::uint16_t port)
        : node(node_config(temp_dir)),
          cluster("churn-node", cluster_config(port)),
          coordinator(cluster, /*worker_count=*/2) {
        node.start();
        cluster.start();
        coordinator.start();
    }
    ~ComputeNode() {
        coordinator.stop();
    }
    static NodeConfig node_config(const TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }
    static ClusterConfig cluster_config(std::uint16_t port) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        return config;
    }
    TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

// =====================================================================
// Acceptance: run_graph over the churn graph -> Model + ranked list + drivers
// =====================================================================

TEST(ChurnStepExecutor, RunGraphProducesRealModelRankedListAndDrivers) {
    // The churn ExecutionGraph: the intent compiler's churn skeleton
    // (connect x2 -> clean -> model -> index -> serve -> act).
    const auto compiled = zetta::pipeline::compile_intent("reduce churn and improve retention");
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->shape, zetta::pipeline::IntentShape::Churn);
    const zetta::ir::ExecutionGraph& graph = compiled->graph;

    // This node's numeric input shards (single-node => the whole fixture).
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::usecases::churn::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    const zetta::usecases::churn::ChurnNumericShards numeric =
        zetta::usecases::churn::encode_churn_dataset(loaded.dataset);
    const zetta::storage::JsonStore events = make_store(numeric.events);
    const zetta::storage::JsonStore accounts = make_store(numeric.accounts);

    ComputeNode host(24400);
    CpuDevice device;

    ChurnPipelineConfig config;
    config.events_store = &events;
    config.events_schema = zetta::usecases::churn::churn_events_schema();
    config.accounts_store = &accounts;
    config.accounts_schema = zetta::usecases::churn::churn_accounts_schema();
    config.featurize = zetta::usecases::churn::churn_featurize_spec(numeric.as_of);
    config.params = zetta::usecases::churn::default_churn_gbdt_params();
    config.entity_column = "entity";
    config.model_name = "churn_model";

    auto churn = std::make_shared<ChurnStepExecutor>(host.node, host.coordinator,
                                                     device, config);

    GraphExecutor executor{churn};
    const ExecutionResult result =
        executor.execute(host.node, graph, "churn-project", "churn-graph-1");

    // The run succeeded end to end.
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.run);
    EXPECT_EQ(result.run->status(), RunStatus::Succeeded);
    EXPECT_TRUE(result.failed_step.empty());

    // The produced run is schema-valid (it only emits closed output kinds).
    std::string schema_error;
    EXPECT_TRUE(zetta::pipeline::validate_run_value(result.run->metadata(), &schema_error))
        << schema_error;
    const nlohmann::json run_metrics =
        result.run->get_metrics({"train.log_loss"});
    ASSERT_EQ(run_metrics["series"].size(), 1u);
    EXPECT_EQ(run_metrics["series"][0]["points"].size(),
              config.params.num_trees);
    EXPECT_EQ(run_metrics["params"]["trainer"], "gbdt");
    EXPECT_TRUE(run_metrics["summary"].contains("train.log_loss"));
    const zetta::pubsub::Address metric_topic{
        "topic", "run-metrics." + result.run->id()};
    const auto metric_topic_settings =
        host.node.pubsub()->topic_settings(metric_topic);
    ASSERT_TRUE(metric_topic_settings);
    EXPECT_TRUE(metric_topic_settings->exposed);

    // (1) A `model` output that references a REAL registered Model.
    const std::vector<RunOutput>& outputs = result.run->outputs();
    const auto model_output =
        std::find_if(outputs.begin(), outputs.end(),
                     [](const RunOutput& o) { return o.kind == "model"; });
    ASSERT_NE(model_output, outputs.end()) << "run has no model output";
    EXPECT_FALSE(churn->model_id().empty());
    EXPECT_EQ(model_output->ref, churn->model_id());
    std::shared_ptr<Model> registered = host.node.find_model(model_output->ref);
    ASSERT_TRUE(registered) << "model output ref does not resolve to a real Model";
    EXPECT_EQ(registered->kind(), "classifier");

    // (2) An `index` output referencing a REAL, queryable Index the run built over
    // the accounts shard (#528), plus the ranked at-risk list (highest risk first).
    const auto index_output =
        std::find_if(outputs.begin(), outputs.end(),
                     [](const RunOutput& o) { return o.kind == "index"; });
    ASSERT_NE(index_output, outputs.end()) << "run has no index output";
    std::shared_ptr<zetta::index::Index> built_index =
        host.node.find_index(index_output->ref);
    ASSERT_TRUE(built_index) << "index output ref does not resolve to a real Index";
    EXPECT_EQ(built_index->kind(), zetta::index::IndexKind::Search);
    // The built index resolves an entity id back to its account row.
    auto index_backend = host.node.index_registry().resolve(built_index->spec());
    ASSERT_TRUE(index_backend);
    const std::vector<EntityScore>& ranked = churn->ranked();
    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(ranked.size(), loaded.dataset.accounts.size());
    for (std::size_t i = 1; i < ranked.size(); ++i) {
        EXPECT_GE(ranked[i - 1].risk, ranked[i].risk) << "rank " << i;
    }

    // (3) Drivers — the SHAP explanation, surfaced as a dataset output. The
    // dominant global churn driver of this fixture is failed payments.
    const auto drivers_output =
        std::find_if(outputs.begin(), outputs.end(), [](const RunOutput& o) {
            return o.kind == "dataset" && o.ref.rfind("drivers:", 0) == 0;
        });
    ASSERT_NE(drivers_output, outputs.end()) << "run has no drivers output";
    EXPECT_EQ(churn->top_driver(), "failed_payments");
    const auto ranking = churn->driver_ranking();
    ASSERT_FALSE(ranking.empty());
    EXPECT_EQ(ranking.front().first, "failed_payments");

    // (4) The retention playbook joins the at-risk list with each entity's driver.
    const std::vector<RetentionAction>& playbook = churn->playbook();
    EXPECT_EQ(playbook.size(), ranked.size());
    ASSERT_FALSE(playbook.empty());
    EXPECT_EQ(playbook.front().entity, ranked.front().entity);
    bool any_driver_named = std::any_of(
        playbook.begin(), playbook.end(),
        [](const RetentionAction& a) { return !a.dominant_driver.empty(); });
    EXPECT_TRUE(any_driver_named);

    // Each heavy step ran as a recorded ComputeJob (featurize/train/score/explain),
    // all Succeeded and visible in the registry.
    const auto jobs = host.node.jobs();
    int featurize = 0, train = 0, score = 0, explain = 0;
    for (const auto& job : jobs) {
        EXPECT_EQ(job->status(), JobStatus::Succeeded) << job->id();
        switch (job->kind()) {
            case JobKind::Featurize: ++featurize; break;
            case JobKind::Train:     ++train; break;
            case JobKind::Score:     ++score; break;
            case JobKind::Explain:   ++explain; break;
            case JobKind::Ddf:       break;
            case JobKind::Ingest:    break;  // no ingest step in this graph
        }
    }
    EXPECT_EQ(featurize, 1);
    EXPECT_EQ(train, 1);
    EXPECT_EQ(score, 1);
    EXPECT_EQ(explain, 1);
}

// A failing graph stops at the first heavy step that can't run, and the run is
// Failed (fail-fast) — here the model step has no features because the configured
// shards are empty, so featurization yields nothing trainable.
TEST(ChurnStepExecutor, FailingComputeFailsTheRun) {
    const auto compiled = zetta::pipeline::compile_intent("predict churn");
    ASSERT_TRUE(compiled.has_value());

    // Empty shards: connect loads empty frames, featurize/train then fail.
    const zetta::storage::JsonStore events = make_store({});
    const zetta::storage::JsonStore accounts = make_store({});

    ComputeNode host(24410);
    CpuDevice device;

    ChurnPipelineConfig config;
    config.events_store = &events;
    config.events_schema = zetta::usecases::churn::churn_events_schema();
    config.accounts_store = &accounts;
    config.accounts_schema = zetta::usecases::churn::churn_accounts_schema();
    config.featurize = zetta::usecases::churn::churn_featurize_spec(180);
    config.params = zetta::usecases::churn::default_churn_gbdt_params();

    auto churn = std::make_shared<ChurnStepExecutor>(host.node, host.coordinator,
                                                     device, config);
    GraphExecutor executor{churn};
    const ExecutionResult result = executor.execute(host.node, compiled->graph);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.run);
    EXPECT_EQ(result.run->status(), RunStatus::Failed);
    EXPECT_FALSE(result.failed_step.empty());
    EXPECT_TRUE(churn->model_id().empty());
}

}  // namespace
