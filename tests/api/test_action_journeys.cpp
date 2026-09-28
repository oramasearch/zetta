// The governed-action journeys (#617/#619, docs/testing/mcp-journeys.md) as
// DATA: Journey{JourneyStep{tool, input, expect}} tables (journey.hpp /
// journeys_canonical.hpp) run by the shared driver against the capability
// registry exactly as an agent would over MCP — layer 1, the same handlers
// the MCP endpoint renders, no HTTP. Every mutating step gets a read-back;
// negative journeys (gate-blocked deploy, denied capability, schema-rejected
// input) are first-class; a failure names the offending step and assertion.
//
// Replay one journey locally with its gtest filter, e.g.:
//   ./build/bin/zetta-tests --gtest_filter='Uc1Journey.*'
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#include <nlohmann/json.hpp>

#include "journey.hpp"
#include "journeys_canonical.hpp"
#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/autograd/safetensors.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/ml/vision.hpp"
#include "zetta/ml/language.hpp"
#include "zetta/ml/speech.hpp"
#include "zetta/node.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace {

namespace fs = std::filesystem;
namespace journey = zetta::test::journey;

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::BackgroundActions;
using zetta::api::Registry;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;

std::string vision_png() {
    const unsigned char bytes[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
        0x0D, 'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
        0x00, 0x00, 0x00, 0x0D, 'I',  'D',  'A',  'T',  0x08, 0xD7, 0x63,
        0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89,
        0x99, 0xD1, 0x8D, 0x00, 0x00, 0x00, 0x00, 'I',  'E',  'N',  'D',
        0xAE, 0x42, 0x60, 0x82};
    return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
}

// A standalone node + the registry + a background-action owner — the layer-1
// journey surface for the actions that need no compute plane.
struct ActionJourneys : ::testing::Test {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    BackgroundActions background;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override {
        background.prune();
        node.stop();
    }

    ApiResult call(const std::string& name, nlohmann::json input,
                   const std::string& key = {}) {
        ApiContext context{node,  nullptr,
                           key,   trace,
                           &node.span_recorder(), &background};
        auto result = registry.call(name, input, context);
        if (name == "get_run_report" && result.ok) {
            EXPECT_TRUE(result.value.contains("features"));
            if (result.value.contains("features")) {
                const auto& features = result.value.at("features");
                EXPECT_FALSE(features.at("used").empty()) << result.value.dump();
                EXPECT_FALSE(features.at("excluded").empty()) << result.value.dump();
                for (const auto& feature : features.at("used")) {
                    EXPECT_FALSE(feature.at("name").get<std::string>().empty());
                    EXPECT_FALSE(feature.at("source").empty());
                    EXPECT_FALSE(feature.at("transform").get<std::string>().empty());
                    EXPECT_FALSE(feature.at("why").get<std::string>().empty());
                }
                for (const auto& excluded : features.at("excluded")) {
                    EXPECT_FALSE(excluded.at("name").get<std::string>().empty());
                    EXPECT_FALSE(excluded.at("why").get<std::string>().empty());
                }
            }
        }
        return result;
    }

    ApiResult call_as_user(const std::string& user_id,
                           const std::string& name,
                           nlohmann::json input) {
        ApiContext context{node, nullptr, user_id, trace,
                           &node.span_recorder(), &background};
        context.acting_is_user = true;
        return registry.call(name, input, context);
    }

    // The layer-1 transport: the same registry the MCP endpoint renders,
    // with the acting key baked in.
    journey::Caller caller(const std::string& key = {}) {
        return [this, key](const std::string& tool,
                           const nlohmann::json& input) {
            return call(tool, input, key);
        };
    }

    // The owning-loop work a poll needs between attempts: tick (the owner
    // drain point for gated worker writes) + prune (finished workers join).
    journey::Pump pump() {
        return [this] {
            node.tick();
            background.prune();
        };
    }
};

// The UC-1 fixture needs the real compute plane: a node with its cluster
// attached (a live JobCoordinator), single-member — the AttachedNode shape.
struct Uc1Journey : ActionJourneys {
    Cluster cluster{"uc1-journey-node", [] {
                        ClusterConfig config;
                        config.listen_host = "127.0.0.1";
                        config.listen_port = 24710;
                        config.port_range = 8;
                        return config;
                    }()};

    void SetUp() override {
        ActionJourneys::SetUp();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
        ASSERT_NE(node.job_coordinator(), nullptr);
    }
};

struct SolutionDeliveryBoundary {
    zetta::connect::HttpTransport previous = zetta::connect::default_http_transport();
    zetta::connect::AsyncHttpTransport previous_async = zetta::connect::default_async_http_transport();
    std::vector<nlohmann::json> requests;

    SolutionDeliveryBoundary() {
        zetta::connect::set_default_http_transport([this](const auto& request) {
            requests.push_back(nlohmann::json::parse(request.body));
            return zetta::connect::HttpResponse{200, "{}", {}};
        }, {});
    }

    ~SolutionDeliveryBoundary() {
        zetta::connect::set_default_http_transport(previous, previous_async);
    }
};

struct SolutionJourney : ::testing::Test {
    TempDir dir;
    zetta::cluster::ReplicationManager replication{"solution-journey-node"};
    Node node{NodeConfig{dir.path().string(), false}, &replication};
    Cluster cluster{"solution-journey-node", [] {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = 24720;
        config.port_range = 8;
        return config;
    }(), &replication};
    Registry registry;
    BackgroundActions background;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        cluster.start();
        node.attach_cluster(cluster, 2);
        zetta::api::register_core_capabilities(registry);
        node.tick();
    }

    void TearDown() override {
        background.prune();
        node.stop();
    }

    ApiResult call(const std::string& name, const nlohmann::json& input) {
        ApiContext context{node, &cluster, {}, {}, &node.span_recorder(), &background};
        return registry.call(name, input, context);
    }

    journey::Caller caller() {
        return [this](const auto& name, const auto& input) {
            return call(name, input);
        };
    }

    journey::Pump pump() {
        return [this] {
            node.tick();
            background.prune();
        };
    }
};

void land_retention_data(Node& node) {
    const auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->create_table("customers");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const bool persuadable = index < 120;
        const int treatment = index % 2;
        const int outcome = persuadable ? treatment : 1 - treatment;
        table->add_row({{"entity", index + 1}, {"record_id", index + 1},
                       {"source", "fixture"}, {"name", "Customer " + std::to_string(index + 1)},
                       {"label", index % 10 != 0 ? 1 : 0}, {"feature", index % 13 + 0.5},
                       {"uplift_segment", persuadable ? 1.0 : 0.0},
                       {"treatment", treatment}, {"outcome", outcome},
                       {"duration", static_cast<double>(30 + index % 60)},
                       {"start_time", static_cast<double>(index)},
                       {"feature_time", static_cast<double>(index)},
                       {"time", "2026-08-01T00:00:00.000Z"},
                       {"in_collections", false}, {"open_complaint", false},
                       {"vulnerable", false}});
    }
    table->add_row({{"entity", 9999}, {"record_id", 9999}, {"source", "fixture"},
                   {"name", "Future customer"}, {"label", 1}, {"feature", 999.0},
                   {"uplift_segment", 1.0}, {"treatment", 1}, {"outcome", 1},
                   {"duration", 30.0}, {"start_time", 240.0},
                   {"feature_time", 240.0},
                   {"time", "2026-10-01T00:00:00.000Z"},
                   {"in_collections", false}, {"open_complaint", false}, {"vulnerable", false}});
    const auto written = zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

void land_demand_forecast_data(Node& node) {
    const auto database = node.create_database("demand");
    ASSERT_TRUE(database);
    const auto table = database->create_table("history");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int series = 1; series <= 60; ++series) {
        for (int day = 0; day < 42; ++day) {
            const int observation = series * 1000 + day;
            const double alternating = day % 2 == 0 ? -45.0 : 75.0;
            const double weekly =
                12.0 * std::sin(day * 2.0 * std::numbers::pi / 7.0);
            const double demand = 180.0 + series + alternating + weekly;
            table->add_row(
                {{"observation_id", observation},
                 {"record_id", observation},
                 {"source", "fixture"},
                 {"name", "record-" + std::to_string(observation)},
                 {"series", series},
                 {"item", "sku-" + std::to_string(series)},
                 {"location", "west"},
                 {"day", day},
                 {"demand", demand},
                 {"history_feature", static_cast<double>(day % 7)},
                 {"planner_owned", true},
                 {"forecast_fresh", true},
                 {"hierarchy_consistent", true},
                 {"stockout_censored", false},
                 {"override_locked", false},
                 {"manual_override", false},
                 {"override_reason", ""},
                 {"owner", "planner-west"}});
        }
    }
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

