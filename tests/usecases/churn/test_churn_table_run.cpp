#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/explain_row.hpp"
#include "zetta/ml/score_table.hpp"
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

std::vector<Json> realistic_warehouse_rows(std::size_t count) {
    std::vector<Json> rows;
    rows.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::int64_t failed_payments = static_cast<std::int64_t>(index % 5);
        const double logins_30d = static_cast<double>(60 - (index % 55));
        const bool churned = failed_payments >= 3 && logins_30d < 30.0;
        std::string customer_id = "c" + std::to_string(100000 + index);
        customer_id.erase(1, 1);
        rows.push_back({{"customer_id", std::move(customer_id)},
                        {"observed_at", "2026-09-01T00:00:00Z"},
                        {"failed_payments", failed_payments},
                        {"logins_30d", logins_30d},
                        {"active", index % 2 == 0},
                        {"churned", churned}});
    }
    return rows;
}

bool is_business_id(const std::string& value) {
    return value.size() == 6 && value.front() == 'c' &&
           std::all_of(value.begin() + 1, value.end(),
                       [](const char character) {
                           return character >= '0' && character <= '9';
                       });
}

std::string land_rows(Node& node, const std::string& database,
                      const std::string& table, std::vector<Json> rows) {
    zetta::data::AddDatasetOptions options;
    options.database = database;
    options.table_name = table;
    options.partitioned = true;
    options.members = {node.node_id()};
    const zetta::data::AddDatasetResult added =
        zetta::data::add_dataset_rows(node, rows, options);
    EXPECT_TRUE(added.ok()) << added.error;
    return added.table ? added.table->id() : std::string{};
}

// One account's value for one column, straight from the generator above. Used to
// check an explanation against the DATA rather than against another engine read,
// which would agree with a wrong answer as readily as a right one.
std::optional<double> warehouse_value(std::int64_t entity, const std::string& column) {
    const std::vector<Json> rows = warehouse_rows(static_cast<std::size_t>(entity));
    if (rows.empty()) return std::nullopt;
    const Json& row = rows.back();  // account_id == index + 1, so the last row is it
    const auto found = row.find(column);
    if (found == row.end() || !found->is_number()) return std::nullopt;
    return found->get<double>();
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
    input.dataset_name = "churn_demo.accounts";
    input.notes = resolved.notes;

    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, options);
    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Succeeded);

    // The run says what it read (lake's UX review 2026-09-27): its lineage names the
    // dataset, never the blueprint's own warehouse sources, and repeated connect
    // steps say that they reuse the source already loaded by this run.
    EXPECT_EQ(result.execution.run->lineage().sources,
              (std::vector<std::string>{"dataset:churn_demo.accounts"}));
    bool trained_on = false;
    bool reused = false;
    for (const auto& log : result.execution.run->logs()) {
        if (log.message == "dataset: trained on churn_demo.accounts") trained_on = true;
        if (log.message.find("reuses dataset churn_demo.accounts") !=
            std::string::npos) {
            reused = true;
        }
        EXPECT_EQ(log.message.find("warehouse"), std::string::npos) << log.message;
    }
    EXPECT_TRUE(trained_on) << "the run logs the dataset it trained on";
    EXPECT_TRUE(reused) << "the run logs that its second connect reuses the dataset";

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

