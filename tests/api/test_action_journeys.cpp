// The governed-action-parity journeys (#617, docs/testing/mcp-journeys.md):
// the mutating half of the product loop driven through the capability
// registry exactly as an agent would over MCP — layer 1, the same handlers
// the MCP endpoint renders, no HTTP. Every mutating step gets a read-back;
// negative journeys (gate-blocked deploy, denied capability, schema-rejected
// input) are first-class.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace {

namespace fs = std::filesystem;

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

    // Patient terminal-status poll over the SAME read surface an agent uses:
    // tick (the owner drain point for gated worker writes), then get_run.
    nlohmann::json wait_for_terminal_run(const std::string& run_id,
                                         const std::string& key = {}) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::minutes(2);
        nlohmann::json last;
        while (std::chrono::steady_clock::now() < deadline) {
            node.tick();
            background.prune();
            auto fetched = call("get_run", {{"run_id", run_id}}, key);
            if (fetched.ok) {
                last = fetched.value;
                const std::string status = last.value("status", "");
                if (status == "succeeded" || status == "failed") return last;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return last;
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

// A leakage-annotated churn-ish graph — the gate-blocked fixture (the #309
// data_leakage check refuses a feature dated after the as_of cutoff).
zetta::ir::ExecutionGraph leakage_graph() {
    zetta::ir::ExecutionGraph graph;
    graph.add_node(zetta::ir::StepKind::Connect, "load accounts", {}, "n-load");
    graph.add_node(
        zetta::ir::StepKind::Model, "train churn model",
        nlohmann::json{
            {"as_of", "2024-01-01"},
            {"features",
             nlohmann::json::array({nlohmann::json{
                 {"name", "spend_after_cutoff"}, {"data_as_of", "2024-02-01"}}})}},
        "n-model");
    graph.add_edge("n-load", "n-model");
    return graph;
}

}  // namespace

// =====================================================================
// UC-1 end to end: project → intent → compile → gate → deploy → run →
// model + drivers read back — with a capability-scoped acting key, every
// action landing in the audit trail under that key (RULE-3).
// =====================================================================
TEST_F(Uc1Journey, JourneyUc1EndToEndWithActingKey) {
    // The acting key: orchestration rights for exactly the loop's actions,
    // plus api:query for the read-backs. Deny-by-default everywhere else.
    auto principal = node.create_principal(
        "uc1 agent", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    zetta::governance::CapabilitySet grants;
    grants.grant(zetta::governance::CapabilityDomain::Api, "query");
    for (const char* action : {"create_project", "capture_intent",
                               "compile_intent", "gate", "deploy", "run"}) {
        grants.grant(zetta::governance::CapabilityDomain::Orchestration, action);
    }
    auto key = principal->create_key(zetta::governance::unrestricted_scope(),
                                     std::move(grants));
    ASSERT_TRUE(key);

    // Step 1 — create the project.
    auto created =
        call("create_project", {{"name", "churn-q3"}}, key->id());
    ASSERT_TRUE(created.ok) << created.error.message;

    // Step 2 — capture the intent; read it back through get_project.
    auto captured = call(
        "capture_intent",
        {{"project", "churn-q3"},
         {"text", "Predict which customers will churn next quarter and rank "
                  "the at-risk accounts with their churn drivers"}},
        key->id());
    ASSERT_TRUE(captured.ok) << captured.error.message;
    const std::string input_id = captured.value.at("input_id");
    EXPECT_EQ(captured.value.at("kind"), "intent");
    auto detail = call("get_project", {{"name", "churn-q3"}}, key->id());
    ASSERT_TRUE(detail.ok) << detail.error.message;
    ASSERT_EQ(detail.value.at("inputs").size(), 1u);
    EXPECT_EQ(detail.value.at("inputs")[0].at("id"), input_id);

    // Step 3 — compile the intent: the churn template, stored + replicated.
    auto compiled = call("compile_intent",
                         {{"project", "churn-q3"}, {"input_id", input_id}},
                         key->id());
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    const std::string graph_name = compiled.value.at("name");
    EXPECT_EQ(graph_name, "churn skeleton");
    EXPECT_GE(compiled.value.at("steps").get<std::size_t>(), 6u);

    // Step 4 — the gate: the skeleton passes (report read back whole).
    auto gated = call("gate", {{"project", "churn-q3"}, {"graph", graph_name}},
                      key->id());
    ASSERT_TRUE(gated.ok) << gated.error.message;
    EXPECT_FALSE(gated.value.at("block_deploy").get<bool>());
    EXPECT_FALSE(gated.value.at("checks").empty());

    // Step 5 — deploy, read the placement back over sql (system.deployments).
    auto deployed = call(
        "deploy", {{"project", "churn-q3"}, {"graph", graph_name}}, key->id());
    ASSERT_TRUE(deployed.ok) << deployed.error.message;
    EXPECT_FALSE(deployed.value.at("deployment_id").get<std::string>().empty());
    auto deployments = call(
        "sql", {{"statement", "SELECT * FROM system.deployments;"}}, key->id());
    ASSERT_TRUE(deployments.ok) << deployments.error.message;
    EXPECT_EQ(deployments.value.at("rows").size(), 1u);

    // Step 6 — run: kind job — the durable Run id comes back immediately,
    // the churn engine executes on the live coordinator, progress rides the
    // read surface.
    auto submitted = call(
        "run",
        {{"project", "churn-q3"}, {"graph", graph_name}, {"top_k", 5}},
        key->id());
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    EXPECT_EQ(submitted.value.at("engine"), "churn");
    const std::string run_id = submitted.value.at("run_id");
    ASSERT_FALSE(run_id.empty());

    const nlohmann::json finished = wait_for_terminal_run(run_id, key->id());
    ASSERT_EQ(finished.value("status", ""), "succeeded") << finished.dump(2);
    EXPECT_FALSE(finished.at("logs").empty());

    // The ranked at-risk list + drivers read back: the run's outputs carry
    // the model, the score/rank index (the at-risk list handle), and the
    // drivers artifact; the registered model is readable via list_models.
    bool saw_model = false, saw_index = false, saw_drivers = false;
    for (const nlohmann::json& output : finished.at("outputs")) {
        const std::string kind = output.value("kind", "");
        const std::string ref = output.value("ref", "");
        saw_model |= kind == "model";
        saw_index |= kind == "index";
        saw_drivers |= ref.rfind("drivers:", 0) == 0;
    }
    EXPECT_TRUE(saw_model) << finished.at("outputs").dump(2);
    EXPECT_TRUE(saw_index) << finished.at("outputs").dump(2);
    EXPECT_TRUE(saw_drivers) << finished.at("outputs").dump(2);

    auto models = call("list_models", nlohmann::json::object(), key->id());
    ASSERT_TRUE(models.ok);
    ASSERT_FALSE(models.value.at("models").empty());
    EXPECT_EQ(models.value.at("models")[0].at("name"), "churn_model");

    // The real per-step compute landed as ComputeJobs (featurize → … → score).
    auto jobs = call("list_jobs", nlohmann::json::object(), key->id());
    ASSERT_TRUE(jobs.ok);
    EXPECT_GE(jobs.value.at("jobs").size(), 3u);

    // RULE-3 read back: every action of the loop is in the audit trail under
    // the acting key.
    for (const char* target :
         {"api:create_project", "api:capture_intent", "api:compile_intent",
          "api:gate", "api:deploy", "api:run"}) {
        bool audited = false;
        for (const auto& event : node.audit_events()) {
            if (event->value().target == target &&
                event->value().actor == key->id() && event->value().allowed) {
                audited = true;
                break;
            }
        }
        EXPECT_TRUE(audited) << "no allowed audit event for " << target
                             << " by " << key->id();
    }
}

// =====================================================================
// The gate-blocked negative: a leakage-annotated graph is refused over MCP
// exactly as in the GUI, with the failing check's reason — by gate, deploy,
// AND run; nothing is placed, nothing executes.
// =====================================================================
TEST_F(ActionJourneys, JourneyGateBlockedDeployAndRunCarryTheReason) {
    ASSERT_TRUE(call("create_project", {{"name", "leaky"}}).ok);
    auto project = node.find_project("leaky");
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->create_graph("leaky graph", leakage_graph()));

    auto gated = call("gate", {{"project", "leaky"}, {"graph", "leaky graph"}});
    ASSERT_TRUE(gated.ok) << gated.error.message;
    EXPECT_TRUE(gated.value.at("block_deploy").get<bool>());

    auto deployed =
        call("deploy", {{"project", "leaky"}, {"graph", "leaky graph"}});
    ASSERT_FALSE(deployed.ok);
    EXPECT_EQ(deployed.error.code, "engine_error");
    EXPECT_NE(deployed.error.message.find("gate blocked deploy"),
              std::string::npos);
    EXPECT_NE(deployed.error.message.find("data_leakage"), std::string::npos);
    EXPECT_NE(deployed.error.message.find("spend_after_cutoff"),
              std::string::npos)
        << deployed.error.message;
    EXPECT_EQ(node.deployment_count(), 0u);

    auto ran = call("run", {{"project", "leaky"}, {"graph", "leaky graph"}});
    ASSERT_FALSE(ran.ok);
    EXPECT_NE(ran.error.message.find("blocked by gate"), std::string::npos);
    EXPECT_EQ(node.run_count(), 0u);
}

// =====================================================================
// import_n8n → compile_n8n: the committed churn workflow fixture rides the
// storage boundary byte-for-byte, compiles to a gate-passing graph; invalid
// content is refused with the schema reason and stores nothing.
// =====================================================================
TEST_F(ActionJourneys, JourneyImportAndCompileN8nWorkflow) {
    ASSERT_TRUE(call("create_project", {{"name", "flows"}}).ok);

    std::ifstream fixture(fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn_workflow.json");
    ASSERT_TRUE(fixture.good()) << "missing churn_workflow.json fixture";
    std::string workflow((std::istreambuf_iterator<char>(fixture)),
                         std::istreambuf_iterator<char>());

    auto imported = call("import_n8n", {{"project", "flows"},
                                        {"content", workflow},
                                        {"filename", "churn_workflow.json"}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    const std::string input_id = imported.value.at("input_id");
    EXPECT_EQ(imported.value.at("kind"), "artifact");

    auto compiled =
        call("compile_n8n", {{"project", "flows"}, {"input_id", input_id}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_GT(compiled.value.at("steps").get<std::size_t>(), 0u);

    auto gated = call("gate", {{"project", "flows"},
                               {"graph", compiled.value.at("name")}});
    ASSERT_TRUE(gated.ok);
    EXPECT_FALSE(gated.value.at("block_deploy").get<bool>());

    // Negative: non-workflow JSON is refused at the import boundary.
    auto rejected = call("import_n8n",
                         {{"project", "flows"}, {"content", "{\"not\": 1}"}});
    ASSERT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error.code, "engine_error");
    auto detail = call("get_project", {{"name", "flows"}});
    ASSERT_TRUE(detail.ok);
    EXPECT_EQ(detail.value.at("inputs").size(), 1u) << "nothing stored on reject";
}

// =====================================================================
// compile_python: a sim-Python pipeline lowers to a stored, gate-passing
// graph; a malformed source is refused with the frontend's reason.
// =====================================================================
TEST_F(ActionJourneys, JourneyCompilePythonSource) {
    ASSERT_TRUE(call("create_project", {{"name", "py"}}).ok);

    const std::string source =
        "ds = load_dataset(\"accounts.parquet\")\n"
        "clean = ds.dropna()\n"
        "model = train(clean, target=\"churned\")\n";
    auto compiled = call("compile_python",
                         {{"project", "py"}, {"source", source},
                          {"name", "churn from python"}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("name"), "churn from python");
    EXPECT_GE(compiled.value.at("steps").get<std::size_t>(), 3u);

    auto gated =
        call("gate", {{"project", "py"}, {"graph", "churn from python"}});
    ASSERT_TRUE(gated.ok);
    EXPECT_FALSE(gated.value.at("block_deploy").get<bool>());

    auto rejected =
        call("compile_python", {{"project", "py"}, {"source", "x = ("}});
    ASSERT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error.code, "engine_error");
    EXPECT_FALSE(rejected.error.message.empty());
}

// =====================================================================
// add_dataset: a local jsonl file becomes a partitioned table whose
// placement reads back through the dataset views; audited as dataset_added.
// =====================================================================
TEST_F(ActionJourneys, JourneyAddDatasetPlacementReadBack) {
    const fs::path file = fs::path(dir.path()) / "accounts.jsonl";
    {
        std::ofstream out(file);
        for (int i = 0; i < 24; ++i) {
            out << R"({"account":")" << i << R"(","mrr":)" << (100 + i) << "}\n";
        }
    }

    auto added = call("add_dataset", {{"path", file.string()},
                                      {"partition_count", 4}});
    ASSERT_TRUE(added.ok) << added.error.message;
    EXPECT_EQ(added.value.at("rows"), 24);
    EXPECT_EQ(added.value.at("format"), "jsonl");
    EXPECT_EQ(added.value.at("partitions").size(), 4u);
    const std::string table = added.value.at("table");

    auto read_back = call("get_dataset", {{"dataset", table}});
    ASSERT_TRUE(read_back.ok) << read_back.error.message;
    EXPECT_EQ(read_back.value.at("rows"), 24);
    EXPECT_EQ(read_back.value.at("partitions").size(), 4u);

    bool audited = false;
    for (const auto& event : node.audit_events()) {
        if (event->value().action ==
            zetta::governance::AuditAction::DatasetAdded) {
            audited = true;
        }
    }
    EXPECT_TRUE(audited) << "add_dataset must land in the audit trail";

    // Negative: an unsupported extension carries the engine's reason.
    auto rejected = call("add_dataset", {{"path", "/tmp/nope.xlsx"}});
    ASSERT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error.code, "engine_error");
}

// =====================================================================
// testkit_run: kind job — the durable Run id returns immediately while the
// case executes on a worker; status + the case's log land on the run.
// Unknown case / unknown option are rejected with path-qualified reasons.
// =====================================================================
TEST_F(ActionJourneys, JourneyTestkitRunReturnsHandleImmediately) {
    auto submitted = call("testkit_run", {{"case", "compute.cpu.elementwise"}});
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    const std::string run_id = submitted.value.at("run_id");
    ASSERT_FALSE(run_id.empty());
    EXPECT_EQ(submitted.value.at("status"), "running");

    // The handle is live immediately (Pending is never observable — the run
    // is Running before submit returns).
    auto immediate = call("get_run", {{"run_id", run_id}});
    ASSERT_TRUE(immediate.ok) << immediate.error.message;
    EXPECT_EQ(immediate.value.at("project"), "testkit");
    EXPECT_EQ(immediate.value.at("graph"), "compute.cpu.elementwise");

    const nlohmann::json finished = wait_for_terminal_run(run_id);
    ASSERT_EQ(finished.value("status", ""), "succeeded") << finished.dump(2);
    EXPECT_GE(finished.at("logs").size(), 2u)
        << "the case's log + verdict lines land with the terminal status";

    // Negatives: unknown case, unknown option key, mistyped option value.
    auto unknown = call("testkit_run", {{"case", "no.such.case"}});
    ASSERT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.code, "not_found");

    auto bad_key = call("testkit_run", {{"case", "compute.cpu.elementwise"},
                                        {"options", {{"bogus", 1}}}});
    ASSERT_FALSE(bad_key.ok);
    EXPECT_EQ(bad_key.error.code, "invalid_input");
    EXPECT_EQ(bad_key.error.path, "/options/bogus");
}

// =====================================================================
// RULE-3 negative: a key without the orchestration capability is denied and
// the denial is audited under that key.
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

    auto denied =
        call("create_project", {{"name", "sneaky"}}, key->id());
    ASSERT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    EXPECT_TRUE(node.project_names().empty());

    const auto events = node.audit_events();
    ASSERT_FALSE(events.empty());
    bool denial_audited = false;
    for (const auto& event : events) {
        if (event->value().target == "api:create_project" &&
            event->value().actor == key->id() && !event->value().allowed) {
            denial_audited = true;
        }
    }
    EXPECT_TRUE(denial_audited);
}