TEST_F(SolutionJourney,
       DemandForecastBuildsActsAndReportsFvaLimitsHonestly) {
    SolutionDeliveryBoundary delivery;
    land_demand_forecast_data(node);
    ASSERT_TRUE(call("create_project", {{"name", "demand planning"}}).ok);
    const ApiResult created = call(
        "create_solution",
        {{"project", "demand planning"},
         {"solution", "S-5"},
         {"level", 1},
         {"sources",
          {{"source", "demand"},
           {"table", "history"},
           {"M-3",
            {{"entity_column", "entity_id"},
             {"label_column", "demand"},
             {"feature_columns", {"history_feature"}}}},
           {"M-4",
            {{"time_column", "day"},
             {"target_column", "demand"},
             {"series_column", "series"},
             {"horizon", 7},
             {"seasonal_period", 7},
             {"lags", {1, 7, 14}},
             {"rolling_window", 7},
             {"backtest_folds", 3},
             {"num_trees", 8},
             {"max_depth", 3},
             {"seed", 1114}}},
           {"correctness",
            {{"hierarchy_columns", {"item", "location", "series"}}}}}},
         {"foundation",
          {{"snapshot",
            {{"database", "demand"},
             {"source_table", "history"},
             {"target_table", "planning_snapshot"},
             {"entity_column", "observation_id"},
             {"time_column", "day"},
             {"as_of", 41}}},
           {"dataset",
            {{"entity_column", "entity_id"},
             {"label_column", "demand"},
             {"time_column", "day"},
             {"as_of", 41},
             {"feature_columns", {"history_feature"}}}},
           {"delivery",
            {{"kind", "webhook"},
             {"endpoint", "https://planning.example.test/forecast"},
             {"owner", "planner-west"}}},
           {"roi_model",
            {{"value_per_outcome", 1.0},
             {"investment", 0.0},
             {"currency", "USD"}}}}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value.at("methodologies"),
              (nlohmann::json{"M-2", "M-4", "M-3"}));

    const auto project = node.find_project("demand planning");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph(
        created.value.at("graph").get<std::string>());
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(binding.at("decision_source").at("methodology"), "M-4");
    EXPECT_EQ(binding.at("decision_source").at("output_name"), "forecast");
    EXPECT_EQ(binding.at("decision_source").at("score_column"),
              "exception_score");

    const ApiResult started = call(
        "run_solution",
        {{"project", "demand planning"},
         {"graph", created.value.at("graph")},
         {"delivery_token", "fixture-only-token"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    ApiResult status;
    for (int attempt = 0; attempt < 8000; ++attempt) {
        node.tick();
        background.prune();
        status = call("get_run", {{"run_id", started.value.at("run_id")}});
        if (status.ok && status.value.at("status") != "running") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(status.ok) << status.error.message;
    if (status.value.at("status") != "succeeded") {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_EQ(status.value.at("status"), "succeeded")
        << status.value.dump(2);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::map<std::string, bool> protected_by_series;
    std::size_t exception_decisions = 0;
    const std::string policy_id = binding.at("policy_id");
    for (const auto& decision : policies.decisions()) {
        if (decision.policy_id != policy_id) {
            continue;
        }
        protected_by_series[decision.entity_id] = decision.holdout;
        if (decision.holdout) {
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        } else if (decision.actionable) {
            ++exception_decisions;
            EXPECT_EQ(decision.action, "exception_queue");
        }
    }
    ASSERT_EQ(protected_by_series.size(), 60u);
    EXPECT_TRUE(std::ranges::any_of(
        protected_by_series,
        [](const auto& entry) { return entry.second; }));
    EXPECT_GT(exception_decisions, 0u);

    std::size_t planner_reports = 0;
    std::size_t exception_tasks = 0;
    for (const auto& request : delivery.requests) {
        if (request.at("action") == "publish_driver_report") {
            ++planner_reports;
            EXPECT_EQ(request.at("payload").at("title"),
                      "Demand forecast planner report");
            for (const auto& row : request.at("payload")
                                       .at("driver_report")
                                       .at("forecast_rows")) {
                const std::string series =
                    std::to_string(row.at("series").get<int>());
                ASSERT_TRUE(protected_by_series.contains(series));
                EXPECT_FALSE(protected_by_series.at(series));
            }
            continue;
        }
        ++exception_tasks;
        const auto decision = policies.get_decision(
            request.at("payload").at("decision_id").get<std::string>());
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
        EXPECT_EQ(request.at("owner"), "planner-west");
        EXPECT_FALSE(request.at("sla_due_at").get<std::string>().empty());
        EXPECT_TRUE(request.at("payload").contains("point_forecast"));
        EXPECT_TRUE(request.at("payload").contains("lower"));
        EXPECT_TRUE(request.at("payload").contains("upper"));
        EXPECT_TRUE(request.at("payload").contains("exception_score"));
    }
    EXPECT_EQ(planner_reports, 1u);
    EXPECT_EQ(exception_tasks, exception_decisions);

    const ApiResult ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    std::size_t holdout_outcomes = 0;
    std::size_t treatment_outcomes = 0;
    for (const auto& entry : ledger.value.at("entries")) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string subject = entry.at("subject");
        if (!protected_by_series.contains(subject)) {
            continue;
        }
        const bool holdout = protected_by_series.at(subject);
        const double planted_error = holdout ? 0.4 : 0.2;
        const ApiResult recorded = call(
            "record_outcome",
            {{"action_id", entry.at("id")},
             {"outcome", "forecast_error"},
             {"value", planted_error},
             {"currency", "USD"},
             {"source", "connector"},
             {"occurred_at", entry.at("action_at")}});
        ASSERT_TRUE(recorded.ok) << recorded.error.message;
        if (holdout) {
            ++holdout_outcomes;
        } else {
            ++treatment_outcomes;
        }
    }
    EXPECT_GT(holdout_outcomes, 0u);
    EXPECT_GT(treatment_outcomes, 0u);

    const ApiResult readout = call(
        "get_roi_readout", {{"solution", binding.at("scope")}});
    ASSERT_TRUE(readout.ok) << readout.error.message;
    EXPECT_EQ(readout.value.at("measurement").at("design"), "fva");
    EXPECT_FALSE(readout.value.at("incrementality").at("claimable"));
    EXPECT_NE(readout.value.at("incrementality")
                  .at("reason")
                  .get<std::string>()
                  .find("module-specific estimator"),
              std::string::npos);
    EXPECT_FALSE(readout.value.at("roi_statement").at("claimable"));

    const auto monitor = node.find_monitor(
        binding.at("monitor_id").get<std::string>());
    ASSERT_TRUE(monitor);
    ASSERT_EQ(monitor->value().guardrails.size(), 5u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value()) << guardrail.bound.name;
    }
}

void land_kpi_anomaly_data(Node& node) {
    const auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->create_table("kpi_history");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const bool planted_anomaly = index > 55 && index % 20 == 0;
        const double seasonal =
            100.0 + 8.0 * std::sin(
                                static_cast<double>(index) *
                                2.0 * std::numbers::pi / 7.0);
        const double driver =
            40.0 + static_cast<double>(index % 9) +
            (planted_anomaly ? 35.0 : 0.0);
        const double kpi = seasonal + 0.4 * driver +
                           (planted_anomaly ? 55.0 : 0.0);
        table->add_row(
            {{"entity", index},
             {"period", static_cast<double>(index)},
             {"observed_at", "2026-08-01T00:00:00.000Z"},
             {"kpi", kpi},
             {"candidate_driver", driver},
             {"confirmed_anomaly", planted_anomaly ? 1 : 0},
             {"eligible", true},
             {"forum_owner_assigned", true},
             {"sensitive_driver_access", true},
             {"suppressed", false},
             {"owner", "weekly-kpi-forum"}});
    }
    table->add_row(
        {{"entity", 9999},
         {"period", 240.0},
         {"observed_at", "2026-10-01T00:00:00.000Z"},
         {"kpi", 9999.0},
         {"candidate_driver", 9999.0},
         {"confirmed_anomaly", 1},
         {"eligible", true},
         {"forum_owner_assigned", true},
         {"sensitive_driver_access", true},
         {"suppressed", false},
         {"owner", "weekly-kpi-forum"}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json kpi_anomaly_configuration() {
    return {
        {"snapshot",
         {{"database", "datasets"},
          {"source_table", "kpi_history"},
          {"target_table", "kpi_anomaly_snapshot"},
          {"entity_column", "entity"},
          {"time_column", "observed_at"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "confirmed_anomaly"},
          {"time_column", "period"},
          {"as_of", 239.0},
          {"feature_columns", {"kpi", "candidate_driver"}}}},
        {"decision_source",
         {{"methodology", "M-7"},
          {"entity_column", "row"},
          {"score_column", "score"},
          {"output_name", "anomaly table"}}},
        {"delivery",
         {{"kind", "webhook"},
          {"endpoint", "https://forum.example.test/kpi-decisions"},
          {"owner", "weekly-kpi-forum"}}},
        {"roi_model",
         {{"value_per_outcome", 0.0},
          {"investment", 0.0},
          {"currency", "USD"}}}};
}

journey::Journey planted_decision_quality_outcomes(
    const nlohmann::json& entries,
    const nlohmann::json& binding) {
    journey::Journey outcomes{"kpi-anomaly-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        outcomes.steps.push_back(
            {"record observed forum adoption",
             "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "decision_adopted"},
              {"value", holdout ? 0.2 : 0.35},
              {"currency", "USD"},
              {"source", "manual"},
              {"occurred_at", entry.at("action_at")}},
             {},
             {{"outcome_id", "/id"}}});
    }
    outcomes.steps.push_back(
        {"read honest phased decision-quality result",
         "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/measurement/design", "phased"),
              journey::equals("/incrementality/claimable", false),
              journey::at_least("/incrementality/holdout_samples", 1),
              journey::equals("/roi_statement/claimable", false),
              journey::predicate(
                  "phased readout states why no causal effect is claimable",
                  [](const auto& value) {
                      return value.at("incrementality")
                          .at("reason")
                          .template get<std::string>()
                          .contains("module-specific estimator");
                  })}}});
    return outcomes;
}

TEST_F(SolutionJourney,
       KpiAnomalyRoutesForumReportAndKeepsPhasedClaimsHonest) {
    SolutionDeliveryBoundary delivery;
    land_kpi_anomaly_data(node);
    journey::Journey flow{"kpi-anomaly-solution", {}};
    flow.steps = {
        {"read S-7 defaults",
         "get_solution",
         {{"solution", "S-7"}},
         {.checks = {
              journey::equals("/solution/runnable", true),
              journey::equals("/solution/minimal_template", false),
              journey::equals("/solution/measurement/design", "phased"),
              journey::equals(
                  "/solution/measurement/outcome/name", "decision_adopted")}}},
        {"create KPI anomaly project",
         "create_project",
         {{"name", "KPI anomaly forum"}}},
        {"create S-7 Pipeline",
         "create_solution",
         {{"project", "KPI anomaly forum"},
          {"solution", "S-7"},
          {"level", 1},
          {"sources",
           {{"source", "datasets"},
            {"table", "kpi_history"},
            {"M-7",
             {{"mode", "kpi_time_series"},
              {"entity_column", "entity_id"},
              {"label_column", "confirmed_anomaly"},
              {"time_column", "period"},
              {"feature_columns", {"kpi", "candidate_driver"}},
              {"alert_budget", 12},
              {"seasonal_period", 7},
              {"trend_window", 28},
              {"residual_window", 56},
              {"seasonal_cycles", 8}}},
            {"M-3",
             {{"entity_column", "entity_id"},
              {"label_column", "confirmed_anomaly"},
              {"time_column", "period"},
              {"as_of", 239.0},
              {"feature_columns", {"kpi", "candidate_driver"}}}}}},
          {"foundation", kpi_anomaly_configuration()}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"anomaly_graph", "/graph"},
          {"anomaly_experiment", "/experiment_id"},
          {"anomaly_readout", "/readout_id"}}},
        {"run S-7 Pipeline",
         "run_solution",
         {{"project", "KPI anomaly forum"},
          {"graph", "$anomaly_graph"},
          {"delivery_token", "fixture-only-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"anomaly_run", "/run_id"}}},
        {"read completed S-7 Run",
         "get_run",
         {{"run_id", "$anomaly_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/lineage/steps", 9)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read protected forum assignments",
         "get_experiment",
         {{"experiment", "$anomaly_experiment"}},
         {.checks = {
              journey::at_least("/sizes/assignments", 200),
              journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read forum action ledger",
         "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 200)}}}};

    const auto report = journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("KPI anomaly forum");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-7 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    std::size_t actionable_alerts = 0;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        }
        if (decision.actionable) {
            ++actionable_alerts;
        }
    }
    EXPECT_GT(protected_rows, 0u);
    EXPECT_GT(actionable_alerts, 0u);
    EXPECT_LE(actionable_alerts, 12u);

    std::size_t forum_tasks = 0;
    std::size_t forum_reports = 0;
    for (const auto& request : delivery.requests) {
        if (request.at("action") == "publish_driver_report") {
            ++forum_reports;
            EXPECT_FALSE(request.at("payload")
                             .at("decision_log")
                             .empty());
            EXPECT_EQ(request.at("payload")
                          .at("driver_report")
                          .at("alert_budget"),
                      12);
            EXPECT_FALSE(request.at("payload")
                             .at("driver_report")
                             .at("causal_caveat")
                             .get<std::string>()
                             .empty());
            continue;
        }
        ++forum_tasks;
        EXPECT_EQ(request.at("action"), "decision_forum_report");
        EXPECT_TRUE(request.at("payload").at("decision_log_required"));
        EXPECT_FALSE(request.at("payload")
                         .at("top_driver")
                         .get<std::string>()
                         .empty());
        EXPECT_FALSE(request.at("payload")
                         .at("causal_caveat")
                         .get<std::string>()
                         .empty());
    }
    EXPECT_EQ(forum_tasks, actionable_alerts);
    EXPECT_EQ(forum_reports, 1u);

    const auto snapshot =
        node.find_database("datasets")->find_table("kpi_anomaly_snapshot");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->store().size(), 240u);
    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 6u);
    const auto alert_guardrail = std::ranges::find_if(
        monitor->value().guardrails,
        [](const auto& guardrail) {
            return guardrail.bound.name == "alert_volume";
        });
    ASSERT_NE(alert_guardrail, monitor->value().guardrails.end());
    ASSERT_TRUE(alert_guardrail->value.has_value());
    EXPECT_EQ(*alert_guardrail->value, 12.0);

    const auto ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"),
              std::string::npos);
    const auto outcomes = planted_decision_quality_outcomes(
        ledger.value.at("entries"), binding);
    const auto measured = journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const auto readout = call(
            "get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

void land_predictive_maintenance_data(Node& node) {
    const auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->create_table("assets");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const bool high_risk = index % 3 != 0;
        const bool failed = high_risk ? index % 5 != 0 : index % 8 == 0;
        const double duration = failed
                                    ? static_cast<double>(high_risk
                                                              ? 2 + index % 4
                                                              : 9 + index % 5)
                                    : static_cast<double>(18 + index % 8);
        const std::string asset_class =
            index % 2 == 0 ? "pump" : "compressor";
        table->add_row(
            {{"entity", index + 1},
             {"record_id", index + 1},
             {"source", "sensor_fixture"},
             {"name", "Asset " + std::to_string(index + 1)},
             {"event", failed ? 1 : 0},
             {"duration", duration},
             {"start_time", static_cast<double>(index)},
             {"feature_time", static_cast<double>(index)},
             {"vibration", high_risk ? 9.0 + index % 3 : 1.0 + index % 2},
             {"temperature", high_risk ? 95.0 + index % 4 : 55.0 + index % 3},
             {"time", "2026-08-01T00:00:00.000Z"},
             {"asset_class", asset_class},
             {"owner", asset_class + "-maintenance"},
             {"maintenance_window", "2026-W40"},
             {"maintenance_eligible", true},
             {"approved_asset_class", true},
             {"maintenance_window_available", true},
             {"safe_to_service", true},
             {"safety_lockout", false},
             {"active_work_order", false},
             {"sensor_data_untrusted", false},
             {"failure_within_horizon", failed && duration <= 12.0},
             {"false_work_order", false},
             {"sensor_drift_psi", 0.04},
             {"maintenance_capacity_utilization", 0.6}});
    }
    table->add_row(
        {{"entity", 9999},
         {"record_id", 9999},
         {"source", "sensor_fixture"},
         {"name", "Future asset"},
         {"event", 1},
         {"duration", 1.0},
         {"start_time", 240.0},
         {"feature_time", 240.0},
         {"vibration", 999.0},
         {"temperature", 999.0},
         {"time", "2026-10-01T00:00:00.000Z"},
         {"asset_class", "pump"},
         {"owner", "pump-maintenance"},
         {"maintenance_window", "2026-W44"},
         {"maintenance_eligible", true},
         {"approved_asset_class", true},
         {"maintenance_window_available", true},
         {"safe_to_service", true},
         {"safety_lockout", false},
         {"active_work_order", false},
         {"sensor_data_untrusted", false},
         {"failure_within_horizon", true},
         {"false_work_order", false},
         {"sensor_drift_psi", 0.04},
         {"maintenance_capacity_utilization", 0.6}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json predictive_maintenance_configuration() {
    return {
        {"snapshot",
         {{"database", "datasets"},
          {"source_table", "assets"},
          {"target_table", "predictive_maintenance_snapshot"},
          {"entity_column", "entity"},
          {"time_column", "time"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "event"},
          {"time_column", "start_time"},
          {"as_of", 239.0},
          {"feature_columns", {"vibration", "temperature"}}}},
        {"delivery",
         {{"kind", "webhook"},
          {"endpoint", "https://cmms.example.test/work-orders"},
          {"owner", "maintenance-planning"}}},
        {"decision_source",
         {{"methodology", "M-5"},
          {"output_name", "survival table"},
          {"entity_column", "entity"},
          {"score_column", "hazard"},
          {"horizon", 30.0}}},
        {"roi_model",
         {{"value_per_outcome", 500.0},
          {"investment", 1000.0},
          {"currency", "USD"}}}};
}

journey::Journey planted_maintenance_outcomes(
    const nlohmann::json& entries,
    const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") == "action") {
            ++counts[entry.at("cell")];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"predictive-maintenance-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered =
            static_cast<double>(positions[cell]++) -
            (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double failure_avoided =
            0.2 + 0.1 * centered / counts[cell] +
            (holdout ? 0.0 : 0.18);
        outcomes.steps.push_back(
            {"record observed failure avoidance",
             "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "failure_avoided"},
              {"value", failure_avoided},
              {"currency", "USD"},
              {"source", "connector"},
              {"occurred_at", entry.at("action_at")}},
             {}});
    }
    outcomes.steps.push_back(
        {"read measured maintenance effect",
         "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/incrementality/claimable", true),
              journey::predicate(
                  "planted maintenance effect is inside its confidence interval",
                  [](const auto& value) {
                      const auto& estimate =
                          value.at("incrementality").at("combined");
                      return std::abs(
                                 estimate.at("effect").template get<double>() -
                                 0.18) < 1e-8 &&
                             estimate.at("confidence_interval")
                                     .at("lower")
                                     .template get<double>() <= 0.18 &&
                             estimate.at("confidence_interval")
                                     .at("upper")
                                     .template get<double>() >= 0.18;
                  })}}});
    return outcomes;
}

TEST_F(SolutionJourney,
       PredictiveMaintenanceCreatesWorkOrdersAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_predictive_maintenance_data(node);
    journey::Journey flow{"predictive-maintenance-solution", {}};
    flow.steps = {
        {"read S-8 defaults",
         "get_solution",
         {{"solution", "S-8"}},
         {.checks = {journey::equals("/solution/runnable", true),
                     journey::equals("/solution/minimal_template", false),
                     journey::equals(
                         "/solution/measurement/outcome/name",
                         "failure_avoided")}}},
        {"create maintenance project",
         "create_project",
         {{"name", "predictive maintenance"}}},
        {"create S-8 Pipeline",
         "create_solution",
         {{"project", "predictive maintenance"},
          {"solution", "S-8"},
          {"level", 1},
          {"sources",
           {{"source", "datasets"},
            {"table", "assets"},
            {"M-5",
             {{"duration_column", "duration"},
              {"event_indicator", "event"}}}}},
          {"foundation", predictive_maintenance_configuration()}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"maintenance_graph", "/graph"},
          {"maintenance_experiment", "/experiment_id"},
          {"maintenance_readout", "/readout_id"}}},
        {"run S-8 Pipeline",
         "run_solution",
         {{"project", "predictive maintenance"},
          {"graph", "$maintenance_graph"},
          {"delivery_token", "fixture-only-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"maintenance_run", "/run_id"}}},
        {"read completed maintenance Run",
         "get_run",
         {{"run_id", "$maintenance_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/lineage/steps", 9)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read protected maintenance assignments",
         "get_experiment",
         {{"experiment", "$maintenance_experiment"}},
         {.checks = {
              journey::at_least("/sizes/assignments", 100),
              journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read maintenance action ledger",
         "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}};

    const auto report = journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("predictive maintenance");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-8 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        }
    }
    EXPECT_GT(protected_rows, 0u);
    ASSERT_FALSE(delivery.requests.empty());
    for (const auto& request : delivery.requests) {
        const auto& payload = request.at("payload");
        EXPECT_FALSE(payload.at("asset_id").get<std::string>().empty());
        EXPECT_FALSE(payload.at("asset_class").get<std::string>().empty());
        EXPECT_FALSE(payload.at("cmms_owner").get<std::string>().empty());
        EXPECT_FALSE(payload.at("maintenance_window").get<std::string>().empty());
        EXPECT_GT(payload.at("risk_horizon").get<double>(), 0.0);
        EXPECT_GT(payload.at("expected_time_to_failure").get<double>(), 0.0);
        EXPECT_GT(payload.at("median_time_to_failure").get<double>(), 0.0);
        const auto decision = policies.get_decision(payload.at("decision_id"));
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
    }

    const auto snapshot = node.find_database("datasets")
                              ->find_table("predictive_maintenance_snapshot");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->store().size(), 240u);
    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 6u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value());
    }

    const auto ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"),
              std::string::npos);
    const auto outcomes =
        planted_maintenance_outcomes(ledger.value.at("entries"), binding);
    const auto measured = journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const auto readout = call(
            "get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

void land_automated_outbound_data(Node& node) {
    const auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->create_table("outbound_customers");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const bool suppressed = index % 30 == 0;
        const bool consent = index % 35 != 0;
        table->add_row(
            {{"entity", index + 1},
             {"record_id", index + 1},
             {"source", "outbound_fixture"},
             {"name", "Outbound customer " + std::to_string(index + 1)},
             {"response", index % 10 != 0 ? 1 : 0},
             {"engagement", index % 13 + 0.5},
             {"start_time", static_cast<double>(index)},
             {"time", "2026-08-01T00:00:00.000Z"},
             {"consent", consent},
             {"suppressed", suppressed},
             {"channel_eligible", true},
             {"unsubscribed", false},
             {"do_not_contact", false},
             {"contacts", index % 20 == 0 ? 4 : 0},
             {"recent_actions", index % 25 == 0 ? 1 : 0},
             {"channel", index % 2 == 0 ? "email" : "sms"},
             {"complaint", false}});
    }
    table->add_row(
        {{"entity", 9999},
         {"record_id", 9999},
         {"source", "outbound_fixture"},
         {"name", "Future outbound customer"},
         {"response", 1},
         {"engagement", 999.0},
         {"start_time", 240.0},
         {"time", "2026-10-01T00:00:00.000Z"},
         {"consent", true},
         {"suppressed", false},
         {"channel_eligible", true},
         {"unsubscribed", false},
         {"do_not_contact", false},
         {"contacts", 0},
         {"recent_actions", 0},
         {"channel", "email"},
         {"complaint", false}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json automated_outbound_configuration() {
    return {
        {"snapshot",
         {{"database", "datasets"},
          {"source_table", "outbound_customers"},
          {"target_table", "automated_outbound_snapshot"},
          {"entity_column", "entity"},
          {"time_column", "time"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "response"},
          {"time_column", "start_time"},
          {"as_of", 239.0},
          {"feature_columns", {"engagement"}}}},
        {"delivery",
         {{"kind", "webhook"},
          {"endpoint", "https://journeys.example.test/trigger"},
          {"owner", "lifecycle-marketing"}}},
        {"roi_model",
         {{"value_per_outcome", 40.0},
          {"investment", 150.0},
          {"currency", "USD"}}}};
}

journey::Journey planted_outbound_outcomes(
    const nlohmann::json& entries,
    const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") == "action") {
            ++counts[entry.at("cell")];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"automated-outbound-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered =
            static_cast<double>(positions[cell]++) -
            (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double response =
            0.2 + 0.1 * centered / counts[cell] +
            (holdout ? 0.0 : 0.12);
        outcomes.steps.push_back(
            {"record observed outbound response",
             "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "response"},
              {"value", response},
              {"currency", "USD"},
              {"source", "connector"},
              {"occurred_at", entry.at("action_at")}},
             {},
             {}});
    }
    outcomes.steps.push_back(
        {"read measured outbound effect",
         "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/incrementality/claimable", true),
              journey::predicate(
                  "planted outbound effect is inside its confidence interval",
                  [](const auto& value) {
                      const auto& estimate =
                          value.at("incrementality").at("combined");
                      return std::abs(
                                 estimate.at("effect").template get<double>() -
                                 0.12) < 1e-8 &&
                             estimate.at("confidence_interval")
                                     .at("lower")
                                     .template get<double>() <= 0.12 &&
                             estimate.at("confidence_interval")
                                     .at("upper")
                                     .template get<double>() >= 0.12;
                  })}}});
    return outcomes;
}

TEST_F(SolutionJourney,
       AutomatedOutboundRespectsEligibilityCapsAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_automated_outbound_data(node);
    journey::Journey flow{"automated-outbound-solution", {}};
    flow.steps = {
        {"read S-9 defaults",
         "get_solution",
         {{"solution", "S-9"}},
         {.checks = {journey::equals("/solution/runnable", true),
                     journey::equals("/solution/minimal_template", false),
                     journey::equals(
                         "/solution/measurement/outcome/name", "response"),
                     journey::equals(
                         "/solution/policy_template/contact_cap", 4)}}},
        {"create outbound project",
         "create_project",
         {{"name", "automated outbound"}}},
        {"create S-9 Pipeline",
         "create_solution",
         {{"project", "automated outbound"},
          {"solution", "S-9"},
          {"level", 1},
          {"sources",
           {{"source", "datasets"},
            {"table", "outbound_customers"},
            {"M-3",
             {{"entity_column", "entity_id"},
              {"label_column", "response"},
              {"feature_columns", {"engagement"}}}}}},
          {"foundation", automated_outbound_configuration()}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"outbound_graph", "/graph"},
          {"outbound_experiment", "/experiment_id"},
          {"outbound_readout", "/readout_id"}}},
        {"run S-9 Pipeline",
         "run_solution",
         {{"project", "automated outbound"},
          {"graph", "$outbound_graph"},
          {"delivery_token", "fixture-only-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"outbound_run", "/run_id"}}},
        {"read completed outbound Run",
         "get_run",
         {{"run_id", "$outbound_run"}},
         {.checks = {
              journey::equals("/status", "succeeded"),
              journey::size_equals("/lineage/steps", 9),
              journey::any_element(
                  "/outputs", {{"/name", "outbound response scores"}})}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read protected outbound assignments",
         "get_experiment",
         {{"experiment", "$outbound_experiment"}},
         {.checks = {
              journey::at_least("/sizes/assignments", 100),
              journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read outbound action ledger",
         "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}};

    const auto report = journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("automated outbound");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-9 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(binding.at("decision_source").at("output_name"),
              "outbound response scores");
    EXPECT_EQ(binding.at("decision_source").at("score_column"),
              "response_propensity");
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    std::size_t eligibility_blocks = 0;
    std::size_t cap_blocks = 0;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
        }
        const auto& evidence = decision.evidence;
        if (!evidence.value("consent", false) ||
            evidence.value("suppressed", true)) {
            ++eligibility_blocks;
            EXPECT_FALSE(decision.actionable);
        }
        if (evidence.value("contacts", 0) >= 4 ||
            evidence.value("recent_actions", 0) >= 1) {
            ++cap_blocks;
            EXPECT_FALSE(decision.actionable);
        }
    }
    EXPECT_GT(protected_rows, 0u);
    EXPECT_GT(eligibility_blocks, 0u);
    EXPECT_GT(cap_blocks, 0u);
    ASSERT_FALSE(delivery.requests.empty());
    for (const auto& request : delivery.requests) {
        const auto& payload = request.at("payload");
        EXPECT_TRUE(payload.at("consent").get<bool>());
        EXPECT_FALSE(payload.at("suppressed").get<bool>());
        EXPECT_LT(payload.at("contacts").get<int>(), 4);
        EXPECT_LT(payload.at("recent_actions").get<int>(), 1);
        EXPECT_FALSE(payload.at("channel").get<std::string>().empty());
        EXPECT_TRUE(payload.at("decision_cell").contains("audience_tier"));
        const auto decision = policies.get_decision(payload.at("decision_id"));
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
    }

    const auto snapshot = node.find_database("datasets")
                              ->find_table("automated_outbound_snapshot");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->store().size(), 240u);
    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 5u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value());
    }

    const auto ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"),
              std::string::npos);
    const auto outcomes =
        planted_outbound_outcomes(ledger.value.at("entries"), binding);
    const auto measured = journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const auto readout = call(
            "get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

void land_lead_scoring_data(Node& node) {
    const auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->create_table("leads");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const std::string territory = index % 2 == 0 ? "west" : "east";
        table->add_row(
            {{"entity", index + 1},
             {"record_id", index + 1},
             {"source", "crm_fixture"},
             {"name", "Lead " + std::to_string(index + 1)},
             {"label", index % 10 != 0 ? 1 : 0},
             {"engagement", index % 13 + 0.5},
             {"start_time", static_cast<double>(index)},
             {"time", "2026-08-01T00:00:00.000Z"},
             {"eligible", true},
             {"excluded", false},
             {"do_not_contact", false},
             {"territory_eligible", true},
             {"rep_capacity_available", true},
             {"territory", territory},
             {"owner", territory + "-sales"}});
    }
    table->add_row(
        {{"entity", 9999},
         {"record_id", 9999},
         {"source", "crm_fixture"},
         {"name", "Future lead"},
         {"label", 1},
         {"engagement", 999.0},
         {"start_time", 240.0},
         {"time", "2026-10-01T00:00:00.000Z"},
         {"eligible", true},
         {"excluded", false},
         {"do_not_contact", false},
         {"territory_eligible", true},
         {"rep_capacity_available", true},
         {"territory", "west"},
         {"owner", "west-sales"}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json retention_configuration() {
    return {
        {"snapshot", {{"database", "datasets"}, {"source_table", "customers"},
                      {"target_table", "retention_snapshot"}, {"entity_column", "entity"},
                      {"time_column", "time"}, {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset", {{"entity_column", "entity_id"}, {"label_column", "label"},
                     {"time_column", "start_time"},
                     {"as_of", 239.0},
                     {"feature_columns", {"feature", "uplift_segment"}}}},
        {"delivery", {{"endpoint", "https://crm.example.test/tasks"}, {"owner", "retention-team"}}},
        {"roi_model", {{"value_per_outcome", 100.0}, {"investment", 200.0}, {"currency", "USD"}}}};
}

nlohmann::json lead_scoring_configuration() {
    return {
        {"snapshot",
         {{"database", "datasets"},
          {"source_table", "leads"},
          {"target_table", "lead_scoring_snapshot"},
          {"entity_column", "entity"},
          {"time_column", "time"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "label"},
          {"time_column", "start_time"},
          {"as_of", 239.0},
          {"feature_columns", {"engagement"}}}},
        {"delivery",
         {{"kind", "webhook"},
          {"endpoint", "https://crm.example.test/lead-queues"},
          {"owner", "sales-operations"}}},
        {"roi_model",
         {{"value_per_outcome", 250.0},
          {"investment", 300.0},
          {"currency", "USD"}}}};
}

journey::Journey retention_journey(const nlohmann::json& foundation) {
    journey::Journey flow{"solution-foundation", {}};
    flow.steps = {
        {"discover the catalogue", "list_solutions", nlohmann::json::object(),
         {.checks = {journey::size_equals("/solutions", 20)}}},
        {"read executable defaults", "get_solution", {{"solution", "S-1"}},
         {.checks = {journey::equals("/solution/runnable", true),
                     journey::equals("/solution/policy_template/kill_switch", false),
                     journey::equals("/solution/levels/0/level", 1),
                     journey::equals("/solution/levels/3/level", 4),
                     journey::size_equals("/solution/missing_components", 0)}}},
        {"create the project", "create_project", {{"name", "retention"}}},
        {"read the project", "get_project", {{"name", "retention"}}},
        {"create the composed solution", "create_solution",
         {{"project", "retention"}, {"solution", "S-1"}, {"level", 1},
          {"sources", {{"source", "datasets"}, {"table", "customers"},
                       {"M-3", {{"entity_column", "entity_id"}, {"label_column", "label"},
                                {"feature_columns", {"feature"}}}}}},
          {"foundation", foundation}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"solution_graph", "/graph"}, {"policy", "/policy_id"},
          {"experiment", "/experiment_id"}, {"readout", "/readout_id"}}},
        {"read the composed Pipeline", "get_project", {{"name", "retention"}},
         {.checks = {journey::any_element("/graphs", {{"/name", "$solution_graph"}})}}},
        {"read the versioned policy", "get_policy", {{"policy_id", "$policy"}},
         {.checks = {journey::equals("/current_version", 1),
                     journey::equals("/versions/0/config/kill_switch", false)}}},
        {"read the experiment", "get_experiment", {{"experiment", "$experiment"}},
         {.checks = {journey::equals("/experiment/allocations/0/fraction", 0.1)}}},
        {"read the configured ROI model", "get_roi_readout", {{"solution", "$readout"}},
         {.checks = {journey::equals("/incrementality/claimable", false)}}},
        {"run the composed solution", "run_solution",
         {{"project", "retention"}, {"graph", "$solution_graph"}, {"delivery_token", "fixture-only-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"solution_run", "/run_id"}}},
        {"refuse concurrent replacement of this graph's score artifacts", "run_solution",
         {{"project", "retention"}, {"graph", "$solution_graph"}},
         {.ok = false, .error_code = "invalid_input", .error_contains = {"already active"}}},
        {"read the completed Run and lineage", "get_run", {{"run_id", "$solution_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/lineage/steps", 9)}},
         {}, journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read assignments after the run", "get_experiment", {{"experiment", "$experiment"}},
         {.checks = {journey::at_least("/sizes/assignments", 100),
                     journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read ledger rows after the run", "list_ledger", nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}
    };
    return flow;
}

journey::Journey planted_retention_outcomes(const nlohmann::json& entries,
                                            const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") == "action") {
            ++counts[entry.at("cell")];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"solution-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered = static_cast<double>(positions[cell]++) -
                                (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double response = 0.3 + 0.1 * centered / counts[cell] + (holdout ? 0.0 : 0.2);
        outcomes.steps.push_back({"record observed outcome", "record_outcome",
            {{"action_id", entry.at("id")}, {"outcome", "retained"}, {"value", response},
             {"currency", "USD"}, {"source", "connector"}, {"occurred_at", entry.at("action_at")}},
            {}, {{"outcome_id", "/id"}}});
        outcomes.steps.push_back({"read the recorded outcome", "list_ledger", nlohmann::json::object(),
            {.checks = {journey::any_element("/entries", {{"/id", "$outcome_id"}})}}});
    }
    outcomes.steps.push_back({"read measured treatment effect", "get_roi_readout",
        {{"solution", binding.at("scope")}},
        {.checks = {journey::equals("/incrementality/claimable", true),
                    journey::predicate("planted effect is inside its confidence interval",
                        [](const auto& value) {
                            const auto& estimate = value.at("incrementality").at("combined");
                            return std::abs(estimate.at("effect").template get<double>() - 0.2) < 1e-8 &&
                                   estimate.at("confidence_interval").at("lower").template get<double>() <= 0.2 &&
                                   estimate.at("confidence_interval").at("upper").template get<double>() >= 0.2;
                        })}}});
    return outcomes;
}

journey::Journey planted_conversion_outcomes(
    const nlohmann::json& entries,
    const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") == "action") {
            ++counts[entry.at("cell")];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"lead-scoring-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered =
            static_cast<double>(positions[cell]++) -
            (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double converted =
            0.25 + 0.1 * centered / counts[cell] +
            (holdout ? 0.0 : 0.15);
        outcomes.steps.push_back(
            {"record observed conversion",
             "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "converted"},
              {"value", converted},
              {"currency", "USD"},
              {"source", "connector"},
              {"occurred_at", entry.at("action_at")}},
             {},
             {{"outcome_id", "/id"}}});
    }
    outcomes.steps.push_back(
        {"read measured conversion effect",
         "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/incrementality/claimable", true),
              journey::predicate(
                  "planted conversion effect is inside its confidence interval",
                  [](const auto& value) {
                      const auto& estimate =
                          value.at("incrementality").at("combined");
                      return std::abs(
                                 estimate.at("effect").template get<double>() -
                                 0.15) < 1e-8 &&
                             estimate.at("confidence_interval")
                                     .at("lower")
                                     .template get<double>() <= 0.15 &&
                             estimate.at("confidence_interval")
                                     .at("upper")
                                     .template get<double>() >= 0.15;
                  })}}});
    return outcomes;
}

void land_lifetime_value_data(Node& node) {
    const auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->create_table("customer_value");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const double value_signal =
            static_cast<double>(index % 16) + 1.0;
        const double engagement =
            static_cast<double>((index * 7) % 11) / 10.0;
        const double future_value =
            200.25 + value_signal * 100.0 + engagement * 50.0;
        table->add_row(
            {{"entity", index + 1},
             {"record_id", index + 1},
             {"source", "fixture"},
             {"name", "Customer " + std::to_string(index + 1)},
             {"future_value", future_value},
             {"value_signal", value_signal},
             {"engagement", engagement},
             {"tenure_days", 180 + index % 90},
             {"start_time", static_cast<double>(index)},
             {"time", "2026-08-01T00:00:00.000Z"},
             {"consent", true},
             {"channel_eligible", true},
             {"margin", 0.4},
             {"do_not_target", false},
             {"account_closed", false},
             {"cost", 10.0},
             {"discount_fairness_gap", 0.0},
             {"cohort_calibration_error", 0.0}});
    }
    table->add_row(
        {{"entity", 9999},
         {"record_id", 9999},
         {"source", "fixture"},
         {"name", "Future customer"},
         {"future_value", 5000.0},
         {"value_signal", 50.0},
         {"engagement", 1.0},
         {"tenure_days", 365},
         {"start_time", 240.0},
         {"time", "2026-10-01T00:00:00.000Z"},
         {"consent", true},
         {"channel_eligible", true},
         {"margin", 0.4},
         {"do_not_target", false},
         {"account_closed", false},
         {"cost", 10.0},
         {"discount_fairness_gap", 0.0},
         {"cohort_calibration_error", 0.0}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json lifetime_value_configuration() {
    return {
        {"snapshot",
         {{"database", "datasets"},
          {"source_table", "customer_value"},
          {"target_table", "lifetime_value_snapshot"},
          {"entity_column", "entity"},
          {"time_column", "time"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "future_value"},
          {"time_column", "start_time"},
          {"as_of", 239.0},
          {"feature_columns",
           {"value_signal", "engagement", "tenure_days"}}}},
        {"delivery",
         {{"endpoint", "https://activation.example.test/audiences"},
          {"owner", "growth-operations"}}},
        {"roi_model",
         {{"value_per_outcome", 1.0},
          {"investment", 1000.0},
          {"currency", "USD"}}}};
}

journey::Journey lifetime_value_journey(
    const nlohmann::json& foundation) {
    journey::Journey flow{"s3-customer-lifetime-value", {}};
    flow.steps = {
        {"read S-3 executable defaults", "get_solution",
         {{"solution", "S-3"}},
         {.checks = {
              journey::equals("/solution/runnable", true),
              journey::equals("/solution/minimal_template", false),
              journey::equals("/solution/measurement/outcome/name",
                              "net_value"),
              journey::equals("/solution/measurement/outcome/window_days",
                              365),
              journey::size_equals("/solution/missing_components", 0)}}},
        {"create the lifetime value project", "create_project",
         {{"name", "customer lifetime value"}}},
        {"create the S-3 Pipeline", "create_solution",
         {{"project", "customer lifetime value"},
          {"solution", "S-3"},
          {"level", 1},
          {"sources",
           {{"source", "datasets"},
            {"table", "customer_value"},
            {"M-1",
             {{"label_column", "future_value"},
              {"value_horizon_days", 365}}},
            {"M-3",
             {{"entity_column", "entity_id"},
              {"label_column", "future_value"},
              {"feature_columns",
               {"value_signal", "engagement", "tenure_days"}}}}}},
          {"foundation", foundation}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"solution_graph", "/graph"},
          {"experiment", "/experiment_id"},
          {"monitor", "/monitor_id"},
          {"readout", "/readout_id"}}},
        {"run S-3", "run_solution",
         {{"project", "customer lifetime value"},
          {"graph", "$solution_graph"},
          {"delivery_token", "fixture-only-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"solution_run", "/run_id"}}},
        {"read the completed S-3 Run", "get_run",
         {{"run_id", "$solution_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/lineage/steps", 9)}},
         {}, journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read protected S-3 assignments", "get_experiment",
         {{"experiment", "$experiment"}},
         {.checks = {journey::at_least("/sizes/assignments", 100),
                     journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read S-3 action evidence", "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}};
    return flow;
}

journey::Journey planted_lifetime_value_outcomes(
    const nlohmann::json& entries, const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        if (cell.ends_with("/holdout") ||
            cell.ends_with("/audience_sync") ||
            cell.ends_with("/budget_reallocation")) {
            ++counts[cell];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"s3-planted-net-value", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        if (!counts.contains(cell)) {
            continue;
        }
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered =
            static_cast<double>(positions[cell]++) -
            (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double net_value =
            400.0 + centered / counts[cell] + (holdout ? 0.0 : 75.0);
        outcomes.steps.push_back(
            {"record planted net value", "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "net_value"},
              {"value", net_value},
              {"currency", "USD"},
              {"source", "connector"},
              {"occurred_at", entry.at("action_at")}}});
    }
    outcomes.steps.push_back(
        {"read measured net-value effect", "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/incrementality/claimable", true),
              journey::predicate(
                  "planted net-value effect is inside its confidence interval",
                  [](const auto& value) {
                      const auto& estimate =
                          value.at("incrementality").at("combined");
                      return std::abs(
                                 estimate.at("effect").template get<double>() -
                                 75.0) < 1e-8 &&
                             estimate.at("confidence_interval")
                                     .at("lower")
                                     .template get<double>() <= 75.0 &&
                             estimate.at("confidence_interval")
                                     .at("upper")
                                     .template get<double>() >= 75.0;
                  })}}});
    return outcomes;
}

void land_claims_data(Node& node) {
    const auto database = node.create_database("claims");
    ASSERT_TRUE(database);
    const auto table = database->create_table("fnol");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const int claim_id = index + 1;
        const double referral_signal =
            static_cast<double>(index % 20) / 19.0;
        const bool label =
            (referral_signal >= 0.55) != (index % 23 == 0);
        const std::string claim_number =
            "CLM-" + std::to_string(10000 + claim_id);
        const std::string loss_date =
            "2026-08-" + std::to_string(10 + index % 18);
        const std::string loss_amount =
            std::to_string(500 + (index % 20) * 250);
        const std::string document =
            "claim_number: " + claim_number + "\nloss_date: " +
            loss_date + "\nloss_amount: " + loss_amount;
        table->add_row(
            {{"claim_id", claim_id},
             {"record_id", claim_id},
             {"source", "fnol-fixture"},
             {"name", "Claim " + std::to_string(claim_id)},
             {"referral_confirmed", label ? 1 : 0},
             {"referral_signal", referral_signal},
             {"claim_amount", 500.0 + static_cast<double>(index % 20) * 250.0},
             {"prior_claims", static_cast<double>(index % 5)},
             {"document", document},
             {"document_labels",
              {{"claim_number", claim_number},
               {"loss_date", loss_date},
               {"loss_amount", loss_amount}}},
             {"observed_at", "2026-08-20T00:00:00.000Z"},
             {"fnol_complete", true},
             {"coverage_confirmed", true},
             {"identity_verified", true},
             {"document_confidence_clear", true},
             {"fairness_review_passed", true},
             {"catastrophe_override", false},
             {"manual_only", false}});
    }
    table->add_row(
        {{"claim_id", 9999},
         {"record_id", 9999},
         {"source", "fnol-fixture"},
         {"name", "Future claim"},
         {"referral_confirmed", 1},
         {"referral_signal", 1.0},
         {"claim_amount", 9999.0},
         {"prior_claims", 9.0},
         {"document", "claim_number: FUTURE\nloss_date: 2026-10-01\nloss_amount: 9999"},
         {"document_labels",
          {{"claim_number", "FUTURE"},
           {"loss_date", "2026-10-01"},
           {"loss_amount", "9999"}}},
         {"observed_at", "2026-10-01T00:00:00.000Z"},
         {"fnol_complete", true},
         {"coverage_confirmed", true},
         {"identity_verified", true},
         {"document_confidence_clear", true},
         {"fairness_review_passed", true},
         {"catastrophe_override", false},
         {"manual_only", false}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json claims_configuration() {
    return {
        {"snapshot",
         {{"database", "claims"},
          {"source_table", "fnol"},
          {"target_table", "fnol_snapshot"},
          {"entity_column", "claim_id"},
          {"time_column", "observed_at"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "referral_confirmed"},
          {"feature_columns",
           {"referral_signal", "claim_amount", "prior_claims"}}}},
        {"delivery",
         {{"endpoint", "https://claims.example.test/tasks"},
          {"owner", "claims-triage"}}},
        {"roi_model",
         {{"value_per_outcome", 100.0},
          {"investment", 200.0},
          {"currency", "USD"}}}};
}

journey::Journey claims_journey(const nlohmann::json& foundation) {
    journey::Journey flow{"claims-solution", {}};
    flow.steps = {
        {"read executable claims defaults", "get_solution",
         {{"solution", "S-12"}},
         {.checks = {
              journey::equals("/solution/runnable", true),
              journey::equals("/solution/minimal_template", false),
              journey::equals(
                  "/solution/policy_template/actions/denial_recommendation/autonomy",
                  "A0"),
              journey::equals("/solution/measurement/design", "holdout"),
              journey::size_equals("/solution/missing_components", 0)}}},
        {"create the claims project", "create_project",
         {{"name", "claims triage"}}},
        {"create the composed claims solution", "create_solution",
         {{"project", "claims triage"},
          {"solution", "S-12"},
          {"level", 1},
          {"sources",
           {{"source", "claims"},
            {"table", "fnol"},
            {"M-11",
             {{"document_column", "document"},
              {"document_id_column", "entity_id"},
              {"labels_column", "document_labels"},
              {"fields",
               {"claim_number", "loss_date", "loss_amount"}},
              {"review_threshold", 0.8}}}}},
          {"foundation", foundation}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"claims_graph", "/graph"},
          {"claims_policy", "/policy_id"},
          {"claims_experiment", "/experiment_id"},
          {"claims_readout", "/readout_id"}}},
        {"read the claims policy", "get_policy",
         {{"policy_id", "$claims_policy"}},
         {.checks = {
              journey::equals("/current_version", 1),
              journey::equals(
                  "/versions/0/config/actions/denial_recommendation/autonomy",
                  "A0"),
              journey::equals("/versions/0/config/tiers/0/action",
                              "siu_referral"),
              journey::equals("/versions/0/config/tiers/1/action",
                              "adjuster_assignment"),
              journey::equals("/versions/0/config/tiers/2/action",
                              "fast_track")}}},
        {"read the claims holdout", "get_experiment",
         {{"experiment", "$claims_experiment"}},
         {.checks = {
              journey::equals("/experiment/allocations/0/fraction", 0.1)}}},
        {"run claims triage", "run_solution",
         {{"project", "claims triage"},
          {"graph", "$claims_graph"},
          {"delivery_token", "claims-fixture-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"claims_run", "/run_id"}}},
        {"read the completed claims Run", "get_run",
         {{"run_id", "$claims_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/lineage/steps", 9)}},
         {}, journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read claim assignments", "get_experiment",
         {{"experiment", "$claims_experiment"}},
         {.checks = {
              journey::at_least("/sizes/assignments", 100),
              journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read claim action evidence", "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}};
    return flow;
}

journey::Journey planted_claim_outcomes(const nlohmann::json& entries,
                                        const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") == "action") {
            ++counts[entry.at("cell")];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"claims-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered =
            static_cast<double>(positions[cell]++) -
            (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double response =
            0.25 + 0.1 * centered / counts[cell] +
            (holdout ? 0.0 : 0.2);
        outcomes.steps.push_back(
            {"record claim outcome", "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "referral_confirmed"},
              {"value", response},
              {"currency", "USD"},
              {"source", "connector"},
              {"occurred_at", entry.at("action_at")}},
             {}, {{"claims_outcome_id", "/id"}}});
        outcomes.steps.push_back(
            {"read claim outcome", "list_ledger", nlohmann::json::object(),
             {.checks = {journey::any_element(
                  "/entries", {{"/id", "$claims_outcome_id"}})}}});
    }
    outcomes.steps.push_back(
        {"read measured claims effect", "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/incrementality/claimable", true),
              journey::predicate(
                  "planted claims effect is inside its confidence interval",
                  [](const auto& value) {
                      const auto& estimate =
                          value.at("incrementality").at("combined");
                      return std::abs(
                                 estimate.at("effect")
                                         .template get<double>() -
                                 0.2) < 1e-8 &&
                             estimate.at("confidence_interval")
                                     .at("lower")
                                     .template get<double>() <= 0.2 &&
                             estimate.at("confidence_interval")
                                     .at("upper")
                                     .template get<double>() >= 0.2;
                  })}}});
    return outcomes;
}

TEST_F(SolutionJourney, FoundationProtectsHoldoutAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_retention_data(node);
    const auto flow = retention_journey(retention_configuration());
    const auto report = journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();
    const auto project = node.find_project("retention");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-1 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph().find_node("solution_manifest")->attributes.at("foundation");
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"), std::string::npos);
    zetta::solutions::DecisionPolicyStore policies(*node.replication(), &node, &node.experiment_manager());
    const auto experiment = node.experiment_manager().report(binding.at("experiment_id").get<std::string>());
    ASSERT_TRUE(experiment);
    std::size_t protected_rows = 0;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        EXPECT_FALSE(decision.tier.empty());
        EXPECT_FALSE(decision.cell.empty());
        const auto read = policies.get_decision(decision.id);
        ASSERT_TRUE(read);
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        }
    }
    EXPECT_GT(protected_rows, 0u);
    EXPECT_FALSE(delivery.requests.empty());
    std::size_t reports = 0;
    for (const auto& request : delivery.requests) {
        if (request.at("action") == "publish_driver_report") {
            ++reports;
            EXPECT_TRUE(request.at("payload").contains("driver_report"));
            continue;
        }
        const auto decision = policies.get_decision(request.at("payload").at("decision_id"));
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
        EXPECT_FALSE(request.at("sla_due_at").get<std::string>().empty());
        EXPECT_LE(request.at("payload").at("reason_codes").size(), 3u);
        EXPECT_TRUE(request.at("payload").contains("decision_cell"));
    }
    EXPECT_EQ(reports, 1u);
    const auto results = node.find_database("solution_results");
    ASSERT_TRUE(results);
    ASSERT_EQ(results->tables().size(), 1u);
    const auto decisions_read = call("sql", {
        {"statement", "SELECT * FROM solution_results." + results->tables().front()->name() + ";"}});
    ASSERT_TRUE(decisions_read.ok) << decisions_read.error.message;
    EXPECT_EQ(decisions_read.value.at("rows").size(), 240u);
    EXPECT_EQ(node.find_database("datasets")->find_table("retention_snapshot")->store().size(), 240u);
    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 4u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value());
    }

    const auto ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"), std::string::npos);
    const auto outcomes = planted_retention_outcomes(ledger.value.at("entries"), binding);
    const auto measured = journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const auto readout = call("get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2)
                          << readout.value.at("correctness_certificate").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

TEST_F(SolutionJourney,
       ClaimsTriageProtectsHoldoutAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_claims_data(node);
    const auto flow = claims_journey(claims_configuration());
    const auto report = journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("claims triage");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-12 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding =
        graph->graph()
            .find_node("solution_manifest")
            ->attributes.at("foundation");
    EXPECT_EQ(binding.at("decision_source").at("methodology"), "M-1");
    EXPECT_EQ(binding.at("decision_source").at("output_name"),
              "Publish claim referral scores");
    EXPECT_EQ(binding.at("decision_source").at("entity_column"),
              "claim_id");
    EXPECT_EQ(binding.at("decision_source").at("score_column"),
              "referral_score");
    EXPECT_EQ(graph->record().dump().find("claims-fixture-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    std::set<std::string> selected_paths;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        EXPECT_NE(decision.action, "denial_recommendation");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
            continue;
        }
        EXPECT_TRUE(decision.actionable);
        EXPECT_EQ(decision.autonomy, "A2");
        selected_paths.insert(decision.action);
    }
    EXPECT_GT(protected_rows, 0u);
    EXPECT_FALSE(selected_paths.empty());
    for (const std::string& path : selected_paths) {
        EXPECT_TRUE(path == "fast_track" ||
                    path == "adjuster_assignment" ||
                    path == "siu_referral")
            << path;
    }

    EXPECT_FALSE(delivery.requests.empty());
    for (const auto& request : delivery.requests) {
        const auto& payload = request.at("payload");
        EXPECT_EQ(payload.at("human_denial_only"), true);
        EXPECT_EQ(payload.at("document_confidence_clear"), true);
        EXPECT_EQ(payload.at("fairness_review_passed"), true);
        EXPECT_NE(payload.at("assigned_path"), "denial_recommendation");
        const auto decision =
            policies.get_decision(payload.at("decision_id"));
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
        EXPECT_EQ(payload.at("assigned_path"), decision->action);
        EXPECT_EQ(payload.at("claim_id"), decision->entity_id);
    }

    const auto claim_results = node.find_database("claims_results");
    ASSERT_TRUE(claim_results);
    ASSERT_EQ(claim_results->tables().size(), 1u);
    const ApiResult scores = call(
        "sql",
        {{"statement",
          "SELECT * FROM claims_results." +
              claim_results->tables().front()->name() + ";"}});
    ASSERT_TRUE(scores.ok) << scores.error.message;
    ASSERT_EQ(scores.value.at("rows").size(), 240u);
    EXPECT_TRUE(scores.value.at("rows").front().contains("claim_id"));
    EXPECT_TRUE(scores.value.at("rows").front().contains("referral_score"));
    EXPECT_EQ(node.find_database("claims")
                  ->find_table("fnol_snapshot")
                  ->store()
                  .size(),
              240u);

    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    ASSERT_EQ(monitor->value().guardrails.size(), 5u);
    for (const auto& guardrail : monitor->value().guardrails) {
        ASSERT_TRUE(guardrail.value.has_value()) << guardrail.bound.name;
        if (guardrail.bound.name == "automated_denials" ||
            guardrail.bound.name == "low_confidence_path_assignments" ||
            guardrail.bound.name == "fairness_review_failures") {
            EXPECT_EQ(*guardrail.value, 0.0) << guardrail.bound.name;
        }
    }

    const auto ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("claims-fixture-token"),
              std::string::npos);
    const auto outcomes =
        planted_claim_outcomes(ledger.value.at("entries"), binding);
    const auto measured = journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const auto readout =
            call("get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2)
                          << readout.value.at("correctness_certificate")
                                 .dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

TEST_F(SolutionJourney,
       CustomerLifetimeValueDrivesActionsAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_lifetime_value_data(node);
    const journey::Journey flow =
        lifetime_value_journey(lifetime_value_configuration());
    const journey::JourneyReport report =
        journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("customer lifetime value");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-3 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(binding.at("decision_source").at("methodology"), "M-1");
    EXPECT_EQ(binding.at("decision_source").at("score_column"),
              "predicted_value");
    EXPECT_EQ(binding.at("decision_source").at("output_name"),
              "Write lifetime value predictions");
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    std::size_t audience_decisions = 0;
    std::size_t budget_approvals = 0;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        } else if (decision.action == "audience_sync" &&
                   decision.actionable) {
            ++audience_decisions;
        } else if (decision.action == "budget_reallocation") {
            ++budget_approvals;
            EXPECT_EQ(decision.status, "pending_approval");
            EXPECT_FALSE(decision.actionable);
        }
    }
    EXPECT_GT(protected_rows, 0u);
    EXPECT_GT(audience_decisions, 0u);
    EXPECT_GT(budget_approvals, 0u);

    std::size_t delivered_audiences = 0;
    std::size_t reports = 0;
    for (const auto& request : delivery.requests) {
        if (request.at("action") == "publish_driver_report") {
            ++reports;
            EXPECT_EQ(request.at("payload").at("title"),
                      "Customer lifetime value allocation report");
            EXPECT_EQ(request.at("payload")
                          .at("driver_report")
                          .at("value_horizon_days"),
                      365);
            continue;
        }
        EXPECT_EQ(request.at("payload").at("action"), "audience_sync");
        EXPECT_TRUE(request.at("payload").contains("predicted_value"));
        EXPECT_EQ(request.at("payload").at("value_horizon_days"), 365);
        ++delivered_audiences;
    }
    EXPECT_EQ(delivered_audiences, audience_decisions);
    EXPECT_EQ(reports, 1u);

    const auto results = node.find_database("solution_results");
    ASSERT_TRUE(results);
    ASSERT_EQ(results->tables().size(), 1u);
    const ApiResult decisions_read = call(
        "sql", {{"statement", "SELECT * FROM solution_results." +
                                  results->tables().front()->name() + ";"}});
    ASSERT_TRUE(decisions_read.ok) << decisions_read.error.message;
    EXPECT_EQ(decisions_read.value.at("rows").size(), 240u);
    for (const auto& row : decisions_read.value.at("rows")) {
        EXPECT_TRUE(row.at("decision_cell").contains("value_band"));
    }

    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 5u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value());
    }

    const ApiResult ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"),
              std::string::npos);
    const journey::Journey outcomes = planted_lifetime_value_outcomes(
        ledger.value.at("entries"), binding);
    const journey::JourneyReport measured =
        journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const ApiResult readout =
            call("get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}


TEST_F(SolutionJourney,
       LeadScoringRoutesCapacityAwareQueuesAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_lead_scoring_data(node);
    journey::Journey flow{"lead-scoring-solution", {}};
    flow.steps = {
        {"read S-2 defaults",
         "get_solution",
         {{"solution", "S-2"}},
         {.checks = {journey::equals("/solution/runnable", true),
                     journey::equals("/solution/minimal_template", false),
                     journey::equals(
                         "/solution/measurement/outcome/name", "converted")}}},
        {"create lead scoring project",
         "create_project",
         {{"name", "lead scoring"}}},
        {"create S-2 Pipeline",
         "create_solution",
         {{"project", "lead scoring"},
          {"solution", "S-2"},
          {"level", 1},
          {"sources",
           {{"source", "datasets"},
            {"table", "leads"},
            {"M-3",
             {{"entity_column", "entity_id"},
              {"label_column", "label"},
              {"feature_columns", {"engagement"}}}}}},
          {"foundation", lead_scoring_configuration()}},
         {.checks = {journey::equals("/preview", false),
                     journey::size_equals("/methodologies", 3),
                     journey::size_equals("/missing_components", 0)}},
         {{"lead_graph", "/graph"},
          {"lead_experiment", "/experiment_id"},
          {"lead_readout", "/readout_id"}}},
        {"run S-2 Pipeline",
         "run_solution",
         {{"project", "lead scoring"},
          {"graph", "$lead_graph"},
          {"delivery_token", "fixture-only-token"}},
         {.checks = {journey::equals("/engine", "solution"),
                     journey::equals("/preview", false)}},
         {{"lead_run", "/run_id"}}},
        {"read completed lead scoring Run",
         "get_run",
         {{"run_id", "$lead_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/lineage/steps", 9)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read protected routing assignments",
         "get_experiment",
         {{"experiment", "$lead_experiment"}},
         {.checks = {
              journey::at_least("/sizes/assignments", 100),
              journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read lead action ledger",
         "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}};

    const auto report = journey::run_journey(flow, caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("lead scoring");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-2 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        }
    }
    EXPECT_GT(protected_rows, 0u);
    ASSERT_FALSE(delivery.requests.empty());
    for (const auto& request : delivery.requests) {
        ASSERT_TRUE(request.at("payload").contains("routing_queue"));
        EXPECT_FALSE(
            request.at("payload").at("territory").get<std::string>().empty());
        EXPECT_TRUE(
            request.at("payload").at("rep_capacity_available").get<bool>());
        EXPECT_FALSE(
            request.at("payload").at("queue_owner").get<std::string>().empty());
        EXPECT_TRUE(request.at("payload")
                        .at("decision_cell")
                        .contains("conversion_band"));
        EXPECT_FALSE(request.at("sla_due_at").get<std::string>().empty());
        const auto decision = policies.get_decision(
            request.at("payload").at("decision_id"));
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
    }

    const auto snapshot =
        node.find_database("datasets")->find_table("lead_scoring_snapshot");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->store().size(), 240u);
    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 4u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value());
    }

    const auto ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"),
              std::string::npos);
    const auto outcomes =
        planted_conversion_outcomes(ledger.value.at("entries"), binding);
    const auto measured = journey::run_journey(outcomes, caller(), pump());
    if (!measured.ok) {
        const auto readout = call(
            "get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

TEST_F(SolutionJourney,
       LevelThreeTargetsPersuadablesAndLeavesSleepingDogsAlone) {
    SolutionDeliveryBoundary delivery;
    land_retention_data(node);
    ASSERT_TRUE(call("create_project", {{"name", "uplift retention"}}).ok);
    const ApiResult created = call(
        "create_solution",
        {{"project", "uplift retention"},
         {"solution", "S-1"},
         {"level", 3},
         {"sources",
          {{"source", "datasets"},
           {"table", "customers"},
           {"M-3",
            {{"entity_column", "entity_id"},
             {"label_column", "label"},
             {"feature_columns", {"feature", "uplift_segment"}}}},
           {"M-6",
            {{"entity", "entity_id"},
             {"treatment", "treatment"},
             {"outcome", "outcome"},
             {"features", {"feature", "uplift_segment"}},
             {"as_of", "2026-09-01T00:00:00Z"},
             {"feature_window_end", "2026-09-01T00:00:00Z"},
             {"label_window_start", "2026-09-08T00:00:00Z"},
             {"blackout_gap_seconds", 604800},
             {"treatment_time", "2026-09-08T00:00:00Z"}}}}},
         {"foundation", retention_configuration()}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value.at("methodologies").size(), 5u);

    const ApiResult started = call(
        "run_solution",
        {{"project", "uplift retention"},
         {"graph", created.value.at("graph")},
         {"delivery_token", "fixture-only-token"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    ApiResult status;
    for (int attempt = 0; attempt < 5000; ++attempt) {
        node.tick();
        background.prune();
        status = call("get_run", {{"run_id", started.value.at("run_id")}});
        if (status.ok && status.value.at("status") != "running") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(status.ok) << status.error.message;
    if (status.value.at("status") != "succeeded") {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_EQ(status.value.at("status"), "succeeded")
        << status.value.dump(2);

    std::string decision_table;
    for (const auto& output : status.value.at("outputs")) {
        if (output.at("name") == "solution decisions") {
            decision_table = output.at("ref").get<std::string>();
        }
    }
    ASSERT_FALSE(decision_table.empty());
    const ApiResult decisions = call(
        "sql", {{"statement", "SELECT * FROM " + decision_table + ";"}});
    ASSERT_TRUE(decisions.ok) << decisions.error.message;
    std::size_t persuaded = 0;
    std::size_t sleeping_dogs = 0;
    for (const auto& row : decisions.value.at("rows")) {
        ASSERT_TRUE(row.contains("estimated_uplift"));
        ASSERT_TRUE(row.contains("uplift_recommended"));
        if (row.at("estimated_uplift").get<double>() <= 0.0) {
            ++sleeping_dogs;
            EXPECT_FALSE(row.at("actionable").get<bool>());
            if (row.at("holdout").get<bool>()) {
                EXPECT_EQ(row.at("action"), "");
            } else {
                EXPECT_EQ(row.at("action"), "no_contact");
            }
        } else if (row.at("uplift_recommended").get<bool>() &&
                   row.at("actionable").get<bool>()) {
            ++persuaded;
        }
    }
    EXPECT_GT(sleeping_dogs, 0u);
    EXPECT_GT(persuaded, 0u);
}

void land_collections_data(Node& node) {
    const auto database = node.create_database("collections_data");
    ASSERT_TRUE(database);
    const auto table = database->create_table("receivables");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 240; ++index) {
        const double recovery_signal = static_cast<double>(index % 10);
        const int rotating_outcome = (index * 7 + index / 10) % 10;
        const int promise_to_pay =
            rotating_outcome < 2 + static_cast<int>(recovery_signal / 2.0)
                ? 1
                : 0;
        table->add_row(
            {{"entity", index + 1},
             {"record_id", index + 1},
             {"source", "fixture"},
             {"name", "Receivable " + std::to_string(index + 1)},
             {"promise_to_pay", promise_to_pay},
             {"recovery_signal", recovery_signal},
             {"balance", 100.0 + index},
             {"promise_to_pay_at", "2026-08-01T00:00:00.000Z"},
             {"collections_eligible", index != 7},
             {"regulatory_contact_allowed", index != 6},
             {"within_contact_hours", index != 8},
             {"vulnerable", index == 5},
             {"open_complaint", false},
             {"recent_actions", 0},
             {"owner", "collections-team"}});
    }
    table->add_row(
        {{"entity", 9999},
         {"record_id", 9999},
         {"source", "fixture"},
         {"name", "Future promise update"},
         {"promise_to_pay", 1},
         {"recovery_signal", 99.0},
         {"balance", 999.0},
         {"promise_to_pay_at", "2026-10-01T00:00:00.000Z"},
         {"collections_eligible", true},
         {"regulatory_contact_allowed", true},
         {"within_contact_hours", true},
         {"vulnerable", false},
         {"open_complaint", false},
         {"recent_actions", 0},
         {"owner", "collections-team"}});
    const auto written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
}

nlohmann::json collections_configuration() {
    return {
        {"snapshot",
         {{"database", "collections_data"},
          {"source_table", "receivables"},
          {"target_table", "collections_snapshot"},
          {"entity_column", "entity"},
          {"time_column", "promise_to_pay_at"},
          {"as_of", "2026-09-01T00:00:00.000Z"}}},
        {"dataset",
         {{"entity_column", "entity_id"},
          {"label_column", "promise_to_pay"},
          {"feature_columns", {"recovery_signal", "balance"}}}},
        {"delivery",
         {{"endpoint", "https://dialer.example.test/queues"},
          {"owner", "collections-team"}}},
        {"roi_model",
         {{"value_per_outcome", 125.0},
          {"investment", 300.0},
          {"currency", "USD"}}}};
}

journey::Journey collections_journey() {
    journey::Journey flow{"collections-prioritization", {}};
    flow.steps = {
        {"read S-4 executable defaults", "get_solution",
         {{"solution", "S-4"}},
         {.checks = {
              journey::equals("/solution/runnable", true),
              journey::equals("/solution/minimal_template", false),
              journey::equals(
                  "/solution/measurement/design", "champion_challenger"),
              journey::size_equals("/solution/action_patterns", 3)}}},
        {"create the collections project", "create_project",
         {{"name", "collections prioritization"}}},
        {"create the composed collections solution", "create_solution",
         {{"project", "collections prioritization"},
          {"solution", "S-4"},
          {"level", 1},
          {"sources",
           {{"source", "collections_data"},
            {"table", "receivables"},
            {"M-3",
             {{"entity_column", "entity_id"},
              {"label_column", "promise_to_pay"},
              {"feature_columns", {"recovery_signal", "balance"}}}}}},
          {"foundation", collections_configuration()}},
         {.checks = {
              journey::equals("/preview", false),
              journey::size_equals("/methodologies", 3),
              journey::size_equals("/missing_components", 0)}},
         {{"collections_graph", "/graph"},
          {"collections_policy", "/policy_id"},
          {"collections_experiment", "/experiment_id"},
          {"collections_readout", "/readout_id"}}},
        {"read the collections policy", "get_policy",
         {{"policy_id", "$collections_policy"}},
         {.checks = {
              journey::equals("/versions/0/config/contact_cap", 6),
              journey::equals(
                  "/versions/0/config/frequency_cap/window_seconds", 604800),
              journey::equals(
                  "/versions/0/config/tiers/0/action", "dialer_queue"),
              journey::equals(
                  "/versions/0/config/tiers/1/action", "digital_treatment")}}},
        {"run collections prioritization", "run_solution",
         {{"project", "collections prioritization"},
          {"graph", "$collections_graph"},
          {"delivery_token", "fixture-only-token"}},
         {.checks = {
              journey::equals("/engine", "solution"),
              journey::equals("/preview", false)}},
         {{"collections_run", "/run_id"}}},
        {"read the completed collections Run", "get_run",
         {{"run_id", "$collections_run"}},
         {.checks = {
              journey::equals("/status", "succeeded"),
              journey::size_equals("/lineage/steps", 9)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read protected collections assignments", "get_experiment",
         {{"experiment", "$collections_experiment"}},
         {.checks = {
              journey::at_least("/sizes/assignments", 100),
              journey::at_least("/sizes/holdout_assignments", 1)}}},
        {"read collections ledger actions", "list_ledger",
         nlohmann::json::object(),
         {.checks = {journey::size_at_least("/entries", 100)}}}};
    return flow;
}

journey::Journey planted_collections_outcomes(
    const nlohmann::json& entries,
    const nlohmann::json& binding) {
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : entries) {
        if (entry.at("kind") == "action") {
            ++counts[entry.at("cell")];
        }
    }
    std::map<std::string, std::size_t> positions;
    journey::Journey outcomes{"collections-planted-outcomes", {}};
    for (const auto& entry : entries) {
        if (entry.at("kind") != "action") {
            continue;
        }
        const std::string cell = entry.at("cell");
        const bool holdout = cell.ends_with("/holdout");
        if (holdout) {
            EXPECT_EQ(entry.at("action"), "no_contact");
        }
        const double centered =
            static_cast<double>(positions[cell]++) -
            (static_cast<double>(counts[cell]) - 1.0) / 2.0;
        const double recovery =
            0.25 + 0.1 * centered / counts[cell] +
            (holdout ? 0.0 : 0.15);
        outcomes.steps.push_back(
            {"record net recovery", "record_outcome",
             {{"action_id", entry.at("id")},
              {"outcome", "net_recovery"},
              {"value", recovery},
              {"currency", "USD"},
              {"source", "connector"},
              {"occurred_at", entry.at("action_at")}}});
    }
    outcomes.steps.push_back(
        {"read measured net-recovery effect", "get_roi_readout",
         {{"solution", binding.at("scope")}},
         {.checks = {
              journey::equals("/incrementality/claimable", true),
              journey::predicate(
                  "planted net-recovery effect is inside its confidence interval",
                  [](const auto& value) {
                      const auto& estimate =
                          value.at("incrementality").at("combined");
                      return std::abs(
                                 estimate.at("effect").template get<double>() -
                                 0.15) < 1e-8 &&
                             estimate.at("confidence_interval")
                                     .at("lower")
                                     .template get<double>() <= 0.15 &&
                             estimate.at("confidence_interval")
                                     .at("upper")
                                     .template get<double>() >= 0.15;
                  })}}});
    return outcomes;
}

TEST_F(SolutionJourney,
       CollectionsPrioritizationProtectsHoldoutAndRecoversPlantedEffect) {
    SolutionDeliveryBoundary delivery;
    land_collections_data(node);
    const auto report =
        journey::run_journey(collections_journey(), caller(), pump());
    if (!report.ok) {
        for (const auto& run : node.runs()) {
            ADD_FAILURE() << run->metadata().dump(2);
        }
    }
    ASSERT_TRUE(report.ok) << report.summary();

    const auto project = node.find_project("collections prioritization");
    ASSERT_TRUE(project);
    const auto graph = project->find_graph("S-4 CL-1");
    ASSERT_TRUE(graph);
    const auto& binding = graph->graph()
                              .find_node("solution_manifest")
                              ->attributes.at("foundation");
    EXPECT_EQ(binding.at("decision_source").at("output_name"),
              "Prioritize collections treatments");
    EXPECT_EQ(binding.at("decision_source").at("entity_column"), "receivable");
    EXPECT_EQ(binding.at("decision_source").at("score_column"),
              "recovery_score");
    bool found_collections_output = false;
    for (const auto& run : node.runs()) {
        for (const auto& output : run->outputs()) {
            if (output.ref.starts_with(
                    "collections_results.recovery_priority_")) {
                found_collections_output = true;
            }
        }
    }
    EXPECT_TRUE(found_collections_output);
    EXPECT_EQ(graph->record().dump().find("fixture-only-token"),
              std::string::npos);

    zetta::solutions::DecisionPolicyStore policies(
        *node.replication(), &node, &node.experiment_manager());
    std::size_t protected_rows = 0;
    const std::set<std::string> blocked = {"6", "7", "8", "9"};
    for (const auto& decision : policies.decisions()) {
        EXPECT_NE(decision.entity_id, "9999");
        if (decision.holdout) {
            ++protected_rows;
            EXPECT_FALSE(decision.actionable);
            EXPECT_TRUE(decision.action.empty());
        }
        if (blocked.contains(decision.entity_id)) {
            EXPECT_FALSE(decision.actionable) << decision.entity_id;
        }
    }
    EXPECT_GT(protected_rows, 0u);
    EXPECT_FALSE(delivery.requests.empty());
    std::set<std::string> delivered_actions;
    for (const auto& request : delivery.requests) {
        const std::string action =
            request.at("payload").at("action").get<std::string>();
        delivered_actions.insert(action);
        EXPECT_TRUE(action == "dialer_queue" ||
                    action == "digital_treatment");
        const auto decision = policies.get_decision(
            request.at("payload").at("decision_id"));
        ASSERT_TRUE(decision);
        EXPECT_TRUE(decision->actionable);
        EXPECT_FALSE(decision->holdout);
    }
    EXPECT_EQ(delivered_actions,
              (std::set<std::string>{"dialer_queue",
                                     "digital_treatment"}));

    const auto monitor = node.find_monitor(binding.at("monitor_id"));
    ASSERT_TRUE(monitor);
    EXPECT_EQ(monitor->value().guardrails.size(), 5u);
    for (const auto& guardrail : monitor->value().guardrails) {
        EXPECT_TRUE(guardrail.value.has_value()) << guardrail.bound.name;
    }

    const ApiResult ledger = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(ledger.ok) << ledger.error.message;
    EXPECT_EQ(ledger.value.dump().find("fixture-only-token"),
              std::string::npos);
    const auto measured = journey::run_journey(
        planted_collections_outcomes(ledger.value.at("entries"), binding),
        caller(),
        pump());
    if (!measured.ok) {
        const ApiResult readout =
            call("get_roi_readout", {{"solution", binding.at("scope")}});
        if (readout.ok) {
            ADD_FAILURE() << readout.value.at("incrementality").dump(2);
        }
    }
    EXPECT_TRUE(measured.ok) << measured.summary();
}

void append_speech_u16(std::string &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value & 0xff));
  bytes.push_back(static_cast<char>((value >> 8) & 0xff));
}

void append_speech_u32(std::string &bytes, std::uint32_t value) {
  for (int offset = 0; offset < 32; offset += 8) {
    bytes.push_back(static_cast<char>((value >> offset) & 0xff));
  }
}

std::string speech_test_wav() {
  constexpr std::uint32_t sample_rate = 16000;
  constexpr std::size_t samples = 8000;
  std::string bytes = "RIFF";
  append_speech_u32(bytes, 36 + samples * 2);
  bytes += "WAVEfmt ";
  append_speech_u32(bytes, 16);
  append_speech_u16(bytes, 1);
  append_speech_u16(bytes, 1);
  append_speech_u32(bytes, sample_rate);
  append_speech_u32(bytes, sample_rate * 2);
  append_speech_u16(bytes, 2);
  append_speech_u16(bytes, 16);
  bytes += "data";
  append_speech_u32(bytes, samples * 2);
  for (std::size_t index = 0; index < samples; ++index) {
    const double value =
        0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * index / sample_rate);
    append_speech_u16(bytes, static_cast<std::uint16_t>(
                                 static_cast<std::int16_t>(value * 32767.0)));
  }
  return bytes;
}

} // namespace

TEST_F(Uc1Journey, FeaturePlanRefusesLongMeasurementsInEveryRunnableMethodology) {
    auto database = node.create_database("feature_plan");
    ASSERT_TRUE(database);
    auto table = database->create_table("long_measurements");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"id", index + 1}, {"entity", index + 1}, {"entity_id", index + 1},
            {"label", index % 2}, {"response", index % 2}, {"treatment", index % 3 == 0},
            {"outcome", index % 2}, {"metric", "temperature"}, {"value", index % 17},
            {"time", index}, {"timestamp", index}, {"feature", index % 7},
            {"src", index + 1}, {"dst", index + 2}, {"weight", 1.0},
            {"query_id", index / 4}, {"relevance", index % 3},
            {"duration", 30}, {"feature_time", index},
            {"action_id", index % 3}, {"action_value", 10.0}, {"action_cost", 1.0},
            {"eligible", true}, {"contacts", 0}, {"contact_cap", 3},
            {"interaction_count", 1}, {"interaction_time", index}, {"logging_propensity", 0.5},
            {"source", "fixture"}, {"record_id", index + 1}, {"name", "Example Person"},
            {"document_id", "document-" + std::to_string(index)}, {"document", "Total: 12.00"},
            {"labels", nlohmann::json::object()}, {"image", "unused-image"},
            {"recording", "recording-" + std::to_string(index)},
            {"audio", "unused-audio"}, {"consent", true}, {"reference_text", "hello"},
            {"decision_id", index + 1}, {"arm", "control"}, {"reward", index % 2},
            {"row_id", index + 1}, {"variable", "item"}, {"objective", index % 7},
            {"integer", true}, {"scenario", "base"}, {"group", "observations"},
            {"event_time", index}, {"label_time", index + 0.5}});
    }
    const auto written = zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
    const auto project = node.create_project("feature refusals");
    ASSERT_TRUE(project);
    ASSERT_EQ(zetta::ml::methodologies().size(), 21u);
    for (const auto& methodology : zetta::ml::methodologies()) {
        SCOPED_TRACE(methodology.id);
        ASSERT_TRUE(methodology.runnable);
        zetta::ml::ShapeRequest shape;
        shape.hints = {{"source", "feature_plan"}, {"table", "long_measurements"}};
        if (methodology.id == "M-6") {
            shape.hints["treatment"] = "treatment";
            shape.hints["outcome"] = "outcome";
        }
        if (methodology.id == "M-9") {
            shape.hints["group"] = "query_id";
            shape.hints["label"] = "relevance";
            shape.hints["features"] = {"feature"};
        }
        if (methodology.id == "M-13") {
            shape.hints = {{"database", "feature_plan"}, {"table", "long_measurements"},
                           {"backbone_model", "not-read-before-plan-check"}};
        }
        auto graph = methodology.build_shape(shape);
        for (const auto& step : graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
            step->attributes["source"] = "feature_plan";
            step->attributes["table"] = "long_measurements";
        }
        const auto stored = project->create_graph(methodology.id, std::move(graph));
        ASSERT_TRUE(stored);
        ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
        zetta::ml::RunRequest request{&context, project.get(), stored.get(), nlohmann::json::object()};
        const auto result = methodology.run(request);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "invalid_input") << result.error.message;
        EXPECT_NE(result.error.message.find("transform_required"), std::string::npos)
            << result.error.message;
        EXPECT_NE(result.error.message.find("pivot_long_to_wide"), std::string::npos)
            << result.error.message;
    }
    EXPECT_TRUE(node.runs().empty());
    EXPECT_TRUE(node.models().empty());
}

TEST_F(Uc1Journey, FeaturePlanRefusesSelectedLeakageAndPostTreatmentAtRun) {
    const auto database = node.create_database("feature_checks");
    const auto table = database->create_table("entities");
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"id", index + 1}, {"label", index % 2},
                       {"label_copy", index % 2}, {"feature", index % 7},
                       {"treatment", index % 3 == 0}, {"post_treatment_score", index % 11}});
    }
    const auto project = node.create_project("unsafe features");
    for (const auto* id : {"M-1", "M-3", "M-6"}) {
        SCOPED_TRACE(id);
        const auto* methodology = zetta::ml::find_methodology(id);
        const std::string unsafe = std::string(id) == "M-6" ? "post_treatment_score"
                                  : std::string(id) == "M-3" ? "label" : "label_copy";
        zetta::ml::ShapeRequest shape;
        shape.hints = {{"source", "feature_checks"}, {"table", "entities"}};
        if (std::string(id) == "M-3") {
            shape.hints["entity_column"] = "id";
            shape.hints["label_column"] = "label";
            shape.hints["feature_columns"] = {"feature", unsafe};
        }
        if (std::string(id) == "M-6") {
            shape.hints["entity"] = "id";
            shape.hints["outcome"] = "label";
        }
        const auto stored = project->create_graph(id, methodology->build_shape(shape));
        ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
        zetta::ml::RunRequest request{&context, project.get(), stored.get(),
            {{"dataset_path", "feature_checks.entities"},
             {"dataset", {{"entity_column", "id"}, {"label_column", "label"},
                           {"feature_columns", {"feature", unsafe}}}}}};
        const auto result = methodology->run(request);
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error.message.find(unsafe), std::string::npos) << result.error.message;
    }
    EXPECT_TRUE(node.runs().empty());
}

TEST_F(Uc1Journey, FeaturePlanRoundFourRefusesSelectedLeakageBeforeCreatingRun) {
    const auto database = node.create_database("round_four_safety");
    ASSERT_TRUE(database);
    const auto table = database->create_table("entities");
    ASSERT_TRUE(table);
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"entity", index + 1}, {"label", index % 2},
                       {"label_copy", index % 2}, {"signal", index % 7},
                       {"event_time", index}, {"label_time", index + 0.5}});
    }
    const auto project = node.create_project("round four safety");
    ASSERT_TRUE(project);
    for (const auto* id : {"M-17", "M-18", "M-19", "M-20", "M-21"}) {
        SCOPED_TRACE(id);
        const auto* methodology = zetta::ml::find_methodology(id);
        ASSERT_NE(methodology, nullptr);
        zetta::ml::ShapeRequest shape;
        shape.hints = {{"source", "round_four_safety"}, {"database", "round_four_safety"},
                       {"table", "entities"}, {"entity_column", "entity"},
                       {"label_column", "label"}, {"group_column", "label_copy"},
                       {"time_column", "event_time"}, {"feature_columns", {"label_copy"}}};
        if (std::string(id) != "M-21") {
            shape.hints.erase("time_column");
        }
        const auto graph = project->create_graph(id, methodology->build_shape(shape));
        ASSERT_TRUE(graph);
        nlohmann::json input = {{"dataset", {{"entity_column", "entity"},
            {"label_column", "label"}, {"feature_columns", {"label_copy"}}}}};
        if (std::string(id) == "M-19") {
            input["label_efficiency"] = {{"labelling_functions", nlohmann::json::array({
                {{"name", "unsafe"}, {"column", "label_copy"}, {"operator", "greater_equal"},
                 {"value", 0.5}, {"label", 1}}})}};
        }
        if (std::string(id) == "M-21") {
            input.erase("dataset");
        }
        ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
        zetta::ml::RunRequest request{&context, project.get(), graph.get(), input};
        const auto result = methodology->run(request);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "invalid_input");
        EXPECT_NE(result.error.message.find("label_copy"), std::string::npos)
            << result.error.message;
    }
    EXPECT_TRUE(node.runs().empty());
    EXPECT_TRUE(node.models().empty());
}

