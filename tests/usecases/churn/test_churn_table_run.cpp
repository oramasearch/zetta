#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/score_table.hpp"
#include <chrono>
#include <thread>
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

// **The bridge #947 closes**: a dataset landed by the connector path — parquet
// partitions addressed `database.table` — feeding the pipeline that trains on it.
//
// Before this, `dataset_path = "churn_demo.accounts"` failed with `cannot open
// dataset.json` while the rows sat there, partitioned and healthy. The whole product
// claim (bring your data, get a model) stopped at that line.
//
// The shape under test is what a warehouse query actually returns: ONE flat table,
// one already-featurized row per account, engineered numeric columns and a binary
// label — not the fixture's accounts+events pair.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::CpuDevice;
using zetta::data::DatasetContract;
using zetta::pipeline::RunStatus;
using zetta::pipeline::Solution;
using zetta::pipeline::SolutionCatalog;
using zetta::usecases::churn::ChurnSolutionOptions;
using zetta::usecases::churn::ChurnSolutionResult;
using zetta::usecases::churn::ChurnTableInput;
using Json = nlohmann::json;

struct AttachedNode {
    explicit AttachedNode(std::uint16_t port)
        : node(node_config(temp_dir)), cluster("churn-table-node", cluster_config(port)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
    }
    ~AttachedNode() { node.stop(); }
    static NodeConfig node_config(const zetta::test::TempDir& dir) {
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
    zetta::test::TempDir temp_dir;
    Node node;
    Cluster cluster;
};

// A connector-shaped result set with a real, learnable signal: failed payments and
// low logins drive churn. Deliberately imperfect (one row in nine is flipped) so a
// model that scores perfectly is a bug, not a success.
std::vector<Json> warehouse_rows(std::size_t count) {
    std::vector<Json> rows;
    rows.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::int64_t failed_payments = static_cast<std::int64_t>(index % 5);
        const double logins_30d = static_cast<double>(60 - (index % 55));
        const double nps = static_cast<double>(index % 11);
        const bool at_risk = failed_payments >= 3 && logins_30d < 30.0;
        const bool flipped = index % 9 == 0;
        rows.push_back(Json{{"account_id", static_cast<std::int64_t>(index + 1)},
                            {"failed_payments", failed_payments},
                            {"logins_30d", logins_30d},
                            {"nps", nps},
                            {"churned", (at_risk != flipped) ? 1 : 0}});
    }
    return rows;
}

// Land the rows the way a connector query lands them: partitioned + placed.
std::string land_dataset(Node& node, const std::string& database,
                         const std::string& table, std::size_t rows) {
    zetta::data::AddDatasetOptions options;
    options.database = database;
    options.table_name = table;
    options.partitioned = true;
    options.members = {node.node_id()};
    const zetta::data::AddDatasetResult added =
        zetta::data::add_dataset_rows(node, warehouse_rows(rows), options);
    EXPECT_TRUE(added.ok()) << added.error;
    EXPECT_TRUE(added.table != nullptr);
    return added.table ? added.table->id() : std::string{};
}

// =====================================================================
// The acceptance: warehouse-shaped table in, trained model out.
// =====================================================================