TEST(ChurnTableRun, TextBusinessIdsAndBooleanLabelsReachEveryDurableOutput) {
    AttachedNode host(24729);
    const std::string table_id = land_rows(
        host.node, "churn_demo", "real_accounts", realistic_warehouse_rows(200));
    ASSERT_FALSE(table_id.empty());

    DatasetContract overrides;
    overrides.time_column = "observed_at";
    overrides.as_of = 1790812800.0;
    const zetta::data::ResolvedDataset resolved =
        zetta::data::resolve_dataset(
            host.node,
            zetta::data::parse_dataset_path("churn_demo.real_accounts"),
            overrides,
            zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    ASSERT_TRUE(resolved.contract.entity_keys);
    EXPECT_EQ(resolved.contract.label_column, "churned");
    EXPECT_EQ(resolved.contract.feature_columns,
              (std::vector<std::string>{"active", "failed_payments", "logins_30d"}));

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);
    CpuDevice device;
    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;
    input.dataset_name = "churn_demo.real_accounts";
    input.notes = resolved.notes;
    ChurnSolutionOptions options;
    options.project = "string-id-churn";
    options.graph = "string-id-churn";
    const ChurnSolutionResult result =
        zetta::usecases::churn::run_churn_solution_on_table(
            host.node, device, solution->graph, input, options);
    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);
    EXPECT_FALSE(result.execution.run->entity_key_mapping().empty());

    const std::shared_ptr<zetta::ml::Model> model =
        host.node.find_model(result.model_id);
    ASSERT_TRUE(model);
    EXPECT_TRUE(model->feature_schema().contains("entity_key_mapping"));

    std::shared_ptr<zetta::data::Table> ranked_table;
    for (const auto& output : result.execution.run->outputs()) {
        if (output.kind != "dataset" ||
            !output.ref.starts_with("churn_results.")) {
            continue;
        }
        const std::shared_ptr<zetta::data::Database> database =
            host.node.find_database("churn_results");
        ranked_table = database
                           ? database->find_table(
                                 output.ref.substr(output.ref.find('.') + 1))
                           : nullptr;
    }
    ASSERT_TRUE(ranked_table);
    ASSERT_GT(ranked_table->store().size(), 0u);
    const auto first = ranked_table->store().at(0);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->at("account").is_string()) << first->dump();
    EXPECT_TRUE(is_business_id(first->at("account").get<std::string>()));

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{
        host.node, &host.cluster, {}, {}, &host.node.span_recorder()};
    const zetta::api::ApiResult scored = registry.call(
        "score",
        {{"model", model->id()},
         {"dataset_path", "churn_demo.real_accounts"},
         {"top_k", 5}},
        context);
    ASSERT_TRUE(scored.ok) << scored.error.message;
    ASSERT_EQ(scored.value.at("ranked").size(), 5u);
    for (const auto& score : scored.value.at("ranked")) {
        ASSERT_TRUE(score.at("entity").is_string()) << score.dump();
        EXPECT_TRUE(is_business_id(score.at("entity").get<std::string>()));
    }
    const zetta::api::ApiResult explained = registry.call(
        "explain",
        {{"model", model->id()},
         {"dataset_path", "churn_demo.real_accounts"},
         {"entity", "c00042"}},
        context);
    ASSERT_TRUE(explained.ok) << explained.error.message;
    EXPECT_EQ(explained.value.at("entity"), "c00042");
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

// "Why is the top one at risk?" — the per-account answer (#948 step 6, #960 item 5).
// The population drivers are the same list for every row, so they cannot say why THIS
// account is where it is.
TEST(ChurnTableRun, TheTopAccountCanSayWhyItIsAtRisk) {
    AttachedNode host(24726);
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
    const ChurnSolutionResult run = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, {});
    ASSERT_TRUE(run.ok()) << run.execution.error;
    const std::shared_ptr<zetta::ml::Model> model = host.node.find_model(run.model_id);
    ASSERT_TRUE(model);
    ASSERT_FALSE(run.ranked.empty());
    const std::int64_t top = run.ranked.front().entity;

    CpuDevice explain_device;
    const zetta::ml::RowExplanation why = zetta::ml::explain_entity(
        host.node, *model, resolved.table_id, resolved.contract, explain_device, top);
    ASSERT_TRUE(why.ok()) << why.error;

    ASSERT_TRUE(why.entity);
    EXPECT_EQ(*why.entity, top);
    // Every feature is accounted for, strongest influence first. A feature that did
    // nothing here sorts last rather than vanishing — "this column did not matter for
    // this account" is an answer.
    ASSERT_EQ(why.contributions.size(), 3u);
    for (std::size_t index = 1; index < why.contributions.size(); ++index) {
        EXPECT_GE(std::abs(why.contributions[index - 1].contribution),
                  std::abs(why.contributions[index].contribution)) << index;
    }

    // Efficiency: the contributions EXPLAIN the score rather than merely accompanying
    // it. This is what makes the answer checkable instead of a plausible story.
    double summed = why.base_value;
    for (const zetta::ml::FeatureContribution& contribution : why.contributions) {
        summed += contribution.contribution;
    }
    EXPECT_NEAR(summed, why.raw, 1e-6);
    EXPECT_NEAR(why.residual, 0.0, 1e-6);

    // The risk agrees with what the run itself ranked this account at, so the
    // explanation and the list cannot tell different stories.
    EXPECT_NEAR(why.risk, run.ranked.front().risk, 1e-6);

    // The reported values are this account's own, not the population's.
    for (const zetta::ml::FeatureContribution& contribution : why.contributions) {
        const std::optional<double> expected =
            warehouse_value(top, contribution.feature);
        ASSERT_TRUE(expected) << contribution.feature;
        EXPECT_DOUBLE_EQ(contribution.value, *expected) << contribution.feature;
    }
}