TEST_F(ActionJourneys, FeaturePlanStreamingReservesClocksBeforeCreatingRun) {
    const auto database = node.create_database("stream_clock_safety");
    ASSERT_TRUE(database);
    const auto table = database->create_table("events");
    ASSERT_TRUE(table);
    for (int index = 0; index < 80; ++index) {
        table->add_row({{"entity", index + 1}, {"label", index % 2},
                       {"amount", index % 7}, {"event_time", index},
                       {"label_time", index + 0.5}});
    }
    const auto project = node.create_project("stream clock safety");
    ASSERT_TRUE(project);
    const auto* methodology = zetta::ml::find_methodology("M-21");
    ASSERT_NE(methodology, nullptr);
    for (const auto* clock : {"event_time", "label_time", "label"}) {
        SCOPED_TRACE(clock);
        zetta::ml::ShapeRequest shape;
        shape.hints = {{"source", "stream_clock_safety"}, {"table", "events"},
                       {"entity_column", "entity"}, {"label_column", "label"},
                       {"time_column", "event_time"}, {"feature_columns", {"amount", clock}}};
        if (std::string(clock) == "label") {
            shape.hints["time_column"] = "label";
            shape.hints["feature_columns"] = {"amount"};
        }
        const auto graph = project->create_graph(clock, methodology->build_shape(shape));
        ASSERT_TRUE(graph);
        ApiContext context{node, nullptr, "", trace, &node.span_recorder(), &background};
        zetta::ml::RunRequest request{&context, project.get(), graph.get(), nlohmann::json::object()};
        const auto result = methodology->run(request);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "invalid_input");
        EXPECT_NE(result.error.message.find(clock), std::string::npos)
            << result.error.message;
    }
    EXPECT_TRUE(node.runs().empty());
    EXPECT_TRUE(node.models().empty());
}