TEST(ChurnTableRun, AConnectorLandedTableTrainsAModelOnItsOwnColumns) {
    AttachedNode host(24720);
    const std::string table_id = land_dataset(host.node, "churn_demo", "accounts", 400);
    ASSERT_FALSE(table_id.empty());

    // 1. `dataset_path = "churn_demo.accounts"` — the line that used to fail.
    const zetta::data::DatasetPath path = zetta::data::parse_dataset_path("churn_demo.accounts");
    ASSERT_EQ(path.kind, zetta::data::DatasetPathKind::Table);
    const zetta::data::ResolvedDataset resolved =
        zetta::data::resolve_dataset(host.node, path, {},
                                     zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    EXPECT_EQ(resolved.table_id, table_id);
    EXPECT_EQ(resolved.contract.entity_column, "account_id");
    EXPECT_EQ(resolved.contract.label_column, "churned");
    EXPECT_EQ(resolved.contract.feature_columns,
              (std::vector<std::string>{"failed_payments", "logins_30d", "nps"}));

    // 2. The shard this node will train on comes from its OWN partition bodies —
    // read near the data, never over the wire. Asserted here because an empty shard
    // fails much later, as "no training rows", with nothing pointing back to why.
    CpuDevice shard_device;
    const zetta::ddf::LocalShardResult shard = zetta::ddf::load_local_partitions(
        host.node, table_id, zetta::usecases::churn::table_accounts_schema(resolved.contract),
        shard_device);
    ASSERT_TRUE(shard.ok()) << shard.status.message;
    EXPECT_GT(shard.partitions_read, 0u) << "this node owns none of the table's partitions";
    EXPECT_EQ(shard.rows_read, 400u);
    EXPECT_GT(shard.bytes_read, 0u);

    // 3. Run the catalog churn blueprint over it, compute-near-data.
    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    CpuDevice device;
    ChurnSolutionOptions options;
    options.project = "churn-demo";
    options.graph = "churn-from-warehouse";
    options.model_name = "warehouse_churn_model";

    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;

    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, options);
    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Succeeded);

    // 3. A real registered Model, trained on the CUSTOMER'S columns — not the
    // fixture's mrr/seats/tenure_days.
    ASSERT_FALSE(result.model_id.empty());
    const std::shared_ptr<zetta::ml::Model> model = host.node.find_model(result.model_id);
    ASSERT_TRUE(model) << "the run's model id does not resolve on the node";

    std::vector<std::string> driver_names;
    for (const auto& [feature, weight] : result.drivers) driver_names.push_back(feature);
    std::sort(driver_names.begin(), driver_names.end());
    EXPECT_EQ(driver_names,
              (std::vector<std::string>{"failed_payments", "logins_30d", "nps"}));

    // 4. Every account was scored, risk is a probability, and the list is ranked.
    EXPECT_EQ(result.ranked.size(), 400u);
    for (std::size_t index = 1; index < result.ranked.size(); ++index) {
        EXPECT_GE(result.ranked[index - 1].risk, result.ranked[index].risk);
    }
    for (const auto& score : result.ranked) {
        EXPECT_GE(score.risk, 0.0);
        EXPECT_LE(score.risk, 1.0);
    }

    // 5. It learned the planted signal rather than memorizing noise: the dominant
    // driver is one of the two columns the label was actually built from.
    EXPECT_TRUE(result.top_driver == "failed_payments" || result.top_driver == "logins_30d")
        << "dominant driver was '" << result.top_driver << "'";
}

// A contract that cannot be met must fail HERE, with the reason — never by training
// on the wrong column and reporting success.
TEST(ChurnTableRun, AnUnresolvableContractFailsBeforeTraining) {
    AttachedNode host(24721);
    ASSERT_FALSE(land_dataset(host.node, "churn_demo", "accounts", 40).empty());

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);
    CpuDevice device;

    ChurnTableInput input;
    input.table_id = "tbl:does-not-exist";
    input.contract.entity_column = "account_id";
    input.contract.label_column = "churned";
    // No feature columns: a label with no features is not a training set.
    const ChurnSolutionResult empty_features = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input);
    EXPECT_FALSE(empty_features.ok());
    EXPECT_NE(empty_features.execution.error.find("feature columns"), std::string::npos);

    // A table that is not there fails at the shard load, naming the table.
    input.contract.feature_columns = {"failed_payments"};
    const ChurnSolutionResult missing_table = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input);
    EXPECT_FALSE(missing_table.ok());
}

