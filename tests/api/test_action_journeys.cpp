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

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
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

}  // namespace

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
    EXPECT_EQ(listed.value.at("methodologies").front().at("id"), "M-1");
    EXPECT_TRUE(listed.value.at("methodologies").front().at("runnable"));
    EXPECT_EQ(listed.value.at("methodologies")[6].at("id"), "M-7");
    EXPECT_TRUE(listed.value.at("methodologies")[6].at("runnable"));
    EXPECT_EQ(listed.value.at("methodologies")[1].at("id"), "M-2");
    EXPECT_TRUE(listed.value.at("methodologies")[1].at("runnable"));

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
                                                     {"methodology", "M-21"}});
    EXPECT_FALSE(unavailable.ok);
    EXPECT_EQ(unavailable.error.code, "not_supported");
    EXPECT_NE(unavailable.error.message.find("1060"), std::string::npos);
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
           {"table", "causal_population"},
           {"entity", "entity"},
           {"treatment", "treatment"},
           {"outcome", "outcome"},
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
              "experiments.causal_population");
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