TEST_F(Uc1Journey, FeaturePlanRefusesEventGrainBeforeStartingM1) {
    const auto database = node.create_database("feature_events");
    const auto table = database->create_table("events");
    for (int index = 0; index < 100; ++index) {
        table->add_row({{"customer_id", index % 10}, {"label", index % 2},
                       {"amount", index % 17}, {"event_at", "2026-09-01T12:00:00Z"}});
    }
    const auto project = node.create_project("event refusal");
    const auto* methodology = zetta::ml::find_methodology("M-1");
    auto graph = methodology->build_shape({});
    for (const auto& source : graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        source->attributes["source"] = "feature_events";
        source->attributes["table"] = "events";
    }
    const auto stored = project->create_graph("events", std::move(graph));
    ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
    zetta::ml::RunRequest request{&context, project.get(), stored.get(), nlohmann::json::object()};
    const auto result = methodology->run(request);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "invalid_input");
    EXPECT_NE(result.error.message.find("transform_required"), std::string::npos);
    EXPECT_TRUE(node.runs().empty());
}

TEST_F(Uc1Journey, FeaturePlanReportsMissingLabelBeforeContractFailure) {
    const auto database = node.create_database("feature_missing");
    const auto table = database->create_table("entities");
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"id", index + 1}, {"feature", index % 7}});
    }
    const auto project = node.create_project("missing label");
    const auto* methodology = zetta::ml::find_methodology("M-1");
    const auto graph = project->create_graph("missing label", methodology->build_shape({}));
    ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
    zetta::ml::RunRequest request{&context, project.get(), graph.get(),
        {{"dataset_path", "feature_missing.entities"}}};
    const auto result = methodology->run(request);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "invalid_input");
    EXPECT_NE(result.error.message.find("capture_label"), std::string::npos) << result.error.message;
    EXPECT_TRUE(node.runs().empty());
}

TEST_F(Uc1Journey, FeaturePlanKeepsSafeInputsAndHonorsExplicitSafetyAnnotations) {
    const auto database = node.create_database("feature_usage");
    const auto table = database->create_table("entities");
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"id", index + 1}, {"label", index % 2},
                       {"feature", 1.0 + (index % 7) * 1.3}, {"label_copy", index % 2}});
    }
    auto dataset = zetta::data::resolve_dataset(node,
        zetta::data::parse_dataset_path("feature_usage.entities"));
    ASSERT_TRUE(dataset.ok()) << dataset.error;
    const auto project = node.create_project("feature usage");
    zetta::ir::ExecutionGraph graph;
    const auto step = graph.add_node(zetta::ir::StepKind::Model, "model",
        {{"label", "label"}}, "model");
    const auto stored = project->create_graph("feature usage", std::move(graph));
    ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
    zetta::ml::RunRequest request{&context, project.get(), stored.get(), nlohmann::json::object()};
    const auto accepted = zetta::ml::plan_run_features(request, "M-1", dataset);
    EXPECT_TRUE(zetta::ml::feature_plan_error(accepted).empty())
        << zetta::ml::feature_plan_error(accepted);
    EXPECT_EQ(dataset.contract.feature_columns, std::vector<std::string>{"feature"});
    for (const auto* excluded : {"id", "label", "label_copy"}) {
        EXPECT_TRUE(std::ranges::any_of(accepted.excluded, [&](const auto& item) {
            return item.source_column == excluded && !item.reason.empty();
        }));
    }
    step->attributes["features"] = {{{"name", "feature"}, {"leaks_target", true}}};
    const auto refused = zetta::ml::plan_run_features(request, "M-1", dataset);
    EXPECT_FALSE(zetta::ml::feature_plan_error(refused).empty());
    EXPECT_TRUE(node.runs().empty());
}

