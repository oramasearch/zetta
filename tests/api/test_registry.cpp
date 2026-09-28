// The capability registry + main-thread bridge (docs/api/registry-design.md,
// #615). Also home of the first layer-1 MCP journeys
// (docs/testing/mcp-journeys.md): ordered capability calls with read-back
// assertions over a real Node.
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <set>
#include <stdexcept>
#include <chrono>
#include <thread>

#include "index/term_frequency_backend.hpp"
#include "temp_dir.hpp"
#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/compute/compute_job.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/version.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Capability;
using zetta::api::CapabilityKind;
using zetta::api::Registry;

struct RegistryFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    // Spans land in the node's own ring (#640) — the same source the
    // system.traces / system.spans views and the GUI read (RULE-7).
    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }
};

TEST_F(RegistryFixture, RegistersTheCoreCapabilities) {
    EXPECT_EQ(
        registry.size(),
        98u); // +4 action capabilities (#1109), +3 audit-ledger capabilities (#1107), +5 decision-policy capabilities (#1104), +2 featurization reads (#1062), +5 pub/sub (#1033), +approve_plan (#885), +3 workspace_profile
              // (#886), +3 plan (Conflict D), +agent_training_status
              // +set_training_schedule (#941), +score (#948), +get_run_report
              // (#958), +explain (#960 item 5), +list_source_tables (#994),
              // +upload_dataset (#1008), +delete_project (#1009),
              // +list_methodologies (#1061)
    // A remote caller can put data on the node without a file on its disk
    // (#1008).
    EXPECT_TRUE(registry.find("upload_dataset"));
    // A mistaken project can be removed, not only created (#1009).
    EXPECT_TRUE(registry.find("delete_project"));
    // Finding a table must not require ingesting one: browsing a source's
    // catalog used to mean running create_dataset_from_query against
    // information_schema, which materialized a dataset to read a catalog (#994).
    EXPECT_TRUE(registry.find("list_source_tables"));
    EXPECT_TRUE(registry.find("server_info"));
    // A run's report is readable through the control surface, not only from the
    // node that ran it (#958).
    EXPECT_TRUE(registry.find("get_run_report"));
    // "Why is this one at risk" is answerable per account, not only per population.
    EXPECT_TRUE(registry.find("explain"));
    // A node can say how to deploy itself, over MCP (#974).
    EXPECT_TRUE(registry.find("deployment_profile"));
    // The #849 agent CRUD surface (create/list/get/update/delete).
    for (const char* name :
         {"create_agent", "list_agents", "get_agent", "update_agent", "delete_agent"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    // The #825 create path for a full-text index over a loaded table.
    EXPECT_TRUE(registry.find("create_index"));
    // The visual graph workspace's read + save path.
    EXPECT_TRUE(registry.find("get_graph"));
    EXPECT_TRUE(registry.find("save_graph"));
    // The #646 spec-index slice.
    EXPECT_TRUE(registry.find("list_specs"));
    EXPECT_TRUE(registry.find("get_spec"));
    EXPECT_TRUE(registry.find("sql"));
    EXPECT_TRUE(registry.find("submit_feedback"));
    EXPECT_TRUE(registry.find("search"));
    // The in-app test-suite catalog listing (sibling of testsuite_run).
    EXPECT_TRUE(registry.find("testsuite_list"));
    // The #616 read-parity slice: typed detail reads + list_* siblings.
    for (const char* name :
         {"get_project", "get_run", "get_model", "get_job", "get_index",
          "get_dataset", "list_runs", "list_models", "list_jobs",
          "list_indexes", "list_datasets", "analyze_dataset", "plan_features",
          "list_methodologies", "list_ledger", "get_policy"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    const ApiResult methodologies =
        call("list_methodologies", nlohmann::json::object());
    ASSERT_TRUE(methodologies.ok);
    ASSERT_EQ(methodologies.value.at("methodologies").size(), 21u);
    EXPECT_EQ(methodologies.value.at("methodologies").front().at("id"), "M-1");
    EXPECT_TRUE(methodologies.value.at("methodologies").front().at("runnable"));
    // The #617 action-parity slice: the rest of the governed product loop,
    // each requiring orchestration:<name>; run/testsuite_run are kind:job.
    // ...and the governed serving slice (#648): exposure is an
    // orchestration:<name> action like every other mutation.
    for (const char* name :
         {"create_project", "delete_project", "capture_intent", "import_n8n",
          "compile_intent", "compile_n8n", "compile_python", "approve_plan", "gate", "deploy", "run",
          "add_dataset", "testsuite_run", "expose_model", "unexpose_model",
          "expose_index", "unexpose_index", "create_workspace_profile",
          "update_workspace_profile", "create_plan", "update_plan",
          "record_action", "record_outcome", "create_policy", "update_policy", "decide", "approve_decision",
          "publish_report", "enqueue_action_task", "deliver_action_task", "execute_closed_loop"}) {
        const auto entry = registry.find(name);
        ASSERT_TRUE(entry) << name;
        EXPECT_EQ(entry->required_operation.domain,
                  zetta::governance::CapabilityDomain::Orchestration)
            << name;
        EXPECT_EQ(entry->required_operation.action, name);
    }
    EXPECT_EQ(registry.find("run")->kind, CapabilityKind::Job);
    EXPECT_EQ(registry.find("testsuite_run")->kind, CapabilityKind::Job);
    EXPECT_FALSE(registry.find("no_such_tool"));
}

TEST_F(RegistryFixture, RejectsDuplicateAndHandlerlessEntries) {
    EXPECT_FALSE(registry.add(Capability{"server_info", "", "", {}, {},
                                         CapabilityKind::Read,
                                         {}, false, false,
                                         [](const nlohmann::json&, ApiContext&) {
                                             return ApiResult::success({});
                                         }}));
    EXPECT_FALSE(registry.add(Capability{"handlerless", "", "", {}, {},
                                         CapabilityKind::Read,
                                         {}, false, false, nullptr}));
}

TEST_F(RegistryFixture, JourneyReconstructsScoreActionOutcomeChain) {
    const nlohmann::json action_input = {
        {"subject", "account-42"},
        {"score_id", "score-42"},
        {"score", 0.91},
        {"decision", "retain"},
        {"action", "no_contact"},
        {"actor_kind", "human"},
        {"model_version", "churn-v7"},
        {"policy_version", "retention-policy-v3"},
        {"agent_version", "agent-v5"},
        {"cell", "holdout"},
        {"cost", 0.0},
        {"currency", "USD"},
        {"score_at", "2026-09-28T10:00:00.000Z"},
        {"decision_at", "2026-09-28T10:00:01.000Z"},
        {"action_at", "2026-09-28T10:00:02.000Z"}};

    const ApiResult action = call("record_action", action_input);
    ASSERT_TRUE(action.ok) << action.error.message;
    EXPECT_EQ(action.value.at("kind"), "action");
    EXPECT_EQ(action.value.at("actor"), "local");
    const std::string action_id = action.value.at("id").get<std::string>();
    const nlohmann::json immutable_action = action.value;

    const ApiResult outcome = call(
        "record_outcome",
        {{"action_id", action_id},
         {"outcome", "renewed"},
         {"value", 1200.0},
         {"currency", "USD"},
         {"source", "connector"},
         {"occurred_at", "2026-10-28T10:00:00.000Z"}});
    ASSERT_TRUE(outcome.ok) << outcome.error.message;
    EXPECT_EQ(outcome.value.at("action_id"), action_id);

    const ApiResult listed = call("list_ledger", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("entries").size(), 2u);
    bool found_action = false;
    bool found_outcome = false;
    for (const auto& entry : listed.value.at("entries")) {
        if (entry.at("kind") == "action") {
            found_action = entry == immutable_action;
        }
        if (entry.at("kind") == "outcome") {
            found_outcome = entry.at("action_id") == action_id;
        }
    }
    EXPECT_TRUE(found_action);
    EXPECT_TRUE(found_outcome);

    const ApiResult sql = call(
        "sql", {{"statement", "SELECT * FROM system.ledger;"}});
    ASSERT_TRUE(sql.ok) << sql.error.message;
    ASSERT_EQ(sql.value.at("rows").size(), 2u);

    const ApiResult orphan = call(
        "record_outcome",
        {{"action_id", "missing"},
         {"outcome", "renewed"},
         {"value", 1200.0},
         {"currency", "USD"},
         {"source", "manual"},
         {"occurred_at", "2026-10-28T10:00:00.000Z"}});
    EXPECT_FALSE(orphan.ok);
    EXPECT_EQ(orphan.error.code, "not_found");
    EXPECT_EQ(node.ledger_entry_count(), 2u);
}

// #1007: a handler that throws must still produce an answer. The exception used
// to unwind through the owner-loop bridge — over REST the caller got no response
// at all, and on a headless node the process terminated.
TEST_F(RegistryFixture, AThrowingHandlerAnswersWithAnEngineError) {
    ASSERT_TRUE(registry.add(Capability{
        "throws_on_read", "", "", nlohmann::json{{"type", "object"}}, {},
        CapabilityKind::Read,
        zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "query"},
        false, false,
        [](const nlohmann::json&, ApiContext&) -> ApiResult {
            // The shape of the real failure: a typed read of a null column.
            const nlohmann::json row = {{"rank", nullptr}};
            return ApiResult::success({{"rank", row.at("rank").get<int>()}});
        }}));
    const ApiResult result = call("throws_on_read", nlohmann::json::object());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "engine_error");
    EXPECT_NE(result.error.message.find("throws_on_read"), std::string::npos)
        << result.error.message;
    // Still a traced call, like every other failure.
    EXPECT_EQ(result.trace_id.size(), 32u);
}

TEST_F(RegistryFixture, UnknownCapabilityFailsClearly) {
    auto result = call("no_such_tool", nlohmann::json::object());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "unknown_capability");
}

TEST_F(RegistryFixture, SchemaRejectsMalformedInputWithTheReason) {
    auto result = call("sql", nlohmann::json{{"wrong_field", 1}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "invalid_input");
    EXPECT_FALSE(result.error.message.empty());
}

TEST_F(RegistryFixture, UnauthenticatedLocalCallsAreAllowedAndAudited) {
    const auto audit_before = node.audit_event_count();
    auto result = call("server_info", nlohmann::json::object());
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("version"), zetta::kVersion);
    EXPECT_GT(node.audit_event_count(), audit_before);
    const auto events = node.audit_events();
    EXPECT_EQ(events.back()->value().target, "api:server_info");
    EXPECT_EQ(events.back()->value().actor, "local");
}

TEST_F(RegistryFixture, KeyWithoutCapabilityIsDeniedAndAudited) {
    auto principal = node.create_principal(
        "api tester", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    auto key = principal->create_key(zetta::governance::unrestricted_scope());
    ASSERT_TRUE(key);
    ApiContext context{node, nullptr, key->id()};
    auto result = registry.call("server_info", nlohmann::json::object(), context);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "unauthorized");
    const auto events = node.audit_events();
    EXPECT_FALSE(events.back()->value().allowed);
}

TEST_F(RegistryFixture, SqlReadsTheSystemViews) {
    auto result =
        call("sql", nlohmann::json{{"statement", "SELECT * FROM system.datasets;"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.contains("rows"));
}

// Journey: seed a table + Search index, then `search` — the capability is a
// thin caller of THE entry point (Node::search, #641), so the ranked hits come
// back with per-node provenance and the search's own trace id; malformed input
// is rejected at the schema boundary with the path-qualified reason.
TEST_F(RegistryFixture, JourneySearchReturnsRankedHitsWithProvenance) {
    node.index_registry().add(
        std::make_shared<zetta::test::TermFrequencyBackend>());
    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    table->add_row({{"title", "Postgres deep dive"},
                    {"body", "postgres postgres postgres tuning"}});
    table->add_row({{"title", "Cooking pasta"}, {"body", "boil water"}});

    zetta::index::IndexValue value;
    value.table = "articles";
    value.kind = zetta::index::IndexKind::Search;
    value.backend = "tf_reference";
    value.fields = {"title", "body"};
    const auto index = node.create_index(std::move(value));
    ASSERT_TRUE(index);
    const zetta::index::IndexSpec spec = index->spec();
    ASSERT_TRUE(node.index_registry()
                    .resolve(spec)
                    ->build(spec, zetta::index::read_index_partitions(
                                      table->store()))
                    .ok());

    // Discovery first (#642): list_indexes reads the same live registry
    // (Node::indexes()) the system.indexes view computes from, so an agent
    // can find what `search` can target before searching.
    auto listed = call("list_indexes", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("indexes").size(), 1u);
    const nlohmann::json& discovered = listed.value.at("indexes")[0];
    EXPECT_EQ(discovered.at("id"), index->id());
    EXPECT_EQ(discovered.at("table"), "articles");
    EXPECT_EQ(discovered.at("kind"), "search");
    EXPECT_EQ(discovered.at("backend"), "tf_reference");
    EXPECT_EQ(discovered.at("fields"),
              (nlohmann::json::array({"title", "body"})));

    // Schema boundary: a missing required field fails as invalid_input.
    auto missing = call("search", nlohmann::json{{"target", "articles"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "invalid_input");

    // An empty query is rejected at the schema boundary with the
    // path-qualified reason (the /query pointer).
    auto empty_query = call("search", nlohmann::json{{"target", "articles"},
                                                     {"query", ""}});
    EXPECT_FALSE(empty_query.ok);
    EXPECT_EQ(empty_query.error.code, "invalid_input");
    EXPECT_NE(empty_query.error.message.find("/query"), std::string::npos);

    auto result = call("search", nlohmann::json{{"target", "articles"},
                                                {"query", "postgres"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("index_id"), index->id());
    EXPECT_EQ(result.value.at("path"), "consolidated");  // nothing partitioned
    ASSERT_EQ(result.value.at("hits").size(), 1u);
    EXPECT_GT(result.value.at("hits")[0].at("score").get<double>(), 0.0);
    // Per-node provenance: exactly this node answered, marked local.
    ASSERT_EQ(result.value.at("nodes").size(), 1u);
    EXPECT_EQ(result.value.at("nodes")[0].at("node_id"), node.node_id());
    EXPECT_TRUE(result.value.at("nodes")[0].at("local").get<bool>());
    // The call rode the fixture's trace (threaded back on the ApiResult).
    EXPECT_EQ(result.trace_id, trace.trace_id);

    // An unknown target is a domain failure carrying the engine's reason.
    auto unknown = call("search", nlohmann::json{{"target", "nonexistent"},
                                                 {"query", "postgres"}});
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.code, "engine_error");
    EXPECT_NE(unknown.error.message.find("nonexistent"), std::string::npos);
}

// Journey: create_project -> read back through list_projects AND sql. Every
// mutating step gets a read-back (the journey discipline), and the mutation
// fires the activity hook exactly once.
TEST_F(RegistryFixture, JourneyCreateProjectThenReadBack) {
    std::atomic<int> activity_count{0};
    registry.set_activity_hook([&](const zetta::api::ActivityEvent& event) {
        EXPECT_EQ(event.capability, "create_project");
        EXPECT_EQ(event.actor, "local");
        ++activity_count;
    });

    auto created = call("create_project", nlohmann::json{{"name", "churn-q3"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(activity_count.load(), 1);

    auto listed = call("list_projects", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    const auto projects = listed.value.at("projects");
    EXPECT_NE(std::find(projects.begin(), projects.end(), "churn-q3"),
              projects.end());

    // Negative branch of the journey: the duplicate is refused with a reason.
    auto duplicate = call("create_project", nlohmann::json{{"name", "churn-q3"}});
    EXPECT_FALSE(duplicate.ok);
    EXPECT_EQ(duplicate.error.code, "engine_error");
    EXPECT_EQ(activity_count.load(), 1);  // no hook on failure
}

// #1009 journey: delete a project and read the deletion back. Its inputs and
// graphs go with it; its runs and models are kept, stay listed, and the answer
// says how many were left behind.
TEST_F(RegistryFixture, JourneyDeleteProjectThenReadBack) {
    ASSERT_TRUE(call("create_project", nlohmann::json{{"name", "mistake"}}).ok);
    ASSERT_TRUE(call("create_project", nlohmann::json{{"name", "keeper"}}).ok);
    auto captured = call("capture_intent",
                         nlohmann::json{{"project", "mistake"},
                                        {"text", "predict which accounts churn"}});
    ASSERT_TRUE(captured.ok) << captured.error.message;
    auto compiled = call("compile_intent",
                         nlohmann::json{{"project", "mistake"},
                                        {"input_id", captured.value.at("input_id")}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    const std::string project_id = node.find_project("mistake")->id();

    // A finished run and a model labelled with the project.
    auto run = node.create_run(project_id);
    ASSERT_TRUE(run);
    run->set_status(zetta::pipeline::RunStatus::Succeeded);
    zetta::ml::ModelValue model_value;
    model_value.kind = "classifier";
    model_value.name = "mistake-model";
    model_value.project = project_id;
    auto model = node.create_model(std::move(model_value));
    ASSERT_TRUE(model);

    auto deleted = call("delete_project", nlohmann::json{{"name", "mistake"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.code << ": " << deleted.error.message;
    EXPECT_EQ(deleted.value.at("name"), "mistake");
    EXPECT_EQ(deleted.value.at("id"), project_id);
    EXPECT_EQ(deleted.value.at("removed").at("inputs"), 1u);
    EXPECT_EQ(deleted.value.at("removed").at("graphs"), 1u);
    EXPECT_EQ(deleted.value.at("kept").at("runs"), 1u);
    EXPECT_EQ(deleted.value.at("kept").at("models"), 1u);

    // Read back: gone from list_projects, the other project untouched...
    auto listed = call("list_projects", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    const auto projects = listed.value.at("projects");
    EXPECT_EQ(std::find(projects.begin(), projects.end(), "mistake"), projects.end());
    EXPECT_NE(std::find(projects.begin(), projects.end(), "keeper"), projects.end());
    auto get = call("get_project", nlohmann::json{{"name", "mistake"}});
    EXPECT_FALSE(get.ok);
    EXPECT_EQ(get.error.code, "not_found");

    // ...and the run + model still listed, still labelled.
    auto runs = call("get_run", nlohmann::json{{"run_id", run->id()}});
    ASSERT_TRUE(runs.ok) << runs.error.message;
    EXPECT_EQ(runs.value.at("project"), project_id);
    auto models = call("get_model", nlohmann::json{{"model_id", model->id()}});
    ASSERT_TRUE(models.ok) << models.error.message;

    // Deleting it again, or anything missing, is not_found.
    auto again = call("delete_project", nlohmann::json{{"name", "mistake"}});
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.error.code, "not_found");
    auto missing = call("delete_project", nlohmann::json{{"name", "never-was"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");

    // The name is free again.
    EXPECT_TRUE(call("create_project", nlohmann::json{{"name", "mistake"}}).ok);
}

// A project whose run is still in flight is not deleted out from under it.
TEST_F(RegistryFixture, DeleteProjectRefusesWhileARunIsInProgress) {
    ASSERT_TRUE(call("create_project", nlohmann::json{{"name", "busy"}}).ok);
    auto run = node.create_run(node.find_project("busy")->id());
    ASSERT_TRUE(run);
    run->set_status(zetta::pipeline::RunStatus::Running);

    auto refused = call("delete_project", nlohmann::json{{"name", "busy"}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "engine_error");
    EXPECT_NE(refused.error.message.find("running"), std::string::npos)
        << refused.error.message;
    EXPECT_TRUE(node.find_project("busy"));

    run->set_status(zetta::pipeline::RunStatus::Failed);
    EXPECT_TRUE(call("delete_project", nlohmann::json{{"name", "busy"}}).ok);
    EXPECT_FALSE(node.find_project("busy"));
}

// Journey (#616): create a project, capture an intent input and a graph, then
// get_project reads the full detail back — by name AND by uuid (the handle a
// run/job/model carries) — inputs and graphs included; a miss is not_found.
TEST_F(RegistryFixture, JourneyGetProjectReturnsFullDetail) {
    auto created = call("create_project", nlohmann::json{{"name", "churn-q3"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    auto project = node.find_project("churn-q3");
    ASSERT_TRUE(project);
    auto input = project->capture_intent("predict churn for Q3", "tester");
    ASSERT_TRUE(input);
    auto graph = project->create_graph("skeleton", zetta::ir::ExecutionGraph{});
    ASSERT_TRUE(graph);

    auto by_name = call("get_project", nlohmann::json{{"name", "churn-q3"}});
    ASSERT_TRUE(by_name.ok) << by_name.error.message;
    EXPECT_EQ(by_name.value.at("id"), project->id());
    EXPECT_EQ(by_name.value.at("name"), "churn-q3");
    EXPECT_FALSE(by_name.value.at("created").get<std::string>().empty());
    ASSERT_EQ(by_name.value.at("inputs").size(), 1u);
    EXPECT_EQ(by_name.value.at("inputs")[0].at("id"), input->id());
    EXPECT_EQ(by_name.value.at("inputs")[0].at("kind"), "intent");
    ASSERT_EQ(by_name.value.at("graphs").size(), 1u);
    EXPECT_EQ(by_name.value.at("graphs")[0].at("name"), "skeleton");

    // The uuid other entities reference resolves to the same project.
    auto by_id = call("get_project", nlohmann::json{{"name", project->id()}});
    ASSERT_TRUE(by_id.ok) << by_id.error.message;
    EXPECT_EQ(by_id.value.at("id"), project->id());

    auto missing = call("get_project", nlohmann::json{{"name", "nonexistent"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");
}

// Journey (#616): each typed detail read returns the entity's already-validated
// value JSON (exactly metadata()) plus the id, and misses are not_found — so an
// agent reads the same record the CRDT/on-disk projection holds (RULE-7).
TEST_F(RegistryFixture, JourneyTypedDetailReadsReturnTheEntityJson) {
    auto run = node.create_run("proj-uuid", "graph-1");
    ASSERT_TRUE(run);
    run->set_status(zetta::pipeline::RunStatus::Running);
    run->add_log({"2026-07-03T00:00:00Z", "info", "step one"});

    zetta::ml::ModelValue model_value;
    model_value.kind = "classifier";
    model_value.name = "churn-gbdt";
    model_value.metrics = {{"auc", 0.91}};
    auto model = node.create_model(std::move(model_value));
    ASSERT_TRUE(model);

    auto job = node.create_job(zetta::compute::JobKind::Featurize, "proj-uuid");
    ASSERT_TRUE(job);

    zetta::index::IndexValue index_value;
    index_value.table = "events";
    index_value.kind = zetta::index::IndexKind::Search;
    index_value.backend = "tf_reference";
    index_value.fields = {"body"};
    auto index = node.create_index(std::move(index_value));
    ASSERT_TRUE(index);

    struct DetailCase {
        const char* capability;
        const char* id_field;
        std::string id;
        nlohmann::json expected;  // metadata() — the validated value shape
    };
    const DetailCase cases[] = {
        {"get_run", "run_id", run->id(), run->metadata()},
        {"get_model", "model_id", model->id(), model->metadata()},
        {"get_job", "job_id", job->id(), job->metadata()},
        {"get_index", "index_id", index->id(), index->metadata()},
    };
    for (const DetailCase& detail : cases) {
        auto fetched =
            call(detail.capability, nlohmann::json{{detail.id_field, detail.id}});
        ASSERT_TRUE(fetched.ok) << detail.capability << ": "
                                << fetched.error.message;
        nlohmann::json value = fetched.value;
        EXPECT_EQ(value.at("id"), detail.id) << detail.capability;
        value.erase("id");
        EXPECT_EQ(value, detail.expected) << detail.capability;

        auto missing = call(detail.capability,
                            nlohmann::json{{detail.id_field, "no-such-id"}});
        EXPECT_FALSE(missing.ok) << detail.capability;
        EXPECT_EQ(missing.error.code, "not_found") << detail.capability;
    }
    // The run detail carries the mutated state, not just creation defaults.
    auto fetched_run = call("get_run", nlohmann::json{{"run_id", run->id()}});
    ASSERT_TRUE(fetched_run.ok);
    EXPECT_EQ(fetched_run.value.at("status"), "running");
    ASSERT_EQ(fetched_run.value.at("logs").size(), 1u);
    EXPECT_EQ(fetched_run.value.at("logs")[0].at("message"), "step one");
}

// Journey (#616): the list_* siblings return compact structured rows over the
// same live registries the system.* views compute from, with the ids feeding
// the get_* detail reads.
TEST_F(RegistryFixture, JourneyListReadsReturnCompactRows) {
    auto run = node.create_run();
    ASSERT_TRUE(run);
    zetta::ml::ModelValue model_value;
    model_value.kind = "classifier";
    auto model = node.create_model(std::move(model_value));
    ASSERT_TRUE(model);
    auto job = node.create_job(zetta::compute::JobKind::Train);
    ASSERT_TRUE(job);
    zetta::index::IndexValue index_value;
    index_value.table = "events";
    index_value.backend = "tf_reference";
    index_value.fields = {"body"};
    auto index = node.create_index(std::move(index_value));
    ASSERT_TRUE(index);

    auto runs = call("list_runs", nlohmann::json::object());
    ASSERT_TRUE(runs.ok) << runs.error.message;
    ASSERT_EQ(runs.value.at("runs").size(), 1u);
    EXPECT_EQ(runs.value.at("runs")[0].at("id"), run->id());
    EXPECT_EQ(runs.value.at("runs")[0].at("status"), "pending");

    // testsuite_list serves the whole in-app test-suite catalog (independent of
    // node state): every case with its id / category / name + tunable options,
    // the same source system.test_cases and the Test suite panel read.
    auto tests = call("testsuite_list", nlohmann::json::object());
    ASSERT_TRUE(tests.ok) << tests.error.message;
    ASSERT_FALSE(tests.value.at("tests").empty());
    bool found_elementwise = false;
    for (const auto& entry : tests.value.at("tests")) {
        EXPECT_FALSE(entry.at("id").get<std::string>().empty());
        EXPECT_FALSE(entry.at("category").get<std::string>().empty());
        EXPECT_TRUE(entry.contains("options"));
        if (entry.at("id") == "compute.cpu.elementwise") {
            found_elementwise = true;
            EXPECT_EQ(entry.at("category"), "compute");
        }
    }
    EXPECT_TRUE(found_elementwise);

    auto models = call("list_models", nlohmann::json::object());
    ASSERT_TRUE(models.ok) << models.error.message;
    ASSERT_EQ(models.value.at("models").size(), 1u);
    EXPECT_EQ(models.value.at("models")[0].at("id"), model->id());
    EXPECT_FALSE(models.value.at("models")[0].at("has_weights").get<bool>());

    auto jobs = call("list_jobs", nlohmann::json::object());
    ASSERT_TRUE(jobs.ok) << jobs.error.message;
    ASSERT_EQ(jobs.value.at("jobs").size(), 1u);
    EXPECT_EQ(jobs.value.at("jobs")[0].at("id"), job->id());
    EXPECT_EQ(jobs.value.at("jobs")[0].at("kind"), "train");

    auto indexes = call("list_indexes", nlohmann::json::object());
    ASSERT_TRUE(indexes.ok) << indexes.error.message;
    ASSERT_EQ(indexes.value.at("indexes").size(), 1u);
    EXPECT_EQ(indexes.value.at("indexes")[0].at("id"), index->id());
    EXPECT_EQ(indexes.value.at("indexes")[0].at("fields"),
              nlohmann::json::array({"body"}));
}

// Journey (#616): seed a table with rows, then get_dataset / list_datasets read
// the computed summary — the same live source as system.datasets (RULE-7) but
// with the schema and placement kept structured; the same name in two databases
// is refused until "database" disambiguates.
TEST_F(RegistryFixture, JourneyDatasetReadsReturnComputedSummaries) {
    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("events");
    ASSERT_TRUE(table);
    table->add_row({{"user", "u1"}, {"action", "login"}});
    table->add_row({{"user", "u2"}, {"action", "churn"}});

    auto fetched = call("get_dataset", nlohmann::json{{"dataset", "events"}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    EXPECT_EQ(fetched.value.at("name"), "events");
    EXPECT_EQ(fetched.value.at("database"), "data");
    EXPECT_EQ(fetched.value.at("rows"), 2u);
    EXPECT_EQ(fetched.value.at("columns"),
              nlohmann::json::array({"action", "user"}));
    EXPECT_TRUE(fetched.value.at("partitions").empty());

    auto listed = call("list_datasets", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("datasets").size(), 1u);
    EXPECT_EQ(listed.value.at("datasets")[0].at("name"), "events");

    auto missing = call("get_dataset", nlohmann::json{{"dataset", "nonexistent"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");

    // The same dataset name in a second database: ambiguous until qualified.
    auto other = node.create_database("staging");
    ASSERT_TRUE(other);
    ASSERT_TRUE(other->create_table("events"));
    auto ambiguous = call("get_dataset", nlohmann::json{{"dataset", "events"}});
    EXPECT_FALSE(ambiguous.ok);
    EXPECT_NE(ambiguous.error.message.find("staging"), std::string::npos);
    auto qualified = call("get_dataset", nlohmann::json{{"dataset", "events"},
                                                        {"database", "data"}});
    ASSERT_TRUE(qualified.ok) << qualified.error.message;
    EXPECT_EQ(qualified.value.at("rows"), 2u);
}

TEST_F(RegistryFixture, AnalyzeAndPlanFeatureReadsUseLandedDatasetRows) {
    std::vector<nlohmann::json> rows;
    for (int index = 0; index < 80; ++index) {
        rows.push_back({{"customer_id", index + 1},
                        {"plan", index % 3 == 0 ? "pro" : "basic"},
                        {"spend", index % 20},
                        {"signup_at", "2025-01-01"},
                        {"churned", index % 4 == 0 ? 1 : 0}});
    }
    zetta::data::AddDatasetOptions options;
    options.database = "features";
    options.table_name = "accounts";
    options.partitioned = false;
    const auto added = zetta::data::add_dataset_rows(node, std::move(rows), options);
    ASSERT_TRUE(added.ok()) << added.error;

    const auto analyzed = call("analyze_dataset", {{"dataset", "features.accounts"},
                                                    {"label", "churned"},
                                                    {"as_of", "2025-12-31"}});
    ASSERT_TRUE(analyzed.ok) << analyzed.error.message;
    EXPECT_EQ(analyzed.value.at("grain"), "entity");
    EXPECT_EQ(analyzed.value.at("columns").size(), 5u);

    const auto planned = call("plan_features", {{"dataset", "features.accounts"},
                                                {"methodology", "M-1"},
                                                {"label", "churned"},
                                                {"as_of", "2025-12-31"}});
    ASSERT_TRUE(planned.ok) << planned.error.message;
    EXPECT_EQ(planned.value.at("status"), "ready");
    EXPECT_FALSE(planned.value.at("entries").empty());
    EXPECT_TRUE(planned.value.at("transform_required").is_null());
}

// #1008: upload_dataset lands a file's TEXT from the request through the same
// decode + table creation + placement as add_dataset, and the table then reads
// back through list_datasets — the path a remote caller (lake's Train -> Data
// page) has, since it cannot write the node's filesystem.
TEST_F(RegistryFixture, UploadDatasetLandsCsvAndJsonlAndListsThem) {
    auto csv = call("upload_dataset",
                    nlohmann::json{{"table_name", "accounts"},
                                   {"format", "csv"},
                                   {"content", "account,plan,mrr\n1,pro,500\n2,free,0\n"
                                               "3,\"pro, annual\",1200\n"}});
    ASSERT_TRUE(csv.ok) << csv.error.code << ": " << csv.error.message;
    EXPECT_EQ(csv.value.at("database"), "datasets");
    EXPECT_EQ(csv.value.at("table"), "accounts");
    EXPECT_EQ(csv.value.at("format"), "csv");
    EXPECT_EQ(csv.value.at("rows"), 3u);
    EXPECT_FALSE(csv.value.at("partitions").empty());

    auto jsonl = call("upload_dataset",
                      nlohmann::json{{"table_name", "events"},
                                     {"format", "jsonl"},
                                     {"database", "uploads"},
                                     {"replication_factor", 1},
                                     {"content", "{\"account\":1,\"event\":\"login\"}\n"
                                                 "\n"
                                                 "{\"account\":2,\"event\":\"churn\"}\n"}});
    ASSERT_TRUE(jsonl.ok) << jsonl.error.code << ": " << jsonl.error.message;
    EXPECT_EQ(jsonl.value.at("database"), "uploads");
    EXPECT_EQ(jsonl.value.at("format"), "jsonl");
    EXPECT_EQ(jsonl.value.at("rows"), 2u);

    // The cells were type-inferred exactly as add_dataset infers a csv file's,
    // and the quoted comma stayed inside its cell.
    auto accounts = node.find_database("datasets")->find_table("accounts");
    ASSERT_TRUE(accounts);
    bool quoted_cell_kept = false;
    for (std::size_t index = 0; index < accounts->store().size(); ++index) {
        const auto row = accounts->store().at(index);
        ASSERT_TRUE(row);
        EXPECT_TRUE(row->at("mrr").is_number()) << row->dump();
        if (row->at("plan") == "pro, annual") quoted_cell_kept = true;
    }
    EXPECT_TRUE(quoted_cell_kept);

    // Both read back through the dataset reads.
    auto listed = call("list_datasets", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    std::set<std::string> names;
    for (const auto& dataset : listed.value.at("datasets")) {
        names.insert(dataset.at("database").get<std::string>() + "." +
                     dataset.at("name").get<std::string>());
    }
    EXPECT_TRUE(names.contains("datasets.accounts")) << listed.value.dump();
    EXPECT_TRUE(names.contains("uploads.events")) << listed.value.dump();

    // Audited like add_dataset.
    std::size_t audited = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == zetta::governance::AuditAction::DatasetAdded) ++audited;
    }
    EXPECT_EQ(audited, 2u);

    // A second upload under a taken name gets a suffix, and says so.
    auto again = call("upload_dataset",
                      nlohmann::json{{"table_name", "accounts"},
                                     {"format", "csv"},
                                     {"content", "account\n9\n"}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value.at("table"), "accounts_2");
}

TEST_F(RegistryFixture, UploadDatasetRefusesOversizeBadFormatAndMalformedContent) {
    // Over the 16 MiB cap: a clear invalid_input naming the cap, and nothing made.
    const std::string oversize(zetta::data::kMaxDatasetContentBytes + 1, 'x');
    auto too_big = call("upload_dataset",
                        nlohmann::json{{"table_name", "huge"},
                                       {"format", "csv"},
                                       {"content", oversize}});
    EXPECT_FALSE(too_big.ok);
    EXPECT_EQ(too_big.error.code, "invalid_input");
    EXPECT_NE(too_big.error.message.find("16 MiB"), std::string::npos)
        << too_big.error.message;

    // A format outside csv|jsonl is refused by the input schema.
    auto parquet = call("upload_dataset",
                        nlohmann::json{{"table_name", "binary"},
                                       {"format", "parquet"},
                                       {"content", "PAR1"}});
    EXPECT_FALSE(parquet.ok);
    EXPECT_EQ(parquet.error.code, "invalid_input");
    EXPECT_NE(parquet.error.message.find("format"), std::string::npos)
        << parquet.error.message;

    // Missing required fields are refused by the schema too.
    auto no_table = call("upload_dataset",
                         nlohmann::json{{"format", "csv"}, {"content", "a\n1\n"}});
    EXPECT_FALSE(no_table.ok);
    EXPECT_EQ(no_table.error.code, "invalid_input");

    // Malformed jsonl names the line, like a malformed file does.
    auto malformed = call("upload_dataset",
                          nlohmann::json{{"table_name", "broken"},
                                         {"format", "jsonl"},
                                         {"content", "{\"a\":1}\n{not json}\n"}});
    EXPECT_FALSE(malformed.ok);
    EXPECT_EQ(malformed.error.code, "engine_error");
    EXPECT_NE(malformed.error.message.find("line 2"), std::string::npos)
        << malformed.error.message;

    // A csv with a header and no rows has nothing to land.
    auto header_only = call("upload_dataset",
                            nlohmann::json{{"table_name", "empty"},
                                           {"format", "csv"},
                                           {"content", "a,b\n"}});
    EXPECT_FALSE(header_only.ok);

    // None of the refusals created a table.
    EXPECT_FALSE(node.find_database("datasets"));
}

// #833 section 9: promote_dataset raises a cached result set's replication
// factor through the registry — the surface that makes "promote this cache to
// permanent" real, resolving the dataset the way get_dataset does. The fixture
// is a single node, so the interesting assertion here is the honest clamp: it
// reports the factor it could actually apply rather than claiming three copies
// on a cluster of one. Growing the owner set is covered by
// tests/data/test_promote_dataset.cpp.
TEST_F(RegistryFixture, PromoteDatasetReportsTheFactorItCouldApply) {
    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 16; ++i) rows.push_back(nlohmann::json{{"n", i}});
    zetta::data::AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.replication_factor = 1;  // a cache, as the connector path lands it
    const zetta::data::AddDatasetResult added =
        zetta::data::add_dataset_rows(node, rows, options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_FALSE(added.partitions.empty());

    auto promoted = call("promote_dataset",
                         nlohmann::json{{"dataset", "cached"},
                                        {"database", "datasets"},
                                        {"replication_factor", 3}});
    ASSERT_TRUE(promoted.ok) << promoted.error.message;
    EXPECT_EQ(promoted.value.at("table"), "cached");
    EXPECT_EQ(promoted.value.at("partitions"), added.partitions.size());
    EXPECT_EQ(promoted.value.at("replication_factor"), 1u)
        << "a one-node cluster cannot hold three copies, and must not say it does";
    EXPECT_EQ(promoted.value.at("promoted"), 0u);

    auto missing = call("promote_dataset", nlohmann::json{{"dataset", "nope"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");
}

// Every call is a span with intent + result shape, and the result carries the
// trace ids — the observability contract (docs/architecture/observability.md).
TEST_F(RegistryFixture, CallsAreSpansAndTracesReadBack) {
    auto created = call("create_project",
                        nlohmann::json{{"name", "traced"},
                                       {"intent", "verify observability"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.trace_id, trace.trace_id);
    EXPECT_EQ(created.span_id.size(), 16u);
    EXPECT_GE(created.duration_ms, 0.0);

    auto fetched = call("get_trace", nlohmann::json{{"trace_id", trace.trace_id}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    const auto& spans = fetched.value.at("spans");
    ASSERT_GE(spans.size(), 1u);
    bool found_intent = false;
    for (const auto& span : spans) {
        if (span.at("name") == "api:create_project") {
            EXPECT_EQ(span.at("attributes").at("intent"), "verify observability");
            EXPECT_TRUE(span.at("ok").get<bool>());
            found_intent = true;
        }
    }
    EXPECT_TRUE(found_intent);
}

// A failed call closes its span with the error, and the reserved intent field
// never reaches schema validation.
TEST_F(RegistryFixture, FailedCallsRecordErrorSpans) {
    auto denied = call("sql", nlohmann::json{{"intent", "probe"},
                                             {"statement", 42}});
    EXPECT_FALSE(denied.ok);
    auto fetched = call("get_trace", nlohmann::json{{"trace_id", trace.trace_id}});
    ASSERT_TRUE(fetched.ok);
    bool found_error_span = false;
    for (const auto& span : fetched.value.at("spans")) {
        if (span.at("name") == "api:sql" && !span.at("ok").get<bool>()) {
            EXPECT_NE(span.at("error").get<std::string>().find("invalid_input"),
                      std::string::npos);
            found_error_span = true;
        }
    }
    EXPECT_TRUE(found_error_span);
}

// Coroutine-first bridge: a runtime coroutine awaits run_on_owner and resumes
// after the owning loop drains — no caller-facing blocking primitives.
// (A free coroutine, not a lambda coroutine: parameters are copied into the
// frame, while a temporary lambda's captures dangle — the runtime.hpp caveat.)
namespace {
zetta::runtime::task<> await_bridge_work(zetta::api::MainThreadBridge& bridge,
                                         std::atomic<bool>& work_ran,
                                         std::atomic<bool>& coroutine_resumed) {
    co_await bridge.run_on_owner([&work_ran] { work_ran = true; });
    coroutine_resumed = true;
}
}  // namespace

TEST(MainThreadBridge, AwaitingCoroutineResumesAfterDrain) {
    zetta::api::MainThreadBridge bridge;
    std::atomic<bool> work_ran{false};
    std::atomic<bool> coroutine_resumed{false};

    zetta::runtime::spawn(await_bridge_work(bridge, work_ran, coroutine_resumed),
                          [] {});

    // The coroutine suspends on the event until the owning loop drains.
    for (int attempt = 0; attempt < 100 && bridge.pending() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(bridge.pending(), 1u);
    EXPECT_FALSE(work_ran.load());

    bridge.drain();
    for (int attempt = 0; attempt < 1000 && !coroutine_resumed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(work_ran.load());
    EXPECT_TRUE(coroutine_resumed.load());
    EXPECT_EQ(bridge.pending(), 0u);
}

zetta::runtime::task<> await_throwing_bridge_work(zetta::api::MainThreadBridge& bridge,
                                                  std::atomic<bool>& coroutine_resumed) {
    co_await bridge.run_on_owner([] { throw std::runtime_error("work threw"); });
    coroutine_resumed = true;
}

// #1007: work that throws on the owning loop must not park its awaiter forever
// (a REST request that never gets a response), nor take the rest of the batch —
// or the owner loop itself — down with it.
TEST(MainThreadBridge, ThrowingWorkStillResumesItsAwaiterAndTheRestOfTheBatch) {
    zetta::api::MainThreadBridge bridge;
    std::atomic<bool> thrower_resumed{false};
    std::atomic<bool> later_work_ran{false};
    std::atomic<bool> later_resumed{false};

    zetta::runtime::spawn(await_throwing_bridge_work(bridge, thrower_resumed), [] {});
    for (int attempt = 0; attempt < 100 && bridge.pending() < 1; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    zetta::runtime::spawn(await_bridge_work(bridge, later_work_ran, later_resumed),
                          [] {});
    for (int attempt = 0; attempt < 100 && bridge.pending() < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(bridge.pending(), 2u);

    EXPECT_NO_THROW(bridge.drain());
    for (int attempt = 0;
         attempt < 1000 && !(thrower_resumed && later_resumed); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(thrower_resumed.load());
    EXPECT_TRUE(later_work_ran.load());
    EXPECT_TRUE(later_resumed.load());
    EXPECT_EQ(bridge.pending(), 0u);
}

}  // namespace

// Journey (#640 acceptance): an agent runs a tool, then reads the trace back
// through BOTH surfaces — get_trace and the system.traces/system.spans SQL
// views — and sees intent, plan-level naming, duration, and result shape.
TEST_F(RegistryFixture, JourneyRunToolThenReadTraceContent) {
    auto created = call("create_project",
                        nlohmann::json{{"name", "trace-journey"},
                                       {"intent", "verify the trace loop"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    ASSERT_FALSE(created.trace_id.empty());

    // Read-back 1: the capability surface.
    auto fetched = call("get_trace", nlohmann::json{{"trace_id", created.trace_id}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    bool saw_call_span = false;
    for (const auto& span : fetched.value.at("spans")) {
        if (span.at("name") != "api:create_project") continue;
        saw_call_span = true;
        EXPECT_EQ(span.at("attributes").at("intent"), "verify the trace loop");
        EXPECT_GE(span.at("duration_ms").get<double>(), 0.0);
        EXPECT_TRUE(span.at("attributes").contains("result_shape"));
    }
    EXPECT_TRUE(saw_call_span);

    // Read-back 2: the SQL views over the SAME ring (RULE-7 — one source).
    auto traces = call(
        "sql", nlohmann::json{{"statement", "SELECT * FROM system.traces;"}});
    ASSERT_TRUE(traces.ok) << traces.error.message;
    bool saw_trace_row = false;
    for (const auto& row : traces.value.at("rows")) {
        if (row.at("trace_id") != created.trace_id) continue;
        saw_trace_row = true;
        EXPECT_EQ(row.at("intent"), "verify the trace loop");
        EXPECT_TRUE(row.at("ok").get<bool>());
        EXPECT_GE(row.at("span_count").get<int>(), 1);
    }
    EXPECT_TRUE(saw_trace_row);

    auto spans = call(
        "sql",
        nlohmann::json{{"statement", "SELECT name, intent, duration_ms FROM "
                                     "system.spans;"}});
    ASSERT_TRUE(spans.ok) << spans.error.message;
    EXPECT_FALSE(spans.value.at("rows").empty());
}

// submit_feedback (#640): the bundle snapshots description + traces + recent
// errors + versions as a large object in the 'feedback' database; the export
// is explicit (a caller-chosen path) — nothing leaves on its own.
TEST_F(RegistryFixture, SubmitFeedbackSnapshotsABundle) {
    // Leave observable state behind: one good call, one failed call.
    auto created = call("create_project",
                        nlohmann::json{{"name", "feedback-subject"},
                                       {"intent", "reproduce the bug"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    auto failed = call("sql", nlohmann::json{{"statement", "SELCT nope;"}});
    EXPECT_FALSE(failed.ok);

    const std::string export_path =
        (dir.path() / "bundle-export.json").string();
    auto submitted = call(
        "submit_feedback",
        nlohmann::json{{"description", "the run did something odd"},
                       {"trace_ids", {created.trace_id}},
                       {"export_path", export_path}});
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    EXPECT_FALSE(submitted.value.at("feedback_id").get<std::string>().empty());
    EXPECT_EQ(submitted.value.at("database"), "feedback");
    EXPECT_EQ(submitted.value.at("traces_attached").get<int>(), 1);
    EXPECT_GE(submitted.value.at("errors_attached").get<int>(), 1);
    EXPECT_EQ(submitted.value.at("exported_to"), export_path);

    // The exported file IS the bundle: valid against its schema, carrying the
    // description verbatim, the attached trace, and the recent error span.
    std::ifstream exported(export_path);
    ASSERT_TRUE(exported.good());
    const nlohmann::json bundle =
        nlohmann::json::parse(exported, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(bundle.is_discarded());
    EXPECT_EQ(bundle.at("description"), "the run did something odd");
    ASSERT_EQ(bundle.at("traces").size(), 1u);
    EXPECT_EQ(bundle.at("traces")[0].at("trace_id"), created.trace_id);
    EXPECT_GE(bundle.at("recent_errors").size(), 1u);
    EXPECT_EQ(bundle.at("version"), zetta::kVersion);

    // Malformed input is refused at the boundary.
    auto rejected = call("submit_feedback", nlohmann::json{{"description", ""}});
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error.code, "invalid_input");
}


// #983 acceptance criteria 3 and 4: the Monitor and Improve verbs must be REACHABLE by the agent.
//
// A capability that ships and is invisible to chat may as well not exist — `score` shipped,
// worked, and left "which accounts are most at risk" unanswerable until it was allowlisted. The
// agent could previously only DESCRIBE monitoring, which is #976 at the methodology level.
TEST_F(RegistryFixture, TheMonitorAndImproveVerbsAreReachableReads) {
    for (const char* name : {"model_health", "propose_improvement"}) {
        const std::shared_ptr<const Capability> capability = registry.find(name);
        ASSERT_NE(capability, nullptr) << name << " is not registered";
        EXPECT_EQ(capability->kind, CapabilityKind::Read)
            << name << " must be a read — reads are GET, actions are POST (#954)";
        EXPECT_FALSE(capability->mutates) << name << " reports; it does not act";
    }
}
