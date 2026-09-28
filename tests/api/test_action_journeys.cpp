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

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "journey.hpp"
#include "journeys_canonical.hpp"
#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
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
    EXPECT_FALSE(explicit_m3.ok);
    EXPECT_EQ(explicit_m3.error.code, "not_supported");
    EXPECT_NE(explicit_m3.error.message.find("M-3"), std::string::npos);
    EXPECT_NE(explicit_m3.error.message.find("1042"), std::string::npos);

    const ApiResult solution =
        call("compile_intent", {{"project", "portfolio"},
                                {"input_id", input_id},
                                {"solution", "S-1"},
                                {"name", "solution graph"}});
    ASSERT_TRUE(solution.ok) << solution.error.message;
    EXPECT_EQ(solution.value.at("methodology"), "M-1");
    EXPECT_TRUE(solution.value.at("runnable"));

    const ApiResult by_keyword = call(
        "compile_intent", {{"project", "portfolio"}, {"input_id", input_id}});
    EXPECT_FALSE(by_keyword.ok);
    EXPECT_EQ(by_keyword.error.code, "not_supported");
    EXPECT_NE(by_keyword.error.message.find("1042"), std::string::npos);

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
    EXPECT_FALSE(graph_view.value.at("runnable"));

    const ApiResult project_view = call("get_project", {{"name", "portfolio"}});
    ASSERT_TRUE(project_view.ok) << project_view.error.message;
    bool found = false;
    for (const nlohmann::json& entry : project_view.value.at("graphs")) {
        if (entry.at("name") == "segmentation graph") {
            found = true;
            EXPECT_EQ(entry.at("methodology"), "M-3");
            EXPECT_FALSE(entry.at("runnable"));
        }
    }
    EXPECT_TRUE(found);

    const ApiResult run = call(
        "run", {{"project", "portfolio"}, {"graph", "segmentation graph"}});
    EXPECT_FALSE(run.ok);
    EXPECT_EQ(run.error.code, "not_supported");
    EXPECT_NE(run.error.message.find("1042"), std::string::npos);
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
                     journey::equals("/dataset/source",
                                     "experiments.decisions")}}},
    };
    const journey::JourneyReport report =
        journey::run_journey(bandit, caller(), pump());
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