// =====================================================================
// Canonical journey 1 — UC-1 end to end: project → intent → compile → gate →
// deploy → run → model + drivers read back, the near-data trace read-back,
// and every action audited under a capability-scoped acting key (RULE-3).
// The whole journey is the DATA table in journeys_canonical.hpp.
// =====================================================================
TEST_F(Uc1Journey, JourneyUc1EndToEndWithActingKey) {
    // The acting key: orchestration rights for exactly the loop's actions,
    // plus api:query for the read-backs. Deny-by-default everywhere else.
    auto principal = node.create_principal(
        "uc1 agent", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    zetta::governance::CapabilitySet grants;
    grants.grant(zetta::governance::CapabilityDomain::Api, "query");
    for (const char* action :
         {"create_project", "capture_intent", "compile_intent", "approve_plan",
          "gate", "deploy", "run"}) {
        grants.grant(zetta::governance::CapabilityDomain::Orchestration, action);
    }
    auto key = principal->create_key(zetta::governance::unrestricted_scope(),
                                     std::move(grants));
    ASSERT_TRUE(key);

    const journey::JourneyReport report = journey::run_journey(
        journey::canonical::uc1_happy_path(key->id()), caller(key->id()),
        pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey, JourneyGraphMethodologyCompilesRunsAndReports) {
    const std::string edges =
        "src,dst,weight,time,label\n"
        "1,2,1,1,0\n"
        "2,3,1,2,\n"
        "3,1,1,3,0\n"
        "4,5,1,1,1\n"
        "5,6,1,2,\n"
        "6,4,1,3,1\n"
        "3,4,50,9,0\n";
    const ApiResult uploaded = call(
        "upload_dataset", {{"table_name", "edges"},
                           {"format", "csv"},
                           {"content", edges}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    ASSERT_TRUE(call("create_project", {{"name", "graph-fraud"}}).ok);
    const ApiResult captured = call(
        "capture_intent", {{"project", "graph-fraud"},
                           {"text", "Learn fraud signals from relationships"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent", {{"project", "graph-fraud"},
                           {"input_id", captured.value.at("input_id")},
                           {"methodology", "M-12"},
                           {"name", "relationship graph"},
                           {"hints", {{"source", "datasets"},
                                      {"table", "edges"},
                                      {"prediction_time", 5.0}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-12");
    EXPECT_TRUE(compiled.value.at("runnable"));

    const ApiResult started = call(
        "run", {{"project", "graph-fraud"},
                {"graph", "relationship graph"},
                {"holdout_fraction", 0.34}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const std::string run_id = started.value.at("run_id");
    const ApiResult run = call("get_run", {{"run_id", run_id}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("status"), "succeeded");
    EXPECT_GE(run.value.at("outputs").size(), 4u);

    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("model").at("kind"), "graph");
    EXPECT_TRUE(report.value.at("model").contains("held_out_accuracy"));
    EXPECT_EQ(report.value.at("dataset").at("edges_read"), 6.0);
    EXPECT_EQ(report.value.at("dataset").at("edges_excluded"), 1.0);
    EXPECT_EQ(report.value.at("graph").at("component_count"), 2u);
    EXPECT_EQ(report.value.at("output_tables").size(), 2u);
}

// =====================================================================
// Canonical journey 2 — the gate-blocked negative: a leakage-annotated graph
// is refused over MCP exactly as in the GUI, with the failing check's reason
// — by gate, deploy, AND run; the read-backs prove nothing was placed and
// nothing executed.
// =====================================================================
TEST_F(ActionJourneys, JourneyGateBlockedDeployAndRunCarryTheReason) {
    ASSERT_TRUE(call("create_project", {{"name", "leaky"}}).ok);
    auto project = node.find_project("leaky");
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->create_graph("leaky graph",
                                      journey::canonical::leakage_graph()));

    const journey::JourneyReport report =
        journey::run_journey(journey::canonical::gate_blocked(), caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(ActionJourneys, JourneyMethodologyDiscoveryCompileAndRunRefusals) {
    const ApiResult listed =
        call("list_methodologies", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("methodologies").size(), 21u);
    for (std::size_t index = 0; index < 21; ++index) {
        const auto& methodology = listed.value.at("methodologies")[index];
        EXPECT_EQ(methodology.at("id"), "M-" + std::to_string(index + 1));
        EXPECT_TRUE(methodology.at("runnable"));
    }
    EXPECT_EQ(listed.value.at("methodologies").at(12).at("id"), "M-13");
    EXPECT_TRUE(listed.value.at("methodologies").at(12).at("runnable"));

    ASSERT_TRUE(call("create_project", {{"name", "portfolio"}}).ok);
    const ApiResult captured =
        call("capture_intent", {{"project", "portfolio"},
                                {"text", "Group customers into personas"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const std::string input_id = captured.value.at("input_id");

    const ApiResult explicit_m3 =
        call("compile_intent", {{"project", "portfolio"},
                                {"input_id", input_id},
                                {"methodology", "M-3"}});
    ASSERT_TRUE(explicit_m3.ok) << explicit_m3.error.message;
    EXPECT_EQ(explicit_m3.value.at("methodology"), "M-3");
    EXPECT_TRUE(explicit_m3.value.at("runnable"));
    EXPECT_EQ(explicit_m3.value.at("name"), "Segmentation / clustering pipeline");
    EXPECT_NE(explicit_m3.value.at("name"), "generic skeleton");

    const ApiResult solution =
        call("compile_intent", {{"project", "portfolio"},
                                {"input_id", input_id},
                                {"solution", "S-1"},
                                {"name", "solution graph"}});
    ASSERT_TRUE(solution.ok) << solution.error.message;
    EXPECT_EQ(solution.value.at("methodology"), "M-1");
    EXPECT_TRUE(solution.value.at("runnable"));

    const ApiResult by_keyword =
        call("compile_intent", {{"project", "portfolio"},
                                {"input_id", input_id},
                                {"name", "segments from intent"}});
    ASSERT_TRUE(by_keyword.ok) << by_keyword.error.message;
    EXPECT_EQ(by_keyword.value.at("methodology"), "M-3");
    EXPECT_TRUE(by_keyword.value.at("runnable"));

    const ApiResult unknown =
        call("capture_intent",
             {{"project", "portfolio"},
              {"text", "Summarize incoming support tickets by topic"}});
    ASSERT_TRUE(unknown.ok) << unknown.error.message;
    const ApiResult unmatched =
        call("compile_intent", {{"project", "portfolio"},
                                {"input_id", unknown.value.at("input_id")}});
    EXPECT_FALSE(unmatched.ok);
    EXPECT_EQ(unmatched.error.code, "not_supported");
    EXPECT_NE(unmatched.error.message.find("Runnable methodologies"),
              std::string::npos);

    auto project = node.find_project("portfolio");
    ASSERT_TRUE(project);
    zetta::ir::ExecutionGraph graph;
    graph.add_node(zetta::ir::StepKind::Connect, "source", {}, "source");
    graph.add_node(zetta::ir::StepKind::Model, "segments",
                   nlohmann::json{{"model_kind", "segmentation"}}, "segments");
    graph.add_edge("source", "segments");
    ASSERT_TRUE(project->create_graph("segmentation graph", std::move(graph)));

    const ApiResult graph_view =
        call("get_graph",
             {{"project", "portfolio"}, {"graph", "segmentation graph"}});
    ASSERT_TRUE(graph_view.ok) << graph_view.error.message;
    EXPECT_EQ(graph_view.value.at("methodology"), "M-3");
    EXPECT_TRUE(graph_view.value.at("runnable"));

    const ApiResult project_view = call("get_project", {{"name", "portfolio"}});
    ASSERT_TRUE(project_view.ok) << project_view.error.message;
    bool found = false;
    for (const nlohmann::json& entry : project_view.value.at("graphs")) {
        if (entry.at("name") == "segmentation graph") {
            found = true;
            EXPECT_EQ(entry.at("methodology"), "M-3");
            EXPECT_TRUE(entry.at("runnable"));
        }
    }
    EXPECT_TRUE(found);

    const ApiResult run = call(
        "run", {{"project", "portfolio"}, {"graph", "segmentation graph"}});
    EXPECT_FALSE(run.ok);
    EXPECT_EQ(run.error.code, "engine_error");
    EXPECT_NE(run.error.message.find("blocked by gate"), std::string::npos);

    const auto unavailable = call("compile_intent", {{"project", "portfolio"},
                                                     {"input_id", input_id},
                                                     {"methodology", "unregistered_model_kind"}});
    EXPECT_FALSE(unavailable.ok);
    EXPECT_EQ(unavailable.error.code, "invalid_input");
    EXPECT_NE(unavailable.error.message.find("unregistered_model_kind"), std::string::npos);

    zetta::ir::ExecutionGraph unknown_graph;
    unknown_graph.add_node(zetta::ir::StepKind::Model, "unknown model",
                           {{"model_kind", "unregistered_model_kind"}}, "unknown");
    ASSERT_TRUE(project->create_graph("unknown model graph", std::move(unknown_graph)));
    const ApiResult refused = call(
        "run", {{"project", "portfolio"}, {"graph", "unknown model graph"}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "not_supported");
    EXPECT_NE(refused.error.message.find("no registered methodology"), std::string::npos);
}

TEST_F(Uc1Journey, EntityResolutionCompilesAndRunsItsPipelinesTable) {
    std::shared_ptr<zetta::data::Database> database =
        node.create_database("datasets");
    ASSERT_TRUE(database);
    std::shared_ptr<zetta::data::Table> table =
        database->create_table("resolution_records");
    ASSERT_TRUE(table);
    const std::vector<nlohmann::json> rows = {
        {{"id", 1},
         {"record_id", 1},
         {"source", "crm"},
         {"name", "Alice Johnson"},
         {"address", "10 Pine Street"},
         {"email", "alice@example.com"},
         {"phone", "604-555-0101"},
         {"email_verified", true},
         {"phone_verified", true},
         {"truth_entity", "alice"},
         {"label", true},
         {"date", 10}},
        {{"id", 2},
         {"record_id", 101},
         {"source", "billing"},
         {"name", "Alyce Johnson"},
         {"address", "10 Pine St"},
         {"email", "ALICE@example.com"},
         {"phone", "6045550101"},
         {"email_verified", true},
         {"phone_verified", true},
         {"truth_entity", "alice"},
         {"label", true},
         {"date", 11}},
        {{"id", 3},
         {"record_id", 2},
         {"source", "crm"},
         {"name", "Bob Martinez"},
         {"address", "22 Oak Avenue"},
         {"email", "bob@example.com"},
         {"phone", "604-555-0102"},
         {"email_verified", true},
         {"phone_verified", true},
         {"truth_entity", "bob"},
         {"label", false},
         {"date", 20}},
        {{"id", 4},
         {"record_id", 102},
         {"source", "billing"},
         {"name", "Bbo Martinez"},
         {"address", "22 Oak Ave"},
         {"email", "bob@example.com"},
         {"phone", "6045550102"},
         {"email_verified", true},
         {"phone_verified", true},
         {"truth_entity", "bob"},
         {"label", false},
         {"date", 21}},
        {{"id", 5},
         {"record_id", 3},
         {"source", "crm"},
         {"name", "Carol Singh"},
         {"address", "9 Cedar Road"},
         {"email", "carol@example.com"},
         {"phone", "604-555-0103"},
         {"email_verified", true},
         {"phone_verified", true},
         {"truth_entity", "carol"},
         {"label", true},
         {"date", 30}},
        {{"id", 6},
         {"record_id", 103},
         {"source", "billing"},
         {"name", "Carol Sing"},
         {"address", "9 Cedar Rd"},
         {"email", "carol@example.com"},
         {"phone", "6045550103"},
         {"email_verified", true},
         {"phone_verified", true},
         {"truth_entity", "carol"},
         {"label", true},
         {"date", 31}},
    };
    for (const nlohmann::json& row : rows) {
        table->add_row(row);
    }

    ASSERT_TRUE(call("create_project", {{"name", "identity"}}).ok);
    const ApiResult captured =
        call("capture_intent",
             {{"project", "identity"},
              {"text", "Resolve customer records across source systems"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "identity"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-2"},
         {"hints", {{"source", "datasets"},
                    {"table", "resolution_records"}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-2");
    EXPECT_TRUE(compiled.value.at("runnable"));

    const ApiResult run = call(
        "run", {{"project", "identity"},
                {"graph", compiled.value.at("name")}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("engine"), "entity_resolution");

    const ApiResult run_view =
        call("get_run", {{"run_id", run.value.at("run_id")}});
    ASSERT_TRUE(run_view.ok) << run_view.error.message;
    EXPECT_EQ(run_view.value.at("status"), "succeeded");
    ASSERT_EQ(run_view.value.at("outputs").size(), 4u);

    const ApiResult report =
        call("get_run_report", {{"run_id", run.value.at("run_id")}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("model").at("kind"), "entity_resolution");
    EXPECT_GT(
        report.value.at("model").at("metrics").at("pairwise_f1").get<double>(),
        0.9);
    EXPECT_DOUBLE_EQ(report.value.at("thresholds").at("match").get<double>(),
                     0.82);
    EXPECT_DOUBLE_EQ(report.value.at("thresholds").at("review").get<double>(),
                     0.55);
    EXPECT_EQ(report.value.at("thresholds").at("non_match"), "below review");
    EXPECT_EQ(report.value.at("output_tables").size(), 2u);
    EXPECT_EQ(report.value.at("dataset").at("source"),
              "datasets.resolution_records");
}

TEST_F(Uc1Journey, SegmentationMethodologyRunsItsPipelinesCustomerTable) {
    std::string content;
    for (int group = 0; group < 3; ++group) {
        for (int index = 0; index < 30; ++index) {
            const double noise = std::sin(static_cast<double>(index)) * 0.05;
            const nlohmann::json row = {
                {"customer_id", group * 30 + index + 1},
                {"spend", 10.0 + group * 100.0 + noise},
                {"visits", 2.0 + group * 20.0 + noise},
                {"tenure", 60.0 - group * 25.0 + noise},
                {"region",
                 std::vector<std::string>{"north", "south", "west"}[group]},
                {"outcome", {{"never_read", true}}}};
            content += row.dump() + "\n";
        }
    }
    journey::Journey segments{"customer-segmentation", {}};
    segments.steps = {
        {"land the customer's table",
         "upload_dataset",
         {{"database", "crm"},
          {"table_name", "customers"},
          {"format", "jsonl"},
          {"content", content}}},
        {"read the landed table",
         "get_dataset",
         {{"database", "crm"}, {"dataset", "customers"}},
         {.checks = {journey::equals("/rows", 90)}}},
        {"create the project", "create_project", {{"name", "customer-groups"}}},
        {"read the project",
         "get_project",
         {{"name", "customer-groups"}},
         {.checks = {journey::equals("/name", "customer-groups")}}},
        {"describe the goal",
         "capture_intent",
         {{"project", "customer-groups"},
          {"text", "segment my customers into groups"}}},
        {"read the captured intent",
         "get_project",
         {{"name", "customer-groups"}},
         {.checks = {journey::size_equals("/inputs", 1)}}},
        {"compile M-3 with the customer's source",
         "compile_intent",
         {{"project", "customer-groups"},
          {"methodology", "M-3"},
          {"name", "customer segments"},
          {"hints",
           {{"source", "crm"},
            {"table", "customers"},
            {"maximum_k", 5},
            {"restarts", 2}}}},
         {.checks = {journey::equals("/methodology", "M-3"),
                     journey::equals("/runnable", true),
                     journey::equals("/steps", 5)}}},
        {"read the own pipeline",
         "get_graph",
         {{"project", "customer-groups"}, {"graph", "customer segments"}},
         {.checks = {journey::equals("/methodology", "M-3"),
                     journey::equals("/runnable", true)}}},
        {"run without a dataset override",
         "run",
         {{"project", "customer-groups"}, {"graph", "customer segments"}},
         {.checks = {journey::equals("/engine", "segmentation"),
                     journey::equals("/status", "running")}},
         {{"segmentation_run", "/run_id"}}},
        {"wait for trained outputs",
         "get_run",
         {{"run_id", "$segmentation_run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_equals("/outputs", 4),
                     journey::contains_element("/lineage/sources",
                                               "dataset:crm.customers"),
                     journey::any_element("/outputs", {{"/kind", "report"}})}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read the measured segments",
         "get_run_report",
         {{"run_id", "$segmentation_run"}},
         {.checks = {journey::equals("/methodology", "M-3"),
                     journey::equals("/model/metrics/chosen_k", 3),
                     journey::at_least("/model/metrics/silhouette", 0.9),
                     journey::exists("/model/metrics/davies_bouldin"),
                     journey::exists("/model/metrics/stability"),
                     journey::not_empty("/model/chosen_k_reason"),
                     journey::size_equals("/model/k_candidates", 4),
                     journey::equals("/dataset/table", "crm.customers"),
                     journey::equals("/dataset/read_rows", 90),
                     journey::equals("/dataset/clustered_rows", 90),
                     journey::contains_element("/dataset/excluded_columns",
                                               "customer_id"),
                     journey::contains_element("/dataset/excluded_columns",
                                               "outcome"),
                     journey::size_equals("/segments", 3),
                     journey::any_element(
                         "/output_tables",
                         {{"/name", "segment assignments"}, {"/rows", 90}}),
                     journey::any_element(
                         "/output_tables",
                         {{"/name", "segment profiles"}, {"/rows", 3}})}}},
        {"limit the profile preview",
         "get_run_report",
         {{"run_id", "$segmentation_run"}, {"limit", 1}},
         {.checks = {journey::size_equals("/segments", 1)}}},
    };
    const auto report = journey::run_journey(segments, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey,
       SegmentationMethodologyRefusesMissingTableAndUnsafeOverrides) {
    ASSERT_TRUE(call("create_project", {{"name", "missing-source"}}).ok);
    ASSERT_TRUE(
        call("capture_intent", {{"project", "missing-source"},
                                {"text", "segment my customers into groups"}})
            .ok);
    const auto compiled = call("compile_intent", {{"project", "missing-source"},
                                                  {"solution", "S-9"},
                                                  {"name", "segments"}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    const auto missing =
        call("run", {{"project", "missing-source"}, {"graph", "segments"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.message.find("datasets.customers"),
              std::string::npos);
    const auto unsafe =
        call("run", {{"project", "missing-source"},
                     {"graph", "segments"},
                     {"dataset",
                      {{"entity_column", "customer"},
                       {"feature_columns", {"customer", "spend"}}}}});
    EXPECT_FALSE(unsafe.ok);
    EXPECT_NE(unsafe.error.message.find("entity key"), std::string::npos);
    const auto supervised = call("run", {{"project", "missing-source"},
                                         {"graph", "segments"},
                                         {"holdout_fraction", 0.2}});
    EXPECT_FALSE(supervised.ok);
    EXPECT_NE(supervised.error.message.find("no supervised holdout"),
              std::string::npos);
    EXPECT_TRUE(node.runs().empty());
}

TEST_F(Uc1Journey, SegmentationMethodologyRecordsTrainingFailureOnItsRun) {
    journey::Journey failed{"unsuitable-segmentation-data", {}};
    failed.steps = {
        {"land entities without usable variation",
         "upload_dataset",
         {{"table_name", "customers"},
          {"format", "jsonl"},
          {"content", "{\"customer_id\":1,\"spend\":10}\n{\"customer_id\":2,"
                      "\"spend\":10}\n"}}},
        {"read the table",
         "get_dataset",
         {{"dataset", "customers"}},
         {.checks = {journey::equals("/rows", 2)}}},
        {"create the project",
         "create_project",
         {{"name", "constant-customers"}}},
        {"read the project",
         "get_project",
         {{"name", "constant-customers"}},
         {.checks = {journey::equals("/name", "constant-customers")}}},
        {"capture segmentation intent",
         "capture_intent",
         {{"project", "constant-customers"},
          {"text", "segment my customers into groups"}}},
        {"read the intent",
         "get_project",
         {{"name", "constant-customers"}},
         {.checks = {journey::size_equals("/inputs", 1)}}},
        {"compile the intent",
         "compile_intent",
         {{"project", "constant-customers"}, {"name", "segments"}},
         {.checks = {journey::equals("/methodology", "M-3")}}},
        {"read the graph",
         "get_graph",
         {{"project", "constant-customers"}, {"graph", "segments"}},
         {.checks = {journey::equals("/runnable", true)}}},
        {"submit a real training job",
         "run",
         {{"project", "constant-customers"}, {"graph", "segments"}},
         {.checks = {journey::equals("/engine", "segmentation")}},
         {{"run", "/run_id"}}},
        {"the job fails with evidence, never structurally succeeds",
         "get_run",
         {{"run_id", "$run"}},
         {.checks = {journey::equals("/status", "failed"),
                     journey::not_empty("/logs"),
                     journey::size_equals("/outputs", 0)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
    };
    const auto report = journey::run_journey(failed, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey, AnomalyMethodologyRunsItsPipelinesTableAndReportsFindings) {
  std::string content;
  for (int row = 0; row < 100; ++row) {
    content += nlohmann::json{{"request_id", row + 1},
                              {"latency", row == 41 ? 80.0 : (row % 19) / 10.0},
                              {"errors", row == 82 ? 90.0 : (row % 11) / 10.0},
                              {"description", "request"}}
                   .dump() +
               "\n";
  }
  using namespace zetta::test::journey;
  const Journey flow{
      "M-7 table to measured findings",
      {{"land the observations",
        "upload_dataset",
        {{"database", "operations"},
         {"table_name", "requests"},
         {"format", "jsonl"},
         {"content", content}},
        {.checks = {equals("/rows", 100)}}},
       {"read the landed table",
        "get_dataset",
        {{"database", "operations"}, {"dataset", "requests"}},
        {.checks = {equals("/rows", 100)}}},
       {"create anomaly project",
        "create_project",
        {{"name", "anomaly-project"}}},
       {"read project",
        "get_project",
        {{"name", "anomaly-project"}},
        {.checks = {not_empty("/id")}}},
       {"capture the detection goal",
        "capture_intent",
        {{"project", "anomaly-project"},
         {"text", "Find unusual request latency and errors"}}},
       {"read captured goal",
        "get_project",
        {{"name", "anomaly-project"}},
        {.checks = {size_equals("/inputs", 1)}}},
       {"compile M-7",
        "compile_intent",
        {{"project", "anomaly-project"},
         {"methodology", "M-7"},
         {"name", "detector"},
         {"hints",
          {{"source", "operations"},
           {"table", "requests"},
           {"entity_column", "request_id"},
           {"alert_budget", 2}}}},
        {.checks = {equals("/methodology", "M-7"), equals("/runnable", true)}}},
       {"read M-7 pipeline",
        "get_graph",
        {{"project", "anomaly-project"}, {"graph", "detector"}},
        {.checks = {equals("/methodology", "M-7"), equals("/runnable", true)}}},
       {"execute the pipeline's own table",
        "run",
        {{"project", "anomaly-project"}, {"graph", "detector"}},
        {.checks = {equals("/engine", "anomaly"), not_empty("/run_id")}},
        {{"anomaly_run", "/run_id"}}},
       {"wait for measured results",
        "get_run",
        {{"run_id", "$anomaly_run"}},
        {.checks = {equals("/status", "succeeded"), size_equals("/outputs", 4),
                    contains_element("/lineage/sources",
                                     "operations.requests")}},
        {},
        Poll{"/status", {"succeeded", "failed"}}},
       {"read anomaly facts",
        "get_run_report",
        {{"run_id", "$anomaly_run"}},
        {.checks = {equals("/methodology", "M-7"),
                    equals("/model/kind", "anomaly"),
                    equals("/model/metrics/rows_scored", 100),
                    equals("/model/metrics/flagged", 2),
                    equals("/model/metrics/alert_rate", 0.02),
                    equals("/model/metrics/alert_budget", 2),
                    exists("/model/metrics/threshold"),
                    equals("/dataset/source", "operations.requests"),
                    equals("/dataset/rows_read", 100),
                    equals("/dataset/rows_excluded", 0),
                    contains_element(
                        "/dataset/not_used",
                        "description: not selected as a numeric feature"),
                    size_equals("/output_tables", 2),
                    equals("/anomalies/0/flagged", true),
                    equals("/anomalies/1/flagged", true),
                    equals("/anomalies/2/flagged", false),
                    any_element("/missing", {{"/field", "precision_at_k"}})}},
        {{"anomaly_table", "/output_tables/0/table"}}},
       {"verify report table remains queryable",
        "get_run_report",
        {{"run_id", "$anomaly_run"}, {"limit", 1}},
        {.checks = {size_equals("/anomalies", 1),
                    size_equals("/output_tables", 2)}}}}};
  const auto report = run_journey(flow, caller(), pump());
  ASSERT_TRUE(report.ok) << report.summary();
  const auto compiled = call("compile_intent", {{"project", "anomaly-project"},
                                                {"solution", "S-7"},
                                                {"name", "solution-detector"}});
  ASSERT_TRUE(compiled.ok) << compiled.error.message;
  EXPECT_EQ(compiled.value.at("methodology"), "M-7");
  EXPECT_TRUE(compiled.value.at("runnable"));
}

TEST_F(Uc1Journey,
       AnomalyMethodologyRejectsLabelFeatureOverridesBeforeStarting) {
  ASSERT_TRUE(call("create_project", {{"name", "anomaly-negative"}}).ok);
  ASSERT_TRUE(call("capture_intent", {{"project", "anomaly-negative"},
                                      {"text", "detect anomalies"}})
                  .ok);
  ASSERT_TRUE(call("compile_intent", {{"project", "anomaly-negative"},
                                      {"methodology", "M-7"},
                                      {"name", "detector"}})
                  .ok);
  const auto result = call(
      "run", {{"project", "anomaly-negative"},
              {"graph", "detector"},
              {"dataset",
               {{"label_column", "fraud"}, {"feature_columns", {"fraud"}}}}});
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error.code, "invalid_input");
  EXPECT_NE(result.error.message.find("label"), std::string::npos);
  EXPECT_TRUE(node.runs().empty());
}

TEST_F(Uc1Journey, JourneyForecastReadsItsPipelinesTableAndReportsOutputs) {
    const fs::path file = dir.path() / "weekly_demand.jsonl";
    {
        std::ofstream output(file);
        for (int series = 1; series <= 2; ++series) {
            for (int day = 0; day < 168; ++day) {
                const double demand =
                    series * 100.0 + day * 0.4 +
                    12.0 * std::sin(day * 6.283185307179586 / 7.0);
                output << nlohmann::json{{"series", series},
                                         {"day", day},
                                         {"demand", demand},
                                         {"future_note", "excluded"}}
                              .dump()
                       << '\n';
            }
        }
    }
    const nlohmann::json hints = {{"source", "demand"},
                                  {"table", "history"},
                                  {"time_column", "day"},
                                  {"target_column", "demand"},
                                  {"series_column", "series"},
                                  {"num_trees", 12},
                                  {"max_depth", 3},
                                  {"seed", 41}};
    journey::Journey forecast{"forecast-table-to-report", {}};
    forecast.steps = {
        {"land the observation table",
         "add_dataset",
         {{"path", file.string()},
          {"database", "demand"},
          {"table_name", "history"},
          {"partition_count", 2}},
         {.checks = {journey::equals("/rows", 336)}}},
        {"create the forecast project",
         "create_project",
         {{"name", "demand-plan"}}},
        {"capture the question",
         "capture_intent",
         {{"project", "demand-plan"}, {"text", "Forecast weekly demand"}},
         {},
         {{"input", "/input_id"}}},
        {"discover runnable M-4",
         "list_methodologies",
         nlohmann::json::object(),
         {.checks = {journey::any_element(
              "/methodologies", {{"/id", "M-4"}, {"/runnable", true}})}}},
        {"compile the forecast methodology",
         "compile_intent",
         {{"project", "demand-plan"},
          {"input_id", "$input"},
          {"methodology", "M-4"},
          {"hints", hints},
          {"name", "demand forecast"}},
         {.checks = {journey::equals("/methodology", "M-4"),
                     journey::equals("/runnable", true)}},
         {{"graph", "/name"}}},
        {"compile the primary solution too",
         "compile_intent",
         {{"project", "demand-plan"},
          {"input_id", "$input"},
          {"solution", "S-5"},
          {"hints", hints},
          {"name", "solution forecast"}},
         {.checks = {journey::equals("/methodology", "M-4"),
                     journey::equals("/runnable", true)}}},
        {"run without a dataset override",
         "run",
         {{"project", "demand-plan"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/engine", "forecast"),
                     journey::equals("/dataset/source", "demand.history")}},
         {{"run", "/run_id"}}},
        {"wait for forecast outputs",
         "get_run",
         {{"run_id", "$run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::contains_element("/lineage/sources",
                                               "dataset:demand.history"),
                     journey::any_element("/outputs", {{"/kind", "dataset"}}),
                     journey::any_element("/outputs", {{"/kind", "model"}})}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read the measured report",
         "get_run_report",
         {{"run_id", "$run"}, {"limit", 30}},
         {.checks = {journey::equals("/model/kind", "forecast"),
                     journey::exists("/model/metrics/backtest_mae"),
                     journey::exists("/model/metrics/backtest_mase"),
                     journey::exists("/model/metrics/backtest_smape"),
                     journey::exists("/model/metrics/forecast_value_added"),
                     journey::exists("/model/metrics/fold_0_mae"),
                     journey::exists("/model/metrics/seasonal_naive_mae"),
                     journey::equals("/dataset/rows_read", 336),
                     journey::equals("/dataset/rows_excluded", 0),
                     journey::any_element("/dataset/not_used",
                                          {{"/column", "future_note"}}),
                     journey::size_equals("/forecast", 14),
                     journey::exists("/forecast/0/lower"),
                     journey::exists("/forecast/0/upper"),
                     journey::not_empty("/baseline_comparison"),
                     journey::size_equals("/outputs", 3)}}}};
    const auto result = journey::run_journey(forecast, caller(), pump());
    EXPECT_TRUE(result.ok) << result.summary();
}

TEST_F(Uc1Journey, JourneyForecastRefusesWrongTableAndRandomHoldout) {
    ASSERT_TRUE(call("create_project", {{"name", "forecast-refusals"}}).ok);
    const auto captured =
        call("capture_intent",
             {{"project", "forecast-refusals"}, {"text", "Forecast demand"}});
    ASSERT_TRUE(captured.ok);
    const auto compiled =
        call("compile_intent",
             {{"project", "forecast-refusals"},
              {"input_id", captured.value.at("input_id")},
              {"methodology", "M-4"},
              {"hints", {{"source", "missing"}, {"table", "history"}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    const nlohmann::json input = {{"project", "forecast-refusals"},
                                  {"graph", compiled.value.at("name")}};
    const auto missing = call("run", input);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.message.find("missing.history"), std::string::npos);
    auto random = input;
    random["holdout_fraction"] = 0.2;
    const auto refused = call("run", random);
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.message.find("temporal"), std::string::npos);
    EXPECT_TRUE(node.runs().empty());
}

TEST_F(Uc1Journey, SurvivalMethodologyRunsItsPipelinesTable) {
    auto database = node.create_database("datasets");
    ASSERT_TRUE(database);
    auto table = database->create_table("survival_subjects");
    ASSERT_TRUE(table);
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(3));
    for (std::int64_t entity = 0; entity < 120; ++entity) {
        const bool high_risk = entity % 2 == 0;
        const bool event = entity % 5 != 0;
        table->add_row({{"entity", entity + 1},
                        {"start_time", static_cast<double>(entity)},
                        {"duration", high_risk ? 25.0 : 150.0},
                        {"event", event ? 1 : 0},
                        {"feature_time", static_cast<double>(entity)},
                        {"risk_feature", high_risk ? 1.0 : 0.0}});
    }
    const zetta::data::PartitionWriteResult written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;

    ASSERT_TRUE(call("create_project", {{"name", "survival journey"}}).ok);
    const ApiResult captured = call(
        "capture_intent",
        {{"project", "survival journey"},
         {"text", "Predict when each subject will experience an event"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "survival journey"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-5"},
         {"name", "survival graph"}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-5");
    EXPECT_TRUE(compiled.value.at("runnable"));
    ASSERT_TRUE(call("approve_plan",
                     {{"project", "survival journey"},
                      {"graph", "survival graph"}})
                    .ok);

    const ApiResult started = call(
        "run",
        {{"project", "survival journey"},
         {"graph", "survival graph"},
         {"dataset",
          {{"entity_column", "entity"},
           {"label_column", "event"},
           {"time_column", "start_time"},
           {"as_of", 119.0},
           {"feature_columns", {"risk_feature"}}}},
         {"interval_width", 30.0},
         {"max_intervals", 6},
         {"validation_fraction", 0.2}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value.at("engine"), "survival");
    const std::string run_id = started.value.at("run_id");

    ApiResult status;
    for (int attempt = 0; attempt < 500; ++attempt) {
        node.tick();
        background.prune();
        status = call("get_run", {{"run_id", run_id}});
        if (status.ok && status.value.at("status") != "running") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(status.ok) << status.error.message;
    ASSERT_EQ(status.value.at("status"), "succeeded") << status.value.dump();

    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_GT(report.value.at("model").at("concordance").get<double>(), 0.5);
    EXPECT_TRUE(report.value.at("model").contains("integrated_brier_score"));
    EXPECT_EQ(report.value.at("dataset").at("source"),
              "datasets.survival_subjects");
    EXPECT_EQ(report.value.at("dataset").at("features"),
              nlohmann::json::array({"risk_feature"}));
    ASSERT_FALSE(report.value.at("drivers").empty());
    EXPECT_EQ(report.value.at("drivers")[0].at("feature"), "risk_feature");
    EXPECT_TRUE(report.value.at("outputs").contains("survival_table"));
    EXPECT_TRUE(report.value.at("outputs").contains("interval_calibration"));
}

TEST_F(Uc1Journey, JourneyCausalUpliftCompilesRunsAndReports) {
    const fs::path file = fs::path(dir.path()) / "uplift.jsonl";
    {
        std::ofstream out(file);
        for (std::int64_t index = 0; index < 360; ++index) {
            const double segment =
                static_cast<double>((index * 37) % 101) / 101.0;
            const double baseline =
                static_cast<double>((index * 61) % 103) / 103.0;
            const std::int64_t treatment = (index * 17) % 2;
            const double probability =
                0.12 + 0.18 * baseline +
                (segment >= 0.5 ? 0.5 * treatment : 0.0);
            const double draw =
                static_cast<double>((index * 73) % 107) / 107.0;
            out << nlohmann::json{{"entity", index},
                                  {"segment", segment},
                                  {"baseline", baseline},
                                  {"feature_observed_at",
                                   "2024-01-15T00:00:00Z"},
                                  {"treatment", treatment},
                                  {"outcome", draw < probability ? 1 : 0}}
                       .dump()
                << '\n';
        }
    }
    const ApiResult landed =
        call("add_dataset", {{"path", file.string()},
                             {"database", "experiments"},
                             {"table_name", "causal_population"},
                             {"partition_count", 4}});
    ASSERT_TRUE(landed.ok) << landed.error.message;
    const ApiResult snapshot = call(
        "snapshot",
        {{"database", "experiments"},
         {"source_table", "causal_population"},
         {"target_table", "causal_population_snapshot"},
         {"entity_column", "entity"},
         {"time_column", "feature_observed_at"},
         {"as_of", "2024-01-31T00:00:00Z"}});
    ASSERT_TRUE(snapshot.ok) << snapshot.error.message;

    ASSERT_TRUE(call("create_project", {{"name", "causal targeting"}}).ok);
    const ApiResult captured =
        call("capture_intent",
             {{"project", "causal targeting"},
              {"text", "Estimate uplift and decide who to treat"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "causal targeting"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-6"},
         {"name", "uplift policy"},
         {"hints",
          {{"source", "experiments"},
           {"table", "causal_population_snapshot"},
           {"entity", "entity"},
           {"treatment", "treatment"},
           {"outcome", "outcome"},
           {"as_of", "2024-02-01T00:00:00Z"},
           {"snapshot_as_of", "2024-01-31T00:00:00Z"},
           {"feature_window_end", "2024-01-31T00:00:00Z"},
           {"label_window_start", "2024-02-07T00:00:00Z"},
           {"blackout_gap_seconds", 604800},
           {"treatment_time", "2024-02-07T00:00:00Z"},
           {"features", nlohmann::json::array({"segment", "baseline"})}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-6");
    EXPECT_TRUE(compiled.value.at("runnable"));
    ASSERT_TRUE(call("approve_plan", {{"project", "causal targeting"},
                                      {"graph", "uplift policy"}})
                    .ok);

    const ApiResult started =
        call("run", {{"project", "causal targeting"},
                     {"graph", "uplift policy"},
                     {"targeting_fraction", 0.2},
                     {"bootstrap_samples", 40},
                     {"seed", 1045}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value.at("engine"), "uplift");
    const std::string run_id = started.value.at("run_id");

    const ApiResult run = call("get_run", {{"run_id", run_id}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("status"), "succeeded");
    ASSERT_EQ(run.value.at("outputs").size(), 4u);

    const ApiResult report =
        call("get_run_report", {{"run_id", run_id}, {"limit", 12}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_TRUE(report.value.at("model").at("metrics").contains("t_auuc"));
    EXPECT_TRUE(report.value.at("model").at("metrics").contains("x_auuc"));
    EXPECT_EQ(report.value.at("treatment_effects").size(), 12u);
    EXPECT_FALSE(report.value.at("qini_curve").empty());
    EXPECT_EQ(report.value.at("dataset").at("source"),
              "experiments.causal_population_snapshot");
}
// =====================================================================
// Canonical journey 3 — dataset ingest: a local jsonl file becomes a
// partitioned table whose placement reads back through get_dataset AND the
// system.dataset_partitions view; audited as dataset_added.
// =====================================================================
TEST_F(Uc1Journey,
       JourneyNextBestActionReadsItsPipelineTableAndReportsRecommendations) {
    const fs::path file = dir.path() / "recommendation_candidates.jsonl";
    {
        std::ofstream output(file);
        for (int entity = 0; entity < 48; ++entity) {
            for (int action = 0; action < 3; ++action) {
                const double signal =
                    static_cast<double>((entity * 5 + action * 7) % 17) / 16.0;
                const int response = (entity + action) % 2;
                const bool preferred = action == entity % 3;
                output
                    << nlohmann::json{
                           {"entity_id", entity},
                           {"action_id", action},
                           {"response", response},
                           {"signal", signal},
                           {"action_value", 8.0 + action},
                           {"action_cost", 1.0 + action * 0.25},
                           {"eligible", entity == 2 && action == 2 ? 0 : 1},
                           {"contacts", entity == 3 && action == 1 ? 2 : 0},
                           {"contact_cap", 2},
                           {"interaction_count", preferred ? 3 : 0},
                           {"interaction_time", 1},
                           {"logging_propensity", 1.0 / 3.0},
                           {"unused_note", "excluded"}}
                           .dump()
                    << '\n';
                if (preferred) {
                    output << nlohmann::json{
                                  {"entity_id", entity},
                                  {"action_id", action},
                                  {"response", response},
                                  {"signal", signal},
                                  {"action_value", 8.0 + action},
                                  {"action_cost", 1.0 + action * 0.25},
                                  {"eligible", 1},
                                  {"contacts", 0},
                                  {"contact_cap", 2},
                                  {"interaction_count", 2},
                                  {"interaction_time", 2},
                                  {"logging_propensity", 1.0 / 3.0},
                                  {"unused_note", "excluded"}}
                                  .dump()
                           << '\n';
                }
            }
        }
    }
    const nlohmann::json hints = {
        {"source", "offers"},
        {"table", "candidates"},
        {"entity_column", "entity_id"},
        {"action_column", "action_id"},
        {"label_column", "response"},
        {"feature_columns", {"signal"}},
        {"num_trees", 8},
        {"max_depth", 2},
        {"factors", 3},
        {"als_iterations", 4},
        {"budget", 1000.0},
        {"seed", 41}};
    journey::Journey recommendation{"next-best-action-table-to-report", {}};
    recommendation.steps = {
        {"land candidate actions",
         "add_dataset",
         {{"path", file.string()},
          {"database", "offers"},
          {"table_name", "candidates"},
          {"partition_count", 2}},
         {.checks = {journey::equals("/rows", 192)}}},
        {"create recommendation project",
         "create_project",
         {{"name", "offer-plan"}}},
        {"capture recommendation intent",
         "capture_intent",
         {{"project", "offer-plan"},
          {"text", "Recommend the next best action for each customer"}},
         {},
         {{"input", "/input_id"}}},
        {"discover runnable M-8",
         "list_methodologies",
         nlohmann::json::object(),
         {.checks = {journey::any_element(
              "/methodologies", {{"/id", "M-8"}, {"/runnable", true}})}}},
        {"compile next-best-action methodology",
         "compile_intent",
         {{"project", "offer-plan"},
          {"input_id", "$input"},
          {"methodology", "M-8"},
          {"hints", hints},
          {"name", "offer recommendations"}},
         {.checks = {journey::equals("/methodology", "M-8"),
                     journey::equals("/runnable", true)}},
         {{"graph", "/name"}}},
        {"compile primary solution",
         "compile_intent",
         {{"project", "offer-plan"},
          {"input_id", "$input"},
          {"solution", "S-11"},
          {"hints", hints},
          {"name", "solution recommendations"}},
         {.checks = {journey::equals("/methodology", "M-8"),
                     journey::equals("/runnable", true)}}},
        {"run pipeline table",
         "run",
         {{"project", "offer-plan"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/engine", "next_best_action"),
                     journey::equals("/dataset/source", "offers.candidates")}},
         {{"run", "/run_id"}}},
        {"wait for recommendation outputs",
         "get_run",
         {{"run_id", "$run"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::contains_element("/lineage/sources",
                                               "dataset:offers.candidates"),
                     journey::any_element("/outputs", {{"/kind", "dataset"}}),
                     journey::any_element("/outputs", {{"/kind", "model"}})}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read recommendation report",
         "get_run_report",
         {{"run_id", "$run"}, {"limit", 20}},
         {.checks = {journey::equals("/model/kind", "next_best_action"),
                     journey::exists("/model/metrics/hit_rate_at_k"),
                     journey::exists(
                         "/model/metrics/inverse_propensity_value"),
                     journey::equals("/dataset/catalog_rows", 192),
                     journey::any_element("/dataset/not_used",
                                          {{"/column", "unused_note"}}),
                     journey::exists("/policy/effective_sample_size"),
                     journey::exists("/recommendations/0/expected_value"),
                     journey::exists("/recommendations/0/why"),
                     journey::size_equals("/outputs", 4)}}}};
    const auto result = journey::run_journey(recommendation, caller(), pump());
    std::string run_diagnostics;
    for (const auto& run : node.runs()) {
        if (run) {
            run_diagnostics += "\n" + run->metadata().dump();
        }
    }
    EXPECT_TRUE(result.ok) << result.summary() << run_diagnostics;
}

TEST_F(Uc1Journey, JourneyLearningToRankCompilesRunsAndReports) {
    std::ostringstream rows;
    for (std::int64_t group = 0; group < 24; ++group) {
        for (std::int64_t position = 0; position < 8; ++position) {
            rows << nlohmann::json{{"lead_id", group * 100 + position},
                                   {"rep_id", group},
                                   {"relevance", position / 2},
                                   {"intent", static_cast<double>(position)},
                                   {"fit", static_cast<double>((group + position) % 5)}}
                        .dump()
                 << '\n';
        }
    }

    journey::Journey ranking{"learning-to-rank", {}};
    ranking.steps = {
        {"land grouped ranking candidates", "upload_dataset",
         {{"database", "sales"},
          {"table_name", "leads"},
          {"format", "jsonl"},
          {"content", rows.str()}},
         {.checks = {journey::equals("/rows", 192)}}},
        {"create the project", "create_project", {{"name", "rank-leads"}}},
        {"capture the ranking intent", "capture_intent",
         {{"project", "rank-leads"},
          {"text", "Prioritize each representative's leads"}},
         {.checks = {journey::not_empty("/input_id")}},
         {{"input_id", "/input_id"}}},
        {"compile the M-9 pipeline", "compile_intent",
         {{"project", "rank-leads"},
          {"input_id", "$input_id"},
          {"methodology", "M-9"},
          {"name", "lead priority"},
          {"hints",
           {{"source", "sales"},
            {"table", "leads"},
            {"entity", "lead_id"},
            {"group", "rep_id"},
            {"label", "relevance"},
            {"features", {"intent", "fit"}},
            {"cutoff", 5}}}},
         {.checks = {journey::equals("/methodology", "M-9"),
                     journey::equals("/runnable", true)}},
         {{"graph", "/name"}}},
        {"run the compiled ranking pipeline", "run",
         {{"project", "rank-leads"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/engine", "rank"),
                     journey::not_empty("/run_id")}},
         {{"run_id", "/run_id"}}},
        {"wait for the rank run", "get_run", {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::any_element(
                         "/outputs", {{"/name", "ranked queue"}})}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read ranking metrics and the queue", "get_run_report",
         {{"run_id", "$run_id"}, {"limit", 10}},
         {.checks = {journey::exists("/model/ndcg_at_k"),
                     journey::exists("/model/yield_at_k"),
                     journey::equals("/dataset/source", "sales.leads"),
                     journey::contains_element("/dataset/excluded", "rep_id"),
                     journey::size_equals("/ranked_queue", 10)}}},
    };

    const journey::JourneyReport report =
        journey::run_journey(ranking, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey, VisionMethodologyCompilesRunsAndReportsItsTables) {
    ASSERT_TRUE(node.create_database("datasets"));
    const std::shared_ptr<zetta::data::Database> database =
        node.find_database("datasets");
    ASSERT_TRUE(database);
    std::vector<std::string> image_refs;
    for (int index = 0; index < 12; ++index) {
        const auto image = database->large_objects().import_bytes(
            "frame-" + std::to_string(index) + ".png", vision_png());
        ASSERT_TRUE(image);
        image_refs.push_back(image->id);
    }

    const zetta::ml::VitConfig config = {.image_width = 4,
                                         .image_height = 4,
                                         .patch_size = 2,
                                         .channels = 3,
                                         .embedding_size = 4,
                                         .attention_heads = 2,
                                         .encoder_layers = 1,
                                         .mlp_size = 6};
    const zetta::ml::VitBackbone backbone =
        zetta::ml::random_vit_backbone(config, 101);
    zetta::ml::ModelValue backbone_value;
    backbone_value.name = "journey-vit";
    backbone_value.kind = "vision_backbone";
    backbone_value.feature_schema = {{"vit_config", zetta::ml::to_json(config)},
                                     {"weights_origin", "random"}};
    const std::shared_ptr<zetta::ml::Model> backbone_model =
        node.create_model(std::move(backbone_value));
    ASSERT_TRUE(backbone_model);
    ASSERT_TRUE(backbone_model->set_weights(
        zetta::autograd::serialize_safetensors(backbone.tensors)));

    std::string content;
    for (int index = 0; index < 12; ++index) {
        content += nlohmann::json{{"image", image_refs[index]},
                                  {"label", index % 2},
                                  {"source",
                                   "camera-" + std::to_string(index / 3)}}
                       .dump() +
                   "\n";
    }
    const ApiResult uploaded =
        call("upload_dataset", {{"table_name", "inspection_frames"},
                                {"format", "jsonl"},
                                {"database", "datasets"},
                                {"replication_factor", 1},
                                {"content", content}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    EXPECT_EQ(uploaded.value.at("rows"), 12u);

    ASSERT_TRUE(call("create_project", {{"name", "inspection"}}).ok);
    const ApiResult captured =
        call("capture_intent",
             {{"project", "inspection"},
              {"text", "Classify defects in governed inspection images"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult by_solution =
        call("compile_intent", {{"project", "inspection"},
                                {"input_id", captured.value.at("input_id")},
                                {"solution", "S-8"},
                                {"name", "maintenance primary solution"}});
    ASSERT_TRUE(by_solution.ok) << by_solution.error.message;
    EXPECT_EQ(by_solution.value.at("methodology"), "M-7");
    EXPECT_TRUE(by_solution.value.at("runnable"));

    const ApiResult compiled =
        call("compile_intent", {{"project", "inspection"},
                                {"input_id", captured.value.at("input_id")},
                                {"methodology", "M-13"},
                                {"name", "vision classifier"},
                                {"hints",
                                 {{"database", "datasets"},
                                  {"table", "inspection_frames"},
                                  {"backbone_model", backbone_model->id()},
                                  {"epochs", 12},
                                  {"seed", 7}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-13");
    EXPECT_TRUE(compiled.value.at("runnable"));

    const ApiResult ran =
        call("run", {{"project", "inspection"},
                     {"graph", "vision classifier"},
                     {"model_name", "inspection-vision"}});
    ASSERT_TRUE(ran.ok) << ran.error.message;
    EXPECT_EQ(ran.value.at("engine"), "vision");
    const std::string run_id = ran.value.at("run_id");

    const ApiResult run = call("get_run", {{"run_id", run_id}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("status"), "succeeded");
    EXPECT_EQ(run.value.at("outputs").size(), 3u);

    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_TRUE(report.value.at("model").contains("accuracy"));
    EXPECT_TRUE(report.value.at("model").contains("macro_f1"));
    EXPECT_EQ(report.value.at("dataset").at("images_read"), 12u);
    EXPECT_EQ(report.value.at("dataset").at("images_excluded"), 0u);
    EXPECT_FALSE(report.value.at("confusion_matrix").empty());
    ASSERT_EQ(report.value.at("output_tables").size(), 1u);
    EXPECT_TRUE(report.value.at("output_tables")
                    .front()
                    .get<std::string>()
                    .starts_with("vision_results.predictions_"));
}

TEST_F(Uc1Journey, JourneyDocumentMethodologyLandsCompilesRunsAndReports) {
    const std::string document =
        "Invoice Number: INV-42\nDate: 2026-09-27\nTotal: 149.95";
    const nlohmann::json row = {
        {"document_id", "invoice-1"},
        {"document", document},
        {"labels",
         {{"invoice_number", "INV-42"},
          {"date", "2026-09-27"},
          {"total", "149.95"}}}};
    const ApiResult landed = call(
        "upload_dataset",
        {{"database", "documents"},
         {"table_name", "documents"},
         {"format", "jsonl"},
         {"content", row.dump() + "\n"}});
    ASSERT_TRUE(landed.ok) << landed.error.message;

    ASSERT_TRUE(call("create_project", {{"name", "document project"}}).ok);
    const ApiResult captured = call(
        "capture_intent",
        {{"project", "document project"},
         {"text", "Extract invoice fields and route uncertain values for review"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "document project"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-11"},
         {"name", "invoice extraction"},
         {"hints",
          {{"source", "documents"},
           {"table", "documents"},
           {"fields", {"invoice_number", "date", "total"}},
           {"review_threshold", 0.8}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-11");
    EXPECT_TRUE(compiled.value.at("runnable"));

    const ApiResult submitted = call(
        "run", {{"project", "document project"},
                {"graph", "invoice extraction"}});
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    const std::string run_id = submitted.value.at("run_id");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    ApiResult run;
    while (std::chrono::steady_clock::now() < deadline) {
        node.tick();
        background.prune();
        run = call("get_run", {{"run_id", run_id}});
        ASSERT_TRUE(run.ok) << run.error.message;
        if (run.value.at("status") == "succeeded" ||
            run.value.at("status") == "failed") {
            break;
        }
        std::this_thread::yield();
    }
    ASSERT_EQ(run.value.at("status"), "succeeded") << run.value.dump(2);

    const ApiResult report =
        call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("model").at("kind"), "document_extraction");
    EXPECT_DOUBLE_EQ(
        report.value.at("model").at("metrics").at("field_f1").get<double>(),
        1.0);
    EXPECT_EQ(report.value.at("fields").size(), 3u);
    EXPECT_TRUE(report.value.at("review_queue").contains("table"));
    EXPECT_EQ(report.value.at("dataset").at("source"),
              "documents.documents");
}

TEST_F(Uc1Journey, JourneyLanguageMethodologyCompilesRunsAndReports) {
    const std::string content =
        "{\"id\":1,\"text\":\"Cats are good pets.\",\"label\":1,"
        "\"query\":\"Cats are good pets.\",\"relevant_id\":1}\n"
        "{\"id\":2,\"text\":\"Dogs are good pets.\",\"label\":0,"
        "\"query\":\"Dogs are good pets.\",\"relevant_id\":2}\n";
    const ApiResult uploaded =
        call("upload_dataset", {{"database", "knowledge"},
                                {"table_name", "chunks"},
                                {"format", "jsonl"},
                                {"content", content},
                                {"replication_factor", 1}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;

    std::string checkpoint_error;
    const std::optional<zetta::ml::WordPieceTokenizer> tokenizer =
        zetta::ml::WordPieceTokenizer::from_vocabulary(
            {"[PAD]", "[UNK]", "[CLS]", "[SEP]", "cats", "dogs", "are",
             "good", "pet", "##s", "."},
            true, &checkpoint_error);
    ASSERT_TRUE(tokenizer.has_value()) << checkpoint_error;
    zetta::ml::BertConfig config;
    config.vocab_size = 11;
    zetta::ml::BertTextEmbedder embedder(config, *tokenizer, 1049);
    const std::string checkpoint = (dir.path() / "minilm").string();
    ASSERT_TRUE(embedder.save_checkpoint(checkpoint, &checkpoint_error))
        << checkpoint_error;

    ASSERT_TRUE(call("create_project", {{"name", "language"}}).ok);
    const ApiResult captured = call(
        "capture_intent",
        {{"project", "language"},
         {"text", "Answer questions from our documents with citations"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "language"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-10"},
         {"name", "grounded answers"},
         {"hints", {{"dataset_path", "knowledge.chunks"}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-10");
    EXPECT_TRUE(compiled.value.at("runnable"));
    ASSERT_TRUE(call("approve_plan", {{"project", "language"},
                                      {"graph", "grounded answers"}})
                    .ok);

    const ApiResult executed = call(
        "run",
        {{"project", "language"},
         {"graph", "grounded answers"},
         {"embedder_directory", checkpoint},
         {"top_k", 1},
         {"minimum_relevance", -1.0},
         {"dataset",
          {{"entity_column", "id"},
           {"label_column", "label"},
           {"feature_columns", {"text"}}}}});
    ASSERT_TRUE(executed.ok) << executed.error.message;
    EXPECT_EQ(executed.value.at("engine"), "language");
    EXPECT_EQ(executed.value.at("status"), "succeeded");
    const std::string run_id = executed.value.at("run_id");

    const ApiResult run = call("get_run", {{"run_id", run_id}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("status"), "succeeded");
    EXPECT_EQ(run.value.at("outputs").size(), 5U);

    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("model").at("recall_at_k"), 1.0);
    EXPECT_EQ(report.value.at("model").at("groundedness"), 1.0);
    EXPECT_EQ(report.value.at("dataset").at("rows_read"), 2.0);
    EXPECT_EQ(report.value.at("outputs").size(), 3U);

    ASSERT_TRUE(node.create_role("language-reader"));
    ASSERT_TRUE(node.grant_permissions(
        "language-reader", {"mcp:call", "pipelines:manage"}));
    ASSERT_TRUE(node.grant_scoped_permissions(
        "language-reader", {"sql:read"},
        zetta::governance::Resource::column(
            "knowledge", "chunks", {"id", "text", "label", "relevant_id"})));
    const auto restricted = node.create_user("restricted-language-reader");
    ASSERT_TRUE(restricted);
    ASSERT_TRUE(node.assign_role(
        restricted->id(), node.find_role("language-reader")->id()));
    const ApiResult denied = call_as_user(
        restricted->id(), "run",
        {{"project", "language"},
         {"graph", "grounded answers"},
         {"embedder_directory", checkpoint},
         {"dataset",
          {{"entity_column", "id"},
           {"label_column", "label"},
           {"feature_columns", {"text"}}}}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    EXPECT_NE(denied.error.message.find("every requested training column"),
              std::string::npos);
}

TEST_F(Uc1Journey, JourneySpeechMethodologyCompilesRunsAndReports) {
  auto database = node.create_database("media");
  ASSERT_TRUE(database);
  const auto audio =
      database->large_objects().import_bytes("tone.wav", speech_test_wav());
  ASSERT_TRUE(audio);

  const fs::path source = fs::path(dir.path()) / "recordings.jsonl";
  {
    std::ofstream output(source);
    output << nlohmann::json{{"recording", "call-1"},
                             {"audio", audio->id},
                             {"consent", true},
                             {"reference_text", "test"}}
                  .dump()
           << '\n';
  }
  const ApiResult landed = call("add_dataset", {{"path", source.string()},
                                                {"database", "media"},
                                                {"table_name", "recordings"},
                                                {"partition_count", 1},
                                                {"replication_factor", 1}});
  ASSERT_TRUE(landed.ok) << landed.error.message;

  zetta::ml::WhisperConfig config;
  config.mel_bins = 8;
  config.model_width = 8;
  config.encoder_heads = 2;
  config.encoder_feed_forward = 16;
  config.decoder_heads = 2;
  config.decoder_feed_forward = 16;
  config.max_source_positions = 32;
  config.max_target_positions = 8;
  config.vocabulary_size = 12;
  config.timestamp_begin = 8;
  auto checkpoint = zetta::ml::random_whisper_checkpoint(config, 17);
  zetta::ml::ModelValue value;
  value.name = "whisper-test";
  value.kind = "speech_backbone";
  value.feature_schema = {{"whisper_config", zetta::ml::to_json(config)},
                          {"weights_origin", "random"},
                          {"test_vocabulary", checkpoint.test_vocabulary}};
  const auto model = node.create_model(std::move(value));
  ASSERT_TRUE(model);
  ASSERT_TRUE(model->set_weights(
      zetta::autograd::serialize_safetensors(checkpoint.tensors)));

  ASSERT_TRUE(call("create_project", {{"name", "speech journey"}}).ok);
  const ApiResult captured =
      call("capture_intent", {{"project", "speech journey"},
                              {"text", "Transcribe consented recordings"}});
  ASSERT_TRUE(captured.ok) << captured.error.message;
  const ApiResult compiled =
      call("compile_intent", {{"project", "speech journey"},
                              {"input_id", captured.value.at("input_id")},
                              {"methodology", "M-14"},
                              {"name", "speech pipeline"},
                              {"hints",
                               {{"database", "media"},
                                {"table", "recordings"},
                                {"checkpoint_model", model->id()},
                                {"maximum_new_tokens", 2}}}});
  ASSERT_TRUE(compiled.ok) << compiled.error.message;
  EXPECT_EQ(compiled.value.at("methodology"), "M-14");
  EXPECT_TRUE(compiled.value.at("runnable"));

  const ApiResult run = call("run", {{"project", "speech journey"},
                                     {"graph", "speech pipeline"},
                                     {"model_name", "journey transcript"}});
  ASSERT_TRUE(run.ok) << run.error.message;
  EXPECT_EQ(run.value.at("status"), "succeeded");
  const ApiResult run_view =
      call("get_run", {{"run_id", run.value.at("run_id")}});
  ASSERT_TRUE(run_view.ok) << run_view.error.message;
  EXPECT_EQ(run_view.value.at("status"), "succeeded");

  const ApiResult report =
      call("get_run_report", {{"run_id", run.value.at("run_id")}});
  ASSERT_TRUE(report.ok) << report.error.message;
  EXPECT_TRUE(report.value.at("model").contains("word_error_rate"));
  const auto &features = report.value.at("features");
  ASSERT_EQ(features.at("used").size(), 1u);
  EXPECT_EQ(features.at("used").front().at("name"), "audio_log_mel");
  EXPECT_EQ(features.at("used").front().at("source"), nlohmann::json::array({"audio"}));
  for (const auto *column : {"recording", "reference_text", "consent"}) {
    EXPECT_TRUE(std::ranges::any_of(features.at("excluded"), [&](const auto &entry) {
      return entry.at("name") == column && !entry.at("why").template get<std::string>().empty();
    })) << column;
  }
  ASSERT_EQ(report.value.at("output_tables").size(), 1u);
  EXPECT_TRUE(report.value.at("output_tables")
                  .front()
                  .get<std::string>()
                  .starts_with("speech_results.transcripts_"));
  ASSERT_FALSE(report.value.at("transcripts").empty());
  const auto &transcript = report.value.at("transcripts").front();
  EXPECT_TRUE(transcript.contains("recording"));
  EXPECT_TRUE(transcript.contains("segment"));
  EXPECT_TRUE(transcript.contains("start"));
  EXPECT_TRUE(transcript.contains("end"));
  EXPECT_TRUE(transcript.contains("text"));
  EXPECT_TRUE(transcript.contains("confidence"));
}

TEST_F(Uc1Journey, JourneyM15CompilesRunsAndReportsItsOwnTable) {
    const auto database = node.create_database("experiments");
    ASSERT_TRUE(database);
    const auto decisions = database->create_table("decisions");
    ASSERT_TRUE(decisions);
    for (int index = 0; index < 240; ++index) {
        const bool best = index % 3 != 0;
        decisions->add_row({{"decision_id", index},
                            {"arm", best ? "best" : "control"},
                            {"reward", best ? 1 : 0},
                            {"context", static_cast<double>(index % 5)}});
    }

    journey::Journey bandit{"m15-bandit", {}};
    bandit.steps = {
        {"create the adaptive-allocation project", "create_project",
         {{"name", "adaptive offers"}}},
        {"capture its goal", "capture_intent",
         {{"project", "adaptive offers"},
          {"text", "Adaptively allocate offers while learning"}},
         {.checks = {journey::not_empty("/input_id")}},
         {{"input_id", "/input_id"}}},
        {"compile M-15 against the pipeline's own table", "compile_intent",
         {{"project", "adaptive offers"},
          {"input_id", "$input_id"},
          {"methodology", "M-15"},
          {"name", "adaptive policy"},
          {"hints", {{"source", "experiments"},
                     {"table", "decisions"},
                     {"arms", {"control", "best"}}}}},
         {.checks = {journey::equals("/methodology", "M-15"),
                     journey::equals("/runnable", true)}},
         {{"graph", "/name"}}},
        {"approve the proposed policy plan", "approve_plan",
         {{"project", "adaptive offers"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/approval", "approved")}}},
        {"the bandit correctness checks pass", "gate",
         {{"project", "adaptive offers"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/block_deploy", false)}}},
        {"run the policy from its connect-step table", "run",
         {{"project", "adaptive offers"},
          {"graph", "$graph"},
          {"arms", {"control", "best"}},
          {"baseline_arm", "best"},
          {"model_name", "journey_bandit"}},
         {.checks = {journey::equals("/engine", "bandit"),
                     journey::equals("/status", "succeeded")}},
         {{"run_id", "/run_id"}}},
        {"read back the completed run", "get_run",
         {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_at_least("/outputs", 4)}}},
        {"read back policy metrics and output tables", "get_run_report",
         {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/run/status", "succeeded"),
                     journey::at_least("/metrics/cumulative_reward", 1),
                     journey::not_empty("/policy_table"),
                     journey::not_empty("/decisions_table"),
                     journey::any_element("/features/used", {{"/name", "arm_policy_statistics"},
                         {"/source", nlohmann::json::array({"arm", "reward"})}}),
                     journey::any_element("/features/excluded", {{"/name", "reward"}}),
                     journey::any_element("/features/excluded", {{"/name", "decision_id"}}),
                     journey::any_element("/features/excluded", {{"/name", "context"}}),
                     journey::equals("/dataset/source",
                                     "experiments.decisions")}}},
    };
    const journey::JourneyReport report =
        journey::run_journey(bandit, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey, FeaturePlanBanditContextMatchesTheNativePolicy) {
    const auto database = node.create_database("bandit_features");
    const auto table = database->create_table("decisions");
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"decision_id", index + 1}, {"arm", index % 2 ? "a" : "b"},
                       {"reward", index % 3 == 0 ? 1 : 0}, {"context", index % 7}});
    }
    const auto project = node.create_project("bandit feature choices");
    const auto *methodology = zetta::ml::find_methodology("M-15");
    ASSERT_NE(methodology, nullptr);
    for (const auto *algorithm : {"linucb", "linear_thompson"}) {
        SCOPED_TRACE(algorithm);
        zetta::ml::ShapeRequest shape;
        shape.hints = {{"source", "bandit_features"}, {"table", "decisions"},
                       {"algorithm", algorithm}};
        const auto graph = project->create_graph(algorithm, methodology->build_shape(shape));
        ASSERT_TRUE(graph);
        ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
        zetta::ml::RunRequest request{&context, project.get(), graph.get(),
                                     {{"model_name", algorithm}}};
        if (std::string(algorithm) == "linear_thompson") {
            request.input["context_columns"] = {"context"};
        }
        const auto run = methodology->run(request);
        ASSERT_TRUE(run.ok) << run.error.message;
        const auto report = call("get_run_report", {{"run_id", run.value.at("run_id")}});
        ASSERT_TRUE(report.ok) << report.error.message;
        const auto &features = report.value.at("features");
        EXPECT_EQ(features.at("used").size(), 2u);
        EXPECT_TRUE(std::ranges::any_of(features.at("used"), [](const auto &entry) {
            return entry.at("name") == "context" &&
                   entry.at("source") == nlohmann::json::array({"context"});
        }));
        for (const auto *column : {"decision_id", "reward"}) {
            EXPECT_TRUE(std::ranges::any_of(features.at("excluded"), [&](const auto &entry) {
                return entry.at("name") == column;
            }));
        }
    }
}

TEST_F(Uc1Journey, FeaturePlanBanditRefusesLeakedContextBeforeCreatingRun) {
    const auto database = node.create_database("unsafe_bandit");
    const auto table = database->create_table("decisions");
    for (int index = 0; index < 120; ++index) {
        table->add_row({{"decision_id", index + 1}, {"arm", index % 2 ? "a" : "b"},
                       {"reward", index % 3 == 0 ? 1 : 0},
                       {"reward_copy", index % 3 == 0 ? 1 : 0}, {"context", index % 7}});
    }
    const auto project = node.create_project("bandit leakage refusal");
    const auto *methodology = zetta::ml::find_methodology("M-15");
    ASSERT_NE(methodology, nullptr);
    zetta::ml::ShapeRequest shape;
    shape.hints = {{"source", "unsafe_bandit"}, {"table", "decisions"},
                   {"algorithm", "linucb"}};
    const auto graph = project->create_graph("policy", methodology->build_shape(shape));
    ASSERT_TRUE(graph);
    ApiContext context{node, &cluster, "", trace, &node.span_recorder(), &background};
    for (const auto *unsafe : {"reward_copy", "reward", "decision_id"}) {
        SCOPED_TRACE(unsafe);
        zetta::ml::RunRequest request{&context, project.get(), graph.get(),
                                     {{"context_columns", {"context", unsafe}}}};
        const auto result = methodology->run(request);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "invalid_input");
        EXPECT_NE(result.error.message.find(unsafe), std::string::npos)
            << result.error.message;
    }
    EXPECT_TRUE(node.runs().empty());
    EXPECT_TRUE(node.models().empty());
}

TEST_F(Uc1Journey, JourneyM16CompilesRunsAndReportsAFeasiblePlan) {
  const std::string rows =
      R"({"row_id":1,"variable":"item_a","objective":10,"lower_bound":0,"upper_bound":1,"integer":true,"current_value":0,"scenario":"base","constraint":"weight","coefficient":4,"sense":"<=","rhs":5})"
      "\n"
      R"({"row_id":2,"variable":"item_b","objective":6,"lower_bound":0,"upper_bound":1,"integer":true,"current_value":1,"scenario":"base","constraint":"weight","coefficient":2,"sense":"<=","rhs":5})"
      "\n"
      R"({"row_id":3,"variable":"item_c","objective":4,"lower_bound":0,"upper_bound":1,"integer":true,"current_value":1,"scenario":"base","constraint":"weight","coefficient":1,"sense":"<=","rhs":5})"
      "\n";

  journey::Journey optimization{"m16-optimization", {}};
  optimization.steps = {
      {"land the scenario and constraint table",
       "upload_dataset",
       {{"table_name", "knapsack"},
        {"database", "operations"},
        {"format", "jsonl"},
        {"replication_factor", 1},
        {"content", rows}},
       {.checks = {journey::equals("/database", "operations"),
                   journey::equals("/table", "knapsack"),
                   journey::equals("/rows", 3)}}},
      {"create the optimization project",
       "create_project",
       {{"name", "allocation"}}},
      {"capture the allocation goal",
       "capture_intent",
       {{"project", "allocation"},
        {"text", "Optimize a knapsack allocation under capacity"}},
       {},
       {{"input_id", "/input_id"}}},
      {"compile the M-16 pipeline",
       "compile_intent",
       {{"project", "allocation"},
        {"input_id", "$input_id"},
        {"methodology", "M-16"},
        {"hints", {{"source", "operations"}, {"table", "knapsack"}}}},
       {.checks = {journey::equals("/methodology", "M-16"),
                   journey::equals("/runnable", true)}},
       {{"graph", "/name"}}},
      {"run against the pipeline table",
       "run",
       {{"project", "allocation"}, {"graph", "$graph"}},
       {.checks = {journey::equals("/engine", "optimization"),
                   journey::equals("/status", "succeeded"),
                   journey::equals("/solver_status", "optimal"),
                   journey::not_empty("/plan_table"),
                   journey::not_empty("/constraint_table")}},
       {{"run_id", "/run_id"}}},
      {"the durable run finished",
       "get_run",
       {{"run_id", "$run_id"}},
       {.checks = {journey::equals("/status", "succeeded")}},
       {},
       journey::Poll{"/status", {"succeeded", "failed"}}},
      {"the report carries objective, feasibility, gap, and tables",
       "get_run_report",
       {{"run_id", "$run_id"}},
       {.checks = {journey::equals("/model/solver_status", "optimal"),
                   journey::equals("/model/objective_value", 14.0),
                   journey::equals("/model/current_plan_objective", 10.0),
                   journey::equals("/model/feasible", 1.0),
                   journey::equals("/model/optimality_gap", 0.0),
                   journey::size_equals("/plan", 3),
                   journey::any_element("/features/used", {{"/name", "integer"},
                       {"/why", "integer is solver metadata, not a supervised label"}}),
                   journey::any_element("/features/used", {{"/name", "objective"}}),
                   journey::any_element("/features/used", {{"/name", "coefficient"}}),
                   journey::any_element("/features/excluded", {{"/name", "row_id"}}),
                   journey::any_element("/constraints", {{"/satisfied", true}}),
                   journey::equals("/dataset/source", "operations.knapsack")}}},
  };
  const journey::JourneyReport report =
      journey::run_journey(optimization, caller(), pump());
  EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(ActionJourneys, JourneyBayesianMethodologyCompilesRunsAndReports) {
    std::string content = "id,store,value\n";
    for (int index = 1; index <= 120; ++index) {
        const std::string store =
            index % 3 == 0 ? "north" : (index % 3 == 1 ? "south" : "west");
        const double center =
            store == "north" ? 8.0 : (store == "south" ? 12.0 : 18.0);
        content += std::to_string(index) + "," + store + "," +
                   std::to_string(center + static_cast<double>(index % 7) / 10.0) +
                   "\n";
    }
    const ApiResult uploaded =
        call("upload_dataset", {{"table_name", "store_demand"},
                                {"format", "csv"},
                                {"content", content}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    ASSERT_EQ(uploaded.value.at("database"), "datasets");
    ASSERT_EQ(uploaded.value.at("table"), "store_demand");

    ASSERT_TRUE(call("create_project", {{"name", "uncertain demand"}}).ok);
    const ApiResult captured = call(
        "capture_intent",
        {{"project", "uncertain demand"},
         {"text", "Estimate demand by store with honest uncertainty"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "uncertain demand"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-17"},
         {"name", "store posterior"},
         {"hints",
          {{"database", "datasets"},
           {"table", "store_demand"},
           {"entity_column", "id"},
           {"label_column", "value"},
           {"group_column", "store"},
           {"nominal_coverage", 0.9}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-17");
    EXPECT_TRUE(compiled.value.at("runnable"));

    const ApiResult started =
        call("run", {{"project", "uncertain demand"},
                     {"graph", "store posterior"},
                     {"holdout_fraction", 0.25},
                     {"seed", 1056},
                     {"burn_in", 100},
                     {"posterior_draws", 250}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value.at("engine"), "bayesian");
    EXPECT_EQ(started.value.at("status"), "succeeded");
    EXPECT_TRUE(started.value.contains("output_table"));

    const ApiResult run =
        call("get_run", {{"run_id", started.value.at("run_id")}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("status"), "succeeded");

    const ApiResult report =
        call("get_run_report", {{"run_id", started.value.at("run_id")}});
    ASSERT_TRUE(report.ok) << report.error.message;
    ASSERT_TRUE(report.value.contains("uncertainty"));
    const nlohmann::json& uncertainty = report.value.at("uncertainty");
    EXPECT_DOUBLE_EQ(uncertainty.at("nominal_coverage").get<double>(), 0.9);
    EXPECT_GE(uncertainty.at("empirical_coverage").get<double>(), 0.0);
    EXPECT_LE(uncertainty.at("empirical_coverage").get<double>(), 1.0);
    EXPECT_GT(uncertainty.at("mean_interval_width").get<double>(), 0.0);
    EXPECT_NE(uncertainty.at("assumption").get<std::string>().find(
                  "exchangeable"),
              std::string::npos);
    ASSERT_EQ(report.value.at("output_tables").size(), 1u);
    EXPECT_EQ(report.value.at("dataset").at("source"),
              "datasets.store_demand");
    const auto& features = report.value.at("features");
    ASSERT_EQ(features.at("used").size(), 1u);
    EXPECT_EQ(features.at("used").front().at("name"), "hierarchical_group_statistics");
    EXPECT_EQ(features.at("used").front().at("source"), nlohmann::json({"store", "value"}));
    for (const auto* role : {"id", "value"}) {
        EXPECT_TRUE(std::ranges::any_of(features.at("excluded"), [&](const auto& item) {
            return item.at("name") == role;
        }));
    }
}

TEST_F(Uc1Journey, JourneyEnsembleCompilesRunsAndReportsItsOutputs) {
    std::string content = "entity,first,second,label\n";
    for (int index = 0; index < 240; ++index) {
        const double first = static_cast<double>((index * 17) % 101) / 100.0;
        const double second = static_cast<double>((index * 43) % 103) / 102.0;
        const bool first_region = index % 2 == 0;
        const bool positive = first_region ? first > 0.48 : second > 0.52;
        content += std::to_string(index) + "," + std::to_string(first) + "," +
                   std::to_string(second) + "," +
                   std::to_string(positive ? 1 : 0) + "\n";
    }

    journey::Journey ensemble{"m18-ensemble-registry-run", {}};
    ensemble.steps = {
        {"land the training table", "upload_dataset",
         {{"table_name", "ensemble_training"},
          {"format", "csv"},
          {"content", content},
          {"replication_factor", 1}},
         {.checks = {journey::equals("/database", "datasets"),
                     journey::equals("/table", "ensemble_training"),
                     journey::equals("/rows", 240)}}},
        {"create the project", "create_project", {{"name", "ensemble journey"}}},
        {"capture the stacking goal", "capture_intent",
         {{"project", "ensemble journey"},
          {"text", "Blend complementary models for better decisions"}},
         {.checks = {journey::not_empty("/input_id")}},
         {{"input_id", "/input_id"}}},
        {"compile M-18 over the landed table", "compile_intent",
         {{"project", "ensemble journey"},
          {"input_id", "$input_id"},
          {"methodology", "M-18"},
          {"name", "ensemble pipeline"},
          {"hints",
           {{"dataset_path", "datasets.ensemble_training"},
            {"entity_column", "entity"},
            {"label_column", "label"},
            {"feature_columns", {"first", "second"}},
            {"folds", 4}}}},
         {.checks = {journey::equals("/methodology", "M-18"),
                     journey::equals("/runnable", true),
                     journey::equals("/steps", 4)}},
         {{"graph", "/name"}}},
        {"approve the ensemble plan", "approve_plan",
         {{"project", "ensemble journey"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/approval", "approved")}}},
        {"run the pipeline's own table", "run",
         {{"project", "ensemble journey"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/engine", "ensemble"),
                     journey::not_empty("/run_id")}},
         {{"run_id", "/run_id"}}},
        {"wait for the durable run", "get_run", {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_at_least("/outputs", 5)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read lift, weights, lineage, and result tables", "get_run_report",
         {{"run_id", "$run_id"}},
         {.checks = {
             journey::exists("/model/held_out_auc_lift_over_best_single"),
             journey::exists("/model/best_single_held_out_auc"),
             journey::size_equals("/meta_model_weights", 2),
             journey::size_equals("/features/used", 4),
             journey::any_element("/features/used", {{"/name", "first"}, {"/source", {"first"}}}),
             journey::any_element("/features/used", {{"/name", "second"}, {"/source", {"second"}}}),
             journey::any_element("/features/used", {{"/name", "oof_shallow_gbdt"},
                                                       {"/source", {"first", "second"}}}),
             journey::any_element("/features/excluded", {{"/name", "entity"}}),
             journey::any_element("/features/excluded", {{"/name", "label"}}),
             journey::size_equals("/output_tables", 2),
             journey::size_at_least("/at_risk", 1),
             journey::exists("/at_risk/0/risk"),
             journey::equals("/dataset/source", "datasets.ensemble_training")}}},
    };
    const journey::JourneyReport report =
        journey::run_journey(ensemble, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey, JourneyLabelEfficiencyCompilesAndRunsItsPipelineTable) {
    const fs::path file = fs::path(dir.path()) / "label_examples.jsonl";
    {
        std::ofstream out(file);
        for (int row = 0; row < 240; ++row) {
            const double signal =
                -1.0 + 2.0 * static_cast<double>(row) / 239.0;
            const int label = signal >= 0.17 ? 1 : 0;
            const int strong_vote = row % 10 == 0 ? 1 - label : label;
            const int medium_vote = row % 5 == 0 ? 1 - label : label;
            out << nlohmann::json{{"entity", row},
                                  {"signal", signal},
                                  {"strong_vote", strong_vote},
                                  {"medium_vote", medium_vote},
                                  {"label", label}}
                       .dump()
                << "\n";
        }
    }

    const nlohmann::json functions = nlohmann::json::array(
        {{{"name", "strong"},
          {"column", "strong_vote"},
          {"operator", "greater_equal"},
          {"value", 0.5},
          {"label", 1},
          {"otherwise_label", 0}},
         {{"name", "medium"},
          {"column", "medium_vote"},
          {"operator", "greater_equal"},
          {"value", 0.5},
          {"label", 1},
          {"otherwise_label", 0}}});

    journey::Journey label_efficiency{
        "label-efficiency-registry-run",
        {
            {"land the examples table",
             "add_dataset",
             {{"path", file.string()},
              {"database", "datasets"},
              {"table_name", "label_examples"},
              {"partition_count", 2}},
             {.checks = {journey::equals("/rows", 240)}}},
            {"create the project",
             "create_project",
             {{"name", "label factory"}},
             {.checks = {journey::equals("/name", "label factory")}}},
            {"capture the goal",
             "capture_intent",
             {{"project", "label factory"},
              {"text", "Use active learning and weak supervision to label rows"}},
             {}},
            {"compile the M-19 pipeline",
             "compile_intent",
             {{"project", "label factory"},
              {"methodology", "M-19"},
              {"name", "label factory pipeline"},
              {"hints",
               {{"dataset_path", "datasets.label_examples"},
                {"queue_size", 6},
                {"initial_labels", 3},
                {"gold_fraction", 0.25},
                {"seed", 1058},
                {"uncertainty", "entropy"},
                {"diversity_weight", 0.15},
                {"logistic_iterations", 8},
                {"logistic_learning_rate", 0.3},
                {"label_model_iterations", 20},
                {"labelling_functions", functions}}}},
             {.checks = {journey::equals("/methodology", "M-19"),
                         journey::equals("/runnable", true)}}},
            {"run the compiled pipeline against its connect-step table",
             "run",
             {{"project", "label factory"},
              {"graph", "label factory pipeline"}},
             {.checks = {journey::equals("/engine", "label_efficiency")}},
             {{"run_id", "/run_id"}}},
            {"wait for the label factory",
             "get_run",
             {{"run_id", "$run_id"}},
             {.checks = {journey::equals("/status", "succeeded"),
                         journey::size_equals("/outputs", 4)}},
             {},
             journey::Poll{"/status", {"succeeded", "failed"}}},
            {"read metrics and output tables",
             "get_run_report",
             {{"run_id", "$run_id"}},
             {.checks = {
                  journey::equals("/run/status", "succeeded"),
                  journey::size_equals("/output_tables", 2),
                  journey::equals("/correctness/weak_labels_are_gold", false),
                  journey::equals("/correctness/gold_split", "disjoint"),
                  journey::any_element("/features/used", {{"/name", "signal"}, {"/source", {"signal"}}}),
                  journey::any_element("/features/used", {{"/name", "weak_vote_strong"},
                                                           {"/source", {"strong_vote"}}}),
                  journey::any_element("/features/used", {{"/name", "weak_vote_medium"},
                                                           {"/source", {"medium_vote"}}}),
                  journey::any_element("/features/excluded", {{"/name", "entity"}}),
                  journey::any_element("/features/excluded", {{"/name", "label"}}),
                  journey::predicate(
                      "the report includes active and label-model accuracy",
                      [](const nlohmann::json& metrics) {
                          return metrics.contains("active_accuracy_at_budget") &&
                                 metrics.contains("random_accuracy_at_budget") &&
                                 metrics.contains("label_model_gold_accuracy");
                      },
                      "/model/metrics")}}},
        }};

    const journey::JourneyReport report = journey::run_journey(
        label_efficiency, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(Uc1Journey, JourneySyntheticPrivateCompilesRunsAndReports) {
    const fs::path file = fs::path(dir.path()) / "private_training.jsonl";
    {
        std::ofstream output(file);
        for (int index = 0; index < 160; ++index) {
            const double first = static_cast<double>(index) / 159.0;
            const double second =
                std::clamp(0.75 * first + 0.1 * std::sin(index), 0.0, 1.0);
            output << nlohmann::json{{"entity", index},
                                     {"first", first},
                                     {"second", second},
                                     {"label", first + second > 1.0 ? 1 : 0}}
                              .dump()
                   << '\n';
        }
    }

    const ApiResult landed =
        call("add_dataset", {{"path", file.string()},
                             {"database", "privacy_source"},
                             {"table_name", "records"},
                             {"partition_count", 4}});
    ASSERT_TRUE(landed.ok) << landed.error.message;
    ASSERT_TRUE(call("create_project", {{"name", "privacy"}}).ok);
    const ApiResult captured = call(
        "capture_intent",
        {{"project", "privacy"},
         {"text", "Create privacy-preserving synthetic training data"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const ApiResult compiled = call(
        "compile_intent",
        {{"project", "privacy"},
         {"input_id", captured.value.at("input_id")},
         {"methodology", "M-20"},
         {"name", "private synthesis"},
         {"hints", {{"source", "privacy_source"},
                    {"table", "records"},
                    {"epsilon", 4.0},
                    {"delta", 1e-6}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), "M-20");
    EXPECT_TRUE(compiled.value.at("runnable"));

    const ApiResult run = call(
        "run", {{"project", "privacy"}, {"graph", "private synthesis"},
                {"seed", 1059}, {"num_trees", 8}, {"max_depth", 2}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_EQ(run.value.at("engine"), "synthetic_private");
    const std::string run_id = run.value.at("run_id");

    const ApiResult record = call("get_run", {{"run_id", run_id}});
    ASSERT_TRUE(record.ok) << record.error.message;
    EXPECT_EQ(record.value.at("status"), "succeeded");
    EXPECT_EQ(record.value.at("outputs").size(), 3u);

    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("run").at("status"), "succeeded");
    EXPECT_TRUE(report.value.at("privacy").contains("membership_auc"));
    EXPECT_TRUE(report.value.at("privacy").contains("epsilon_spent"));
    EXPECT_EQ(report.value.at("dataset").at("source"),
              "privacy_source.records");
    ASSERT_EQ(report.value.at("outputs").size(), 3u);
    EXPECT_EQ(report.value.at("outputs").front().at("kind"), "dataset");
    const auto& features = report.value.at("features");
    ASSERT_EQ(features.at("used").size(), 2u);
    for (const auto* source : {"first", "second"}) {
        EXPECT_TRUE(std::ranges::any_of(features.at("used"), [&](const auto& item) {
            return item.at("name") == source && item.at("source") == nlohmann::json({source}) &&
                   item.at("transform").template get<std::string>().find("clipped to [0.000000, 1.000000]") != std::string::npos;
        }));
    }
    for (const auto* role : {"entity", "label"}) {
        EXPECT_TRUE(std::ranges::any_of(features.at("excluded"), [&](const auto& item) {
            return item.at("name") == role;
        }));
    }
}

TEST_F(ActionJourneys, JourneyStreamingMethodologyCompilesRunsAndReports) {
    const auto database = node.create_database("stream_journey");
    ASSERT_TRUE(database);
    const auto table = database->create_table("events");
    ASSERT_TRUE(table);
    for (std::size_t index = 0; index < 80; ++index) {
        const double signal = index % 2 == 0 ? 0.0 : 1.0;
        table->add_row({{"event_id", "journey-" + std::to_string(index)},
                        {"account_id", static_cast<std::int64_t>(index)},
                        {"event_time", static_cast<double>(index)},
                        {"amount", static_cast<double>(index % 7)},
                        {"fraud", signal},
                        {"label_time", static_cast<double>(index) + 0.5},
                        {"comment", "excluded text"}});
    }

    journey::Journey streaming{"streaming-methodology", {}};
    streaming.steps = {
        {"create the project", "create_project", {{"name", "live fraud"}}},
        {"capture the streaming intent", "capture_intent",
         {{"project", "live fraud"},
          {"text", "Score transaction events with online learning"}}},
        {"compile M-21 with its event contract", "compile_intent",
         {{"project", "live fraud"},
          {"methodology", "M-21"},
          {"name", "stream decisions"},
          {"hints",
           {{"source", "stream_journey"},
            {"table", "events"},
            {"entity_column", "account_id"},
            {"time_column", "event_time"},
            {"label_column", "fraud"},
            {"label_time_column", "label_time"},
            {"feature_columns", nlohmann::json::array({"amount"})},
            {"input_topic", "journey.events"},
            {"output_topic", "journey.scores"},
            {"window_duration", 1.0}}}},
         {.checks = {journey::equals("/methodology", "M-21"),
                     journey::equals("/runnable", true),
                     journey::equals("/name", "stream decisions")}}},
        {"approve the streaming plan", "approve_plan",
         {{"project", "live fraud"}, {"graph", "stream decisions"}},
         {.checks = {journey::equals("/approval", "approved")}}},
        {"the event-time policy passes the gate", "gate",
         {{"project", "live fraud"}, {"graph", "stream decisions"}},
         {.checks = {journey::equals("/block_deploy", false)}}},
        {"start the native streaming run", "run",
         {{"project", "live fraud"}, {"graph", "stream decisions"}},
         {.checks = {journey::equals("/engine", "streaming"),
                     journey::equals("/status", "running")}},
         {{"run_id", "/run_id"}}},
        {"wait for the durable run", "get_run", {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_at_least("/outputs", 3)}},
         {}, journey::Poll{"/status", {"succeeded", "failed"}}},
        {"read prequential metrics and output tables", "get_run_report",
         {{"run_id", "$run_id"}},
         {.checks = {
             journey::equals("/model/kind", "streaming"),
             journey::at_least("/model/evaluated_labels", 80),
             journey::size_at_least("/outputs", 3),
             journey::equals("/dataset/source", "stream_journey.events"),
             journey::size_equals("/features/used", 2),
             journey::any_element("/features/used", {{"/name", "window_count"},
                                                       {"/source", {"account_id", "event_time"}}}),
             journey::any_element("/features/used", {{"/name", "window_sum_amount"},
                                                       {"/source", {"account_id", "event_time", "amount"}}}),
             journey::any_element("/features/excluded", {{"/name", "fraud"}}),
             journey::any_element("/features/excluded", {{"/name", "account_id"}}),
             journey::any_element("/features/excluded", {{"/name", "label_time"}}),
             journey::any_element("/features/excluded", {{"/name", "event_time"}}),
             journey::contains("/dataset/not_used/0", "comment")}}},
        {"read scores back from the output topic", "receive_events",
         {{"topic", "journey.scores"}, {"max", 100}},
         {.checks = {journey::size_at_least("/events", 80)}}},
    };
    const journey::JourneyReport report =
        journey::run_journey(streaming, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

// =====================================================================
// Canonical journey 3 — dataset ingest: a local jsonl file becomes a
// partitioned table whose placement reads back through get_dataset AND the
// system.dataset_partitions view; audited as dataset_added.
// =====================================================================
TEST_F(ActionJourneys, JourneyAddDatasetPlacementReadBack) {
    const fs::path file = fs::path(dir.path()) / "accounts.jsonl";
    {
        std::ofstream out(file);
        for (int i = 0; i < 24; ++i) {
            out << R"({"account":")" << i << R"(","mrr":)" << (100 + i) << "}\n";
        }
    }

    const journey::JourneyReport report = journey::run_journey(
        journey::canonical::dataset_ingest(
            file.string(), (fs::path(dir.path()) / "nope.xlsx").string()),
        caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

// =====================================================================
// import_n8n → compile_n8n: the committed churn workflow fixture rides the
// storage boundary byte-for-byte, compiles to a gate-passing graph; invalid
// content is refused with the schema reason and stores nothing.
// =====================================================================
TEST_F(ActionJourneys, JourneyImportAndCompileN8nWorkflow) {
    std::ifstream fixture(fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn_workflow.json");
    ASSERT_TRUE(fixture.good()) << "missing churn_workflow.json fixture";
    std::string workflow((std::istreambuf_iterator<char>(fixture)),
                         std::istreambuf_iterator<char>());

    journey::Journey n8n{"n8n-import-compile", {}};
    n8n.steps = {
        {"create the project", "create_project", {{"name", "flows"}}},
        {"import the workflow file", "import_n8n",
         {{"project", "flows"},
          {"content", workflow},
          {"filename", "churn_workflow.json"}},
         {.checks = {journey::equals("/kind", "artifact"),
                     journey::not_empty("/input_id")}},
         {{"input_id", "/input_id"}}},
        {"compile the imported workflow", "compile_n8n",
         {{"project", "flows"}, {"input_id", "$input_id"}},
         {.checks = {journey::at_least("/steps", 1)}},
         {{"graph", "/name"}}},
        {"approve the proposed plan (#885)", "approve_plan",
         {{"project", "flows"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/approval", "approved")}}},
        {"the compiled graph passes the gate", "gate",
         {{"project", "flows"}, {"graph", "$graph"}},
         {.checks = {journey::equals("/block_deploy", false)}}},
        {"non-workflow JSON is refused at the import boundary", "import_n8n",
         {{"project", "flows"}, {"content", "{\"not\": 1}"}},
         {.ok = false, .error_code = "engine_error"}},
        {"nothing was stored on the reject", "get_project",
         {{"name", "flows"}},
         {.checks = {journey::size_equals("/inputs", 1)}}},
    };
    const journey::JourneyReport report = journey::run_journey(n8n, caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

// =====================================================================
// compile_python: a sim-Python pipeline lowers to a stored, gate-passing
// graph; a malformed source is refused with the frontend's reason.
// =====================================================================
TEST_F(ActionJourneys, JourneyCompilePythonSource) {
    journey::Journey python{"python-compile", {}};
    python.steps = {
        {"create the project", "create_project", {{"name", "py"}}},
        {"compile the sim-Python source", "compile_python",
         {{"project", "py"},
          {"source",
           "ds = load_dataset(\"accounts.parquet\")\n"
           "clean = ds.dropna()\n"
           "model = train(clean, target=\"churned\")\n"},
          {"name", "churn from python"}},
         {.checks = {journey::equals("/name", "churn from python"),
                     journey::at_least("/steps", 3)}}},
        {"approve the proposed plan (#885)", "approve_plan",
         {{"project", "py"}, {"graph", "churn from python"}},
         {.checks = {journey::equals("/approval", "approved")}}},
        {"the lowered graph passes the gate", "gate",
         {{"project", "py"}, {"graph", "churn from python"}},
         {.checks = {journey::equals("/block_deploy", false)}}},
        {"a malformed source is refused with the frontend's reason",
         "compile_python",
         {{"project", "py"}, {"source", "x = ("}},
         {.ok = false, .error_code = "engine_error"}},
    };
    const journey::JourneyReport report = journey::run_journey(python, caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

// =====================================================================
// A pipeline that is not churn-shaped runs structurally: it trains nothing,
// and says so on its answer and on the run (lake's UX review 2026-09-27, lake#251).
// =====================================================================
TEST_F(ActionJourneys, JourneyAStructuralRunSaysNothingWasTrained) {
    journey::Journey structural{"structural-run", {}};
    structural.steps = {
        {"create the project", "create_project", {{"name", "docs"}}},
        {"save a two-step search pipeline", "save_graph",
         {{"project", "docs"},
          {"graph", "search docs"},
          {"nodes", nlohmann::json::array({{{"id", "load"}, {"kind", "connect"}, {"name", "Load docs"}},
                                 {{"id", "find"}, {"kind", "index"}, {"name", "Index docs"}}})},
          {"edges", nlohmann::json::array({{{"from", "load"}, {"to", "find"}, {"kind", "data_flow"}}})}}},
        {"approve it", "approve_plan",
         {{"project", "docs"}, {"graph", "search docs"}},
         {.checks = {journey::equals("/approval", "approved")}}},
        {"run it with a dataset it will not read", "run",
         {{"project", "docs"}, {"graph", "search docs"}, {"dataset_path", "datasets.docs"}},
         {.checks = {journey::equals("/engine", "structural"),
                     journey::contains("/note", "without training"),
                     journey::contains("/note", "was not read")}},
         {{"run_id", "/run_id"}}},
        {"the run itself says so", "get_run",
         {{"run_id", "$run_id"}},
         {.checks = {journey::any_element("/logs", {{"/level", "warn"}})}}},
    };
    const journey::JourneyReport report = journey::run_journey(structural, caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

// =====================================================================
// testsuite_run: kind job — the durable Run id returns immediately while the
// case executes on a worker; status + the case's log land on the run.
// Unknown case / unknown option are rejected with path-qualified reasons.
// =====================================================================
TEST_F(ActionJourneys, JourneyTestkitRunReturnsHandleImmediately) {
    journey::Journey testkit{"testkit-submit-then-poll", {}};
    testkit.steps = {
        {"submit the case (the handle returns immediately)", "testsuite_run",
         {{"case", "compute.cpu.elementwise"}},
         {.checks = {journey::not_empty("/run_id"),
                     journey::equals("/status", "running")}},
         {{"run_id", "/run_id"}}},
        {"the handle is live before the case finishes", "get_run",
         {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/project", "testkit"),
                     journey::equals("/graph", "compute.cpu.elementwise")}}},
        {"poll the run to its terminal status", "get_run",
         {{"run_id", "$run_id"}},
         {.checks = {journey::equals("/status", "succeeded"),
                     journey::size_at_least("/logs", 2)}},
         {},
         journey::Poll{"/status", {"succeeded", "failed"}}},
        {"an unknown case is not_found", "testsuite_run",
         {{"case", "no.such.case"}},
         {.ok = false, .error_code = "not_found"}},
        {"an unknown option key carries the path-qualified reason",
         "testsuite_run",
         {{"case", "compute.cpu.elementwise"}, {"options", {{"bogus", 1}}}},
         {.ok = false,
          .error_code = "invalid_input",
          .error_path = "/options/bogus"}},
    };
    const journey::JourneyReport report =
        journey::run_journey(testkit, caller(), pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

// =====================================================================
// RULE-3 negative: a key without the orchestration capability is denied, and
// the denial (plus the untouched state) reads back over the local session.
// =====================================================================
TEST_F(ActionJourneys, JourneyDeniedKeyIsRefusedAndAudited) {
    auto principal = node.create_principal(
        "read-only agent", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    zetta::governance::CapabilitySet reads_only;
    reads_only.grant(zetta::governance::CapabilityDomain::Api, "query");
    auto key = principal->create_key(zetta::governance::unrestricted_scope(),
                                     std::move(reads_only));
    ASSERT_TRUE(key);
    const std::string actor = key->id();

    journey::Journey denied{"denied-key", {}};
    denied.steps = {
        {"a mutation under a read-only key is unauthorized", "create_project",
         {{"name", "sneaky"}},
         {.ok = false, .error_code = "unauthorized"}},
    };
    const journey::JourneyReport denied_report =
        journey::run_journey(denied, caller(actor));
    EXPECT_TRUE(denied_report.ok) << denied_report.summary();

    journey::Journey read_back{"denied-key-read-back", {}};
    read_back.steps = {
        {"nothing was created", "list_projects", nlohmann::json::object(),
         {.checks = {journey::size_equals("/projects", 0)}}},
        {"the denial is audited under the denied key", "sql",
         {{"statement", "SELECT * FROM system.audit;"}},
         {.checks = {journey::predicate(
              "a denied audit row exists for api:create_project by " + actor,
              [actor](const nlohmann::json& rows) {
                  for (const nlohmann::json& row : rows) {
                      if (row.value("target", std::string{}) ==
                              "api:create_project" &&
                          row.value("actor", std::string{}) == actor &&
                          !row.value("allowed", true)) {
                          return true;
                      }
                  }
                  return false;
              },
              "/rows")}}},
    };
    const journey::JourneyReport read_back_report =
        journey::run_journey(read_back, caller());
    EXPECT_TRUE(read_back_report.ok) << read_back_report.summary();
}


// =====================================================================
// A run given no dataset trains on the table its pipeline's connect step names
// (lake points a project's pipeline at a table; before, such a run trained on
// the built-in demo dataset and said nothing).
// =====================================================================
TEST_F(Uc1Journey, ARunWithoutADatasetReadsItsPipelinesTable) {
    std::vector<nlohmann::json> rows;
    for (std::int64_t index = 0; index < 120; ++index) {
        const std::int64_t failed = index % 5;
        rows.push_back({{"account_id", index + 1},
                        {"failed_payments", failed},
                        {"tenure_months", static_cast<double>(60 - index % 50)},
                        {"churned", (failed >= 3) != (index % 9 == 0) ? 1 : 0}});
    }
    zetta::data::AddDatasetOptions options;
    options.database = "datasets";
    options.table_name = "accounts";
    options.partitioned = true;
    options.members = {node.node_id()};
    ASSERT_TRUE(zetta::data::add_dataset_rows(node, rows, options).ok());

    ASSERT_TRUE(call("create_project", {{"name", "retention"}}).ok);
    const nlohmann::json nodes = nlohmann::json::array(
        {{{"id", "load"}, {"kind", "connect"}, {"name", "Load accounts"},
          {"attributes", {{"source", "datasets"}, {"table", "accounts"}}}},
         {{"id", "features"}, {"kind", "clean"}, {"name", "Build features"}},
         {{"id", "train"}, {"kind", "model"}, {"name", "Train"},
          {"attributes", {{"target", "churned"}, {"algo", "gbdt"}}}}});
    const nlohmann::json edges = nlohmann::json::array(
        {{{"from", "load"}, {"to", "features"}, {"kind", "data_flow"}},
         {{"from", "features"}, {"to", "train"}, {"kind", "data_flow"}}});
    const ApiResult saved = call("save_graph", {{"project", "retention"}, {"graph", "churn"},
                                                {"nodes", nodes}, {"edges", edges}});
    ASSERT_TRUE(saved.ok) << saved.error.message;
    ASSERT_TRUE(call("approve_plan", {{"project", "retention"}, {"graph", "churn"}}).ok);

    const ApiResult ran = call("run", {{"project", "retention"}, {"graph", "churn"}});
    ASSERT_TRUE(ran.ok) << ran.error.message;
    EXPECT_EQ(ran.value.value("engine", ""), "churn");
    ASSERT_TRUE(ran.value.contains("dataset_notes")) << ran.value.dump();
    EXPECT_NE(ran.value["dataset_notes"].dump().find(
                  "read from the pipeline's connect step 'Load accounts': datasets.accounts, "
                  "label 'churned'"),
              std::string::npos)
        << ran.value.dump();
    EXPECT_EQ(ran.value["dataset"].value("label_column", ""), "churned");

    // The run finishes (its worker joins before the node stops), trained on the table.
    const std::string run_id = ran.value.value("run_id", "");
    std::string status;
    for (int attempt = 0; attempt < 3000 && status != "succeeded" && status != "failed"; ++attempt) {
        node.tick();
        background.prune();
        const ApiResult read = call("get_run", {{"run_id", run_id}});
        status = read.ok ? read.value.value("status", "") : "";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(status, "succeeded");
    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value["dataset"].value("source", ""), "datasets.accounts")
        << report.value.dump();
}
