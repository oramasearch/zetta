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
#include "zetta/churn_dataset.hpp"
#include "zetta/churn_step_executor.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/executor.hpp"
#include "zetta/intent_compiler.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/run.hpp"
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

using zetta::ChurnPipelineConfig;
using zetta::ChurnStepExecutor;
using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::ExecutionResult;
using zetta::GraphExecutor;
using zetta::JobKind;
using zetta::JobStatus;
using zetta::Model;
using zetta::Node;
using zetta::NodeConfig;
using zetta::RetentionAction;
using zetta::RunOutput;
using zetta::RunStatus;
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
// (the same encoding the GBDT / TreeSHAP suites use — string ids / ISO dates /
// categorical event types into the numeric columns DDF operates on).

std::int64_t day_of_year_2026(const std::string& iso) {
    const int month = std::stoi(iso.substr(5, 2));
    const int day = std::stoi(iso.substr(8, 2));
    static const int before[] = {0,   0,   31,  59,  90,  120, 151,
                                 181, 212, 243, 273, 304, 334};
    return before[month] + day;
}
std::int64_t event_type_code(const std::string& type) {
    if (type == "login") return 1;
    if (type == "feature_use") return 2;
    if (type == "payment_failed") return 3;
    if (type == "support_ticket") return 4;
    if (type == "csm_contact") return 5;
    if (type == "comms") return 6;
    return 0;
}

struct ChurnNumeric {
    std::vector<Json> events;
    std::vector<Json> accounts;
    std::int64_t as_of = 0;
};

ChurnNumeric encode_churn(const zetta::ChurnDataset& dataset) {
    ChurnNumeric out;
    std::map<std::string, std::int64_t> entity_index;
    for (const zetta::ChurnAccount& account : dataset.accounts) {
        const std::int64_t key = static_cast<std::int64_t>(entity_index.size());
        entity_index.emplace(account.account_id, key);
        out.accounts.push_back({{"entity", key},
                                {"mrr", account.mrr},
                                {"seats", account.seats},
                                {"tenure_days", account.tenure_days},
                                {"churned", account.churned ? 1 : 0}});
    }
    for (const zetta::ChurnEvent& event : dataset.events) {
        out.events.push_back({{"entity", entity_index.at(event.account_id)},
                              {"ts", day_of_year_2026(event.event_ts)},
                              {"value", event.value},
                              {"type", event_type_code(event.event_type)}});
    }
    out.as_of = day_of_year_2026(dataset.as_of_date);
    return out;
}

DataFrameSchema events_schema() {
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                      ColumnSchema{.name = "ts", .dtype = DataType::Int64},
                      ColumnSchema{.name = "value", .dtype = DataType::Float64},
                      ColumnSchema{.name = "type", .dtype = DataType::Int64}};
    return schema;
}
DataFrameSchema accounts_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ColumnSchema{.name = "mrr", .dtype = DataType::Float64},
        ColumnSchema{.name = "seats", .dtype = DataType::Int64},
        ColumnSchema{.name = "tenure_days", .dtype = DataType::Int64},
        ColumnSchema{.name = "churned", .dtype = DataType::Int64}};
    return schema;
}

FeaturizeSpec churn_spec(std::int64_t as_of) {
    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = static_cast<double>(as_of);
    spec.event_features = {
        WindowedFeature{.name = "failed_payments",
                        .agg = zetta::ddf::AggregateOp::Count,
                        .type_column = "type",
                        .type_value = 3.0},
        WindowedFeature{.name = "support_severity",
                        .agg = zetta::ddf::AggregateOp::Sum,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 4.0},
        WindowedFeature{.name = "total_events",
                        .agg = zetta::ddf::AggregateOp::Count},
    };
    spec.account_features = {"mrr", "seats", "tenure_days"};
    spec.label_column = "churned";
    return spec;
}

GbdtParams churn_params() {
    GbdtParams params;
    params.num_trees = 30;
    params.max_depth = 3;
    params.max_bins = 16;
    params.learning_rate = 0.3;
    params.min_child_hessian = 1e-3;
    return params;
}

zetta::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::JsonStore store;
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
    const auto compiled = zetta::compile_intent("reduce churn and improve retention");
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->shape, zetta::IntentShape::Churn);
    const zetta::ir::ExecutionGraph& graph = compiled->graph;

    // This node's numeric input shards (single-node => the whole fixture).
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    const ChurnNumeric numeric = encode_churn(loaded.dataset);
    const zetta::JsonStore events = make_store(numeric.events);
    const zetta::JsonStore accounts = make_store(numeric.accounts);

    ComputeNode host(24400);
    CpuDevice device;

    ChurnPipelineConfig config;
    config.events_store = &events;
    config.events_schema = events_schema();
    config.accounts_store = &accounts;
    config.accounts_schema = accounts_schema();
    config.featurize = churn_spec(numeric.as_of);
    config.params = churn_params();
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
    EXPECT_TRUE(zetta::validate_run_value(result.run->metadata(), &schema_error))
        << schema_error;

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

    // (2) An `index` output — the ranked at-risk list (highest risk first).
    const auto index_output =
        std::find_if(outputs.begin(), outputs.end(),
                     [](const RunOutput& o) { return o.kind == "index"; });
    ASSERT_NE(index_output, outputs.end()) << "run has no index output";
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
    const auto compiled = zetta::compile_intent("predict churn");
    ASSERT_TRUE(compiled.has_value());

    // Empty shards: connect loads empty frames, featurize/train then fail.
    const zetta::JsonStore events = make_store({});
    const zetta::JsonStore accounts = make_store({});

    ComputeNode host(24410);
    CpuDevice device;

    ChurnPipelineConfig config;
    config.events_store = &events;
    config.events_schema = events_schema();
    config.accounts_store = &accounts;
    config.accounts_schema = accounts_schema();
    config.featurize = churn_spec(180);
    config.params = churn_params();

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