// Two accounts at similar risk can be there for different reasons, and that is the
// whole point of a per-row explanation. If the strongest driver were always the same
// column, the population ranking would already have answered the question.
TEST(ChurnTableRun, ExplanationsDifferBetweenAccounts) {
    AttachedNode host(24727);
    const std::string table_id = land_dataset(host.node, "churn_demo", "accounts", 300);
    ASSERT_FALSE(table_id.empty());
    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("churn_demo.accounts"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    CpuDevice device;
    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;
    const ChurnSolutionResult run = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, {});
    ASSERT_TRUE(run.ok()) << run.execution.error;
    const std::shared_ptr<zetta::ml::Model> model = host.node.find_model(run.model_id);
    ASSERT_TRUE(model);

    // A row invented to be at risk for a reason the population ranking would not
    // name first, against one that is healthy on every column.
    const zetta::ml::RowExplanation risky = zetta::ml::explain_row(
        *model, {{"failed_payments", 9}, {"logins_30d", 0}, {"nps", 1}});
    ASSERT_TRUE(risky.ok()) << risky.error;
    const zetta::ml::RowExplanation healthy = zetta::ml::explain_row(
        *model, {{"failed_payments", 0}, {"logins_30d", 40}, {"nps", 10}});
    ASSERT_TRUE(healthy.ok()) << healthy.error;

    EXPECT_GT(risky.risk, healthy.risk);
    // The same feature pushes one row up and the other down — the sign carries the
    // direction, which a magnitude-only population ranking cannot.
    const auto contribution_for = [](const zetta::ml::RowExplanation& explanation,
                                     const std::string& feature) {
        for (const zetta::ml::FeatureContribution& entry : explanation.contributions) {
            if (entry.feature == feature) return entry.contribution;
        }
        return 0.0;
    };
    EXPECT_GT(contribution_for(risky, "failed_payments"), 0.0);
    EXPECT_LT(contribution_for(healthy, "failed_payments"), 0.0);

    // A row missing a column the model needs fails rather than being zero-filled: a
    // zero is a real value to a GBDT, and would be explained as "no failed payments".
    const zetta::ml::RowExplanation incomplete =
        zetta::ml::explain_row(*model, {{"failed_payments", 3}, {"nps", 4}});
    EXPECT_FALSE(incomplete.ok());
    EXPECT_NE(incomplete.error.find("logins_30d"), std::string::npos) << incomplete.error;
}

// The capability is the deliverable, not the function behind it: a feature that
// cannot be called from the API and MCP is not shipped. `score` shipped once and was
// invisible to chat until it reached the allowlist.
TEST(ChurnTableRun, ExplainIsAnswerableThroughTheControlSurface) {
    AttachedNode host(24728);
    ASSERT_FALSE(land_dataset(host.node, "churn_demo", "accounts", 200).empty());
    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("churn_demo.accounts"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    CpuDevice device;
    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;
    const ChurnSolutionResult run = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, {});
    ASSERT_TRUE(run.ok()) << run.execution.error;
    ASSERT_FALSE(run.ranked.empty());

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{host.node};

    const zetta::api::ApiResult answered = registry.call(
        "explain",
        {{"model", run.model_id},
         {"dataset_path", "churn_demo.accounts"},
         {"entity", run.ranked.front().entity}},
        context);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(answered.value.at("entity").get<std::int64_t>(), run.ranked.front().entity);
    ASSERT_FALSE(answered.value.at("contributions").empty());
    EXPECT_NEAR(answered.value.at("residual").get<double>(), 0.0, 1e-6);
    EXPECT_NEAR(answered.value.at("risk").get<double>(), run.ranked.front().risk, 1e-6);

    // An account that is not in the data at all, explained the same way.
    const zetta::api::ApiResult hypothetical = registry.call(
        "explain",
        {{"model", run.model_id},
         {"row", {{"failed_payments", 9}, {"logins_30d", 0}, {"nps", 1}}}},
        context);
    ASSERT_TRUE(hypothetical.ok) << hypothetical.error.message;
    EXPECT_FALSE(hypothetical.value.contains("entity"));  // it is not one

    // Neither half given is a usable-answer failure, not a silent empty result.
    const zetta::api::ApiResult neither =
        registry.call("explain", {{"model", run.model_id}}, context);
    EXPECT_FALSE(neither.ok);
    EXPECT_EQ(neither.error.code, "invalid_input");
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