// Naming a table that is not in the catalog must name the near-miss, not the
// stack it failed in.
TEST(ChurnTableRun, ResolvingAMissingTableNamesWhatTheClusterHas) {
    AttachedNode host(24722);
    ASSERT_FALSE(land_dataset(host.node, "churn_demo", "accounts", 20).empty());

    const zetta::data::ResolvedDataset wrong_database = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("analytics.accounts"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    EXPECT_FALSE(wrong_database.ok());
    EXPECT_NE(wrong_database.error.find("churn_demo.accounts"), std::string::npos);
}

// =====================================================================
// After the run: the at-risk list is answerable again (#948 steps 5 + 7).
// =====================================================================

// The ranked list a run produces dies with the run. This is the question the whole
// product exists to answer — "which accounts are most at risk?" — asked AFTER the
// run has ended, and answered by recomputing from the durable Model + table.
TEST(ChurnTableRun, TheAtRiskListSurvivesTheRunThatProducedIt) {
    AttachedNode host(24723);
    const std::string table_id = land_dataset(host.node, "churn_demo", "accounts", 300);
    ASSERT_FALSE(table_id.empty());

    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("churn_demo.accounts"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);
    CpuDevice device;
    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;
    ChurnSolutionOptions options;
    options.project = "churn-demo";
    const ChurnSolutionResult run = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, options);
    ASSERT_TRUE(run.ok()) << run.execution.error;
    ASSERT_FALSE(run.model_id.empty());

    // The run is over; everything it held in memory is gone. Ask again from the
    // durable Model + the table.
    const std::shared_ptr<zetta::ml::Model> model = host.node.find_model(run.model_id);
    ASSERT_TRUE(model);
    CpuDevice score_device;
    const zetta::ml::TableScoreResult scored = zetta::ml::score_table(
        host.node, *model, resolved.table_id, resolved.contract, score_device);
    ASSERT_TRUE(scored.ok()) << scored.error;

    EXPECT_EQ(scored.rows_scored, 300u);
    EXPECT_GT(scored.partitions_read, 0u);
    EXPECT_EQ(scored.feature_columns,
              (std::vector<std::string>{"failed_payments", "logins_30d", "nps"}));
    EXPECT_FALSE(scored.drivers.empty());

    // The same accounts, the same risks, the same order as the run reported — a
    // recomputed answer that disagreed with the run would be worse than none.
    ASSERT_EQ(scored.ranked.size(), run.ranked.size());
    for (std::size_t index = 0; index < scored.ranked.size(); ++index) {
        EXPECT_EQ(scored.ranked[index].entity, run.ranked[index].entity) << index;
        EXPECT_NEAR(scored.ranked[index].risk, run.ranked[index].risk, 1e-9) << index;
    }

    // top_k caps the list without changing what is at the top.
    const zetta::ml::TableScoreResult top = zetta::ml::score_table(
        host.node, *model, resolved.table_id, resolved.contract, score_device, 10);
    ASSERT_TRUE(top.ok()) << top.error;
    ASSERT_EQ(top.ranked.size(), 10u);
    EXPECT_EQ(top.ranked.front().entity, scored.ranked.front().entity);
}

// "Take an account not in the training data and ask what it would score."
TEST(ChurnTableRun, AnAccountThatIsNotInTheDataCanStillBeScored) {
    AttachedNode host(24724);
    const std::string table_id = land_dataset(host.node, "churn_demo", "accounts", 200);
    ASSERT_FALSE(table_id.empty());
    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("churn_demo.accounts"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);
    CpuDevice device;
    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;
    const ChurnSolutionResult run = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, {});
    ASSERT_TRUE(run.ok()) << run.execution.error;
    const std::shared_ptr<zetta::ml::Model> model = host.node.find_model(run.model_id);
    ASSERT_TRUE(model);

    // An account nobody has seen: many failed payments, barely logging in.
    const zetta::ml::TableScoreResult risky = zetta::ml::score_rows(
        *model, {Json{{"failed_payments", 4}, {"logins_30d", 6.0}, {"nps", 1.0}}},
        {9001});
    ASSERT_TRUE(risky.ok()) << risky.error;
    ASSERT_EQ(risky.ranked.size(), 1u);
    EXPECT_EQ(risky.ranked.front().entity, 9001);
    EXPECT_GE(risky.ranked.front().risk, 0.0);
    EXPECT_LE(risky.ranked.front().risk, 1.0);

    // A healthy account scores lower than the struggling one — the model is being
    // asked a question, not returning a constant.
    const zetta::ml::TableScoreResult healthy = zetta::ml::score_rows(
        *model, {Json{{"failed_payments", 0}, {"logins_30d", 58.0}, {"nps", 10.0}}},
        {9002});
    ASSERT_TRUE(healthy.ok()) << healthy.error;
    EXPECT_LT(healthy.ranked.front().risk, risky.ranked.front().risk);

    // A row missing a feature the model needs FAILS. Zero-filling it would score
    // the account as if it had never logged in, which is confidently wrong rather
    // than visibly broken.
    const zetta::ml::TableScoreResult incomplete = zetta::ml::score_rows(
        *model, {Json{{"failed_payments", 4}, {"nps", 1.0}}});
    EXPECT_FALSE(incomplete.ok());
    EXPECT_NE(incomplete.error.find("logins_30d"), std::string::npos);
}

// The shape that deadlocked the node: the pipeline driven on a WORKER thread.
//
// AsyncChurnRun runs the graph off the owner loop, and the table path reads the
// partition catalog and blob store off the Node tree — which is owner-thread only. The
// direct-call tests above never see it, because a test calls the runner from the thread
// that owns the node. The deployed API path hangs every time. This pins the gate.
TEST(ChurnTableRun, TheTablePathRunsFromAWorkerThreadWithoutDeadlocking) {
    AttachedNode host(24725);
    const std::string table_id = land_dataset(host.node, "churn_demo", "accounts", 200);
    ASSERT_FALSE(table_id.empty());
    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("churn_demo.accounts"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;
    ChurnSolutionOptions options;
    options.project = "churn-demo";
    options.coordinate = false;

    const std::shared_ptr<zetta::usecases::churn::AsyncChurnRun> handle =
        zetta::usecases::churn::AsyncChurnRun::start(host.node, solution->graph,
                                                     std::move(input), options);
    ASSERT_TRUE(handle);

    // Pump the owner gate, exactly as the node's tick loop does. Without the gate on
    // the shard load this never completes: the worker blocks on the tree while the
    // owner blocks here.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!handle->done() && std::chrono::steady_clock::now() < deadline) {
        host.node.drain_owner_work();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(handle->done()) << "the run never finished — the owner gate deadlocked";
    EXPECT_TRUE(handle->result().ok()) << handle->result().execution.error;
    EXPECT_EQ(handle->result().ranked.size(), 200u);
}

// --- the pure spec construction ----------------------------------------------

TEST(ChurnTableRun, TheFeaturizeSpecIsStaticCarryThrough) {
    DatasetContract contract;
    contract.entity_column = "account_id";
    contract.label_column = "churned";
    contract.feature_columns = {"failed_payments", "logins_30d"};

    const auto spec = zetta::usecases::churn::table_featurize_spec(contract);
    EXPECT_EQ(spec.entity_column, "entity");
    EXPECT_EQ(spec.label_column, "churned");
    EXPECT_EQ(spec.account_features, contract.feature_columns);
    // A connector query returns rows, not events — inventing windowed aggregates
    // over them would be arithmetic on a shape that does not exist.
    EXPECT_TRUE(spec.event_features.empty());
    EXPECT_TRUE(spec.time_column.empty());
}

TEST(ChurnTableRun, TheAccountsSchemaTypesTheEntityAsAnIntegerKey) {
    DatasetContract contract;
    contract.entity_column = "account_id";
    contract.label_column = "churned";
    contract.feature_columns = {"mrr"};
    contract.time_column = "observed_day";
    contract.as_of = 1000.0;

    const auto schema = zetta::usecases::churn::table_accounts_schema(contract);
    ASSERT_EQ(schema.columns.size(), 4u);
    EXPECT_EQ(schema.columns[0].name, "entity");
    EXPECT_EQ(schema.columns[0].source_field(), "account_id");
    EXPECT_EQ(schema.columns[0].dtype, zetta::compute::DataType::Int64);
    EXPECT_EQ(schema.columns[1].name, "mrr");
    EXPECT_EQ(schema.columns[1].dtype, zetta::compute::DataType::Float64);
    EXPECT_EQ(schema.columns[2].name, "churned");
    EXPECT_EQ(schema.columns[2].dtype, zetta::compute::DataType::Int64);
    EXPECT_EQ(schema.columns[3].name, "observed_day");
    // Everything but the entity reads the customer's own spelling straight through;
    // the entity is renamed to the canonical `entity` so one concept has one name.
    for (std::size_t index = 1; index < schema.columns.size(); ++index) {
        EXPECT_EQ(schema.columns[index].source_field(), schema.columns[index].name);
    }
}

}  // namespace
