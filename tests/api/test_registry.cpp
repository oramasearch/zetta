// The capability registry + main-thread bridge (docs/api/registry-design.md,
// #615). Also home of the first layer-1 MCP journeys
// (docs/testing/mcp-journeys.md): ordered capability calls with read-back
// assertions over a real Node.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <vector>

#include "index/term_frequency_backend.hpp"
#include "temp_dir.hpp"
#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
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
#include "zetta/ml/study.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/export_path.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_controller.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/observability/feedback.hpp"
#include "zetta/solutions/roi_readout.hpp"
#include "zetta/version.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/postgres_runner.hpp"

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

// The input check never throws past Registry::call (#1231). A schema with
// contentEncoding made the validator throw "content checker was not set" out of
// the call; base64 is now checked, and a schema the validator cannot compile is
// an invalid_input refusal.
TEST_F(RegistryFixture, InputSchemaProblemsAreInvalidInputNeverExceptions) {
    const auto echo = [](const nlohmann::json& input, zetta::api::ApiContext&) {
        return ApiResult::success(input);
    };
    Capability encoded{"echo_encoded", "Echo", "Echo base64. No side effects.",
                       {{"type", "object"},
                        {"properties",
                         {{"bytes", {{"type", "string"}, {"contentEncoding", "base64"}}}}}},
                       {{"type", "object"}}, CapabilityKind::Read,
                       zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "query"},
                       false, false, echo};
    ASSERT_TRUE(registry.add(std::move(encoded)));
    Capability broken{"echo_broken", "Echo", "Echo with a bad pattern. No side effects.",
                      {{"type", "object"},
                       {"properties", {{"text", {{"type", "string"}, {"pattern", "(unclosed"}}}}}},
                      {{"type", "object"}}, CapabilityKind::Read,
                      zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "query"},
                      false, false, echo};
    ASSERT_TRUE(registry.add(std::move(broken)));

    ApiResult result;
    ASSERT_NO_THROW(result = call("echo_encoded", {{"bytes", "aGVsbG8="}}));
    EXPECT_TRUE(result.ok) << result.error.message;
    ASSERT_NO_THROW(result = call("echo_encoded", {{"bytes", "not base64!"}}));
    EXPECT_EQ(result.error.code, "invalid_input");
    EXPECT_EQ(result.error.path, "/bytes");
    ASSERT_NO_THROW(result = call("echo_broken", {{"text", "x"}}));
    EXPECT_EQ(result.error.code, "invalid_input");
    EXPECT_NE(result.error.message.find("could not be checked"), std::string::npos)
        << result.error.message;
}

TEST_F(RegistryFixture, RegistersTheCoreCapabilities) {
    EXPECT_EQ(
        registry.size(),
        179u); // +set_safe_mode +get_safe_mode (#1373), +4 published pipelines (#1289), +5 run proposals (#969: propose_run, approve_run_proposal, reject_run_proposal, get_run_proposal, list_run_proposals), +load_dataset_object (#1302), +2 app-credential capabilities (#1280), +2 network-record capabilities (#1281), +append_methodology_artifact, +read_model_weights (lake#690), +export_model (lake#681), +5 workspace methodology capabilities (zetta#1261), +storage_usage (lake#658), +4 standalone models (lake#654), +4 deletes (lake#645), +8 object capability controls (#1005), +8 record store and time-series capabilities (#1231), +validate_study (#1207), +delete_study (#1158), +purge_audit (#1169), +node_memory (#1163), +8 study controls (#1138), +5 run-metrics controls (#1137), +5 topic/message CRUD (#1136), +4 solution registry capabilities (#1102), +snapshot (#1103), +5 decision-policy capabilities (#1104), +3 experiment controls (#1105), +get_monitor (#1106), +3 audit-ledger capabilities (#1107), +get_roi_readout (#1108), +4 action capabilities (#1109), +2 featurization reads (#1062), +5 pub/sub (#1033), +approve_plan (#885), +3 workspace_profile
              // (#886), +3 plan (Conflict D), +agent_training_status
              // +set_training_schedule (#941), +score (#948), +get_run_report
              // (#958), +explain (#960 item 5), +list_source_tables (#994),
              // +upload_dataset (#1008), +delete_project (#1009),
              // +list_methodologies (#1061)
    for (const char* name :
         {"create_collection", "put_record", "get_record", "delete_record", "list_records",
          "create_metric", "record_metric", "query_metric"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    for (const char* name : {"propose_run", "approve_run_proposal", "reject_run_proposal",
                             "get_run_proposal", "list_run_proposals"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    EXPECT_TRUE(registry.find("write_network_record"));
    EXPECT_TRUE(registry.find("list_network_records"));
    EXPECT_TRUE(registry.find("issue_app_credential"));
    EXPECT_TRUE(registry.find("revoke_app_credential"));
    for (const char* name :
         {"create_bucket", "list_objects", "object_usage", "object_barrier", "delete_object",
          "acquire_object_lease", "renew_object_lease", "release_object_lease"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    for (const char* name :
         {"get_topic", "update_topic", "browse_topic", "update_event", "delete_events"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    // A remote caller can put data on the node without a file on its disk
    // (#1008).
    EXPECT_TRUE(registry.find("upload_dataset"));
    // ...and a file past its 16 MiB cap, staged in a bucket first (#1302).
    EXPECT_TRUE(registry.find("load_dataset_object"));
    // A mistaken project can be removed, not only created (#1009).
    EXPECT_TRUE(registry.find("delete_project"));
    // Finding a table must not require ingesting one: browsing a source's
    // catalog used to mean running create_dataset_from_query against
    // information_schema, which materialized a dataset to read a catalog (#994).
    EXPECT_TRUE(registry.find("list_source_tables"));
    EXPECT_TRUE(registry.find("server_info"));
    EXPECT_TRUE(registry.find("node_memory"));
    EXPECT_TRUE(registry.find("purge_audit"));
    EXPECT_TRUE(registry.find("create_experiment"));
    EXPECT_TRUE(registry.find("assign"));
    EXPECT_TRUE(registry.find("get_experiment"));
    for (const char* name : {"log_run_metrics", "log_run_params",
                             "log_run_artifact", "get_run_metrics",
                             "compare_runs"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    for (const char* name : {"validate_study", "create_study", "get_study",
                             "list_studies",
                             "list_trials", "stop_study", "stop_trial",
                             "promote_trial", "delete_study",
                             "get_study_analysis"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
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
         {"get_project", "get_run", "get_model", "get_monitor", "get_roi_readout", "get_job", "get_index",
          "get_dataset", "list_runs", "list_models", "list_jobs",
          "list_indexes", "list_datasets", "analyze_dataset", "plan_features",
          "list_methodologies", "list_solutions", "get_solution", "list_ledger",
          "get_policy"}) {
        EXPECT_TRUE(registry.find(name)) << name;
    }
    const ApiResult methodologies =
        call("list_methodologies", nlohmann::json::object());
    ASSERT_TRUE(methodologies.ok);
    ASSERT_EQ(methodologies.value.at("methodologies").size(), 21u);
    EXPECT_EQ(methodologies.value.at("methodologies").front().at("id"), "M-1");
    EXPECT_TRUE(methodologies.value.at("methodologies").front().at("runnable"));
    EXPECT_TRUE(methodologies.value.at("methodologies").front().at("studies"));
    EXPECT_FALSE(methodologies.value.at("methodologies").front()
                     .at("study_search_space")
                     .is_null());
    EXPECT_FALSE(methodologies.value.at("methodologies").at(1).at("studies"));
    EXPECT_FALSE(methodologies.value.at("methodologies").at(1)
                     .at("study_reason")
                     .get<std::string>()
                     .empty());
    const ApiResult solutions = call("list_solutions", nlohmann::json::object());
    ASSERT_TRUE(solutions.ok);
    ASSERT_EQ(solutions.value.at("solutions").size(), 20u);
    EXPECT_EQ(solutions.value.at("solutions").front().at("id"), "S-1");
    EXPECT_TRUE(solutions.value.at("solutions").front().at("runnable"));
    EXPECT_FALSE(solutions.value.at("solutions").front().contains(
        "capability_levels"));
    ASSERT_EQ(solutions.value.at("solutions").front().at("levels").size(),
              4u);
    EXPECT_EQ(solutions.value.at("solutions").front().at("levels").at(0).at(
                  "level"),
              1);
    EXPECT_TRUE(solutions.value.at("solutions").front().at("missing_components").empty());
    const auto b4 = std::find_if(
        solutions.value.at("solutions").begin(),
        solutions.value.at("solutions").end(),
        [](const auto& solution) { return solution.at("id") == "B-4"; });
    ASSERT_NE(b4, solutions.value.at("solutions").end());
    EXPECT_EQ(b4->at("status"), "catalogue");
    EXPECT_EQ(b4->at("runnable"), true);
    EXPECT_EQ(b4->at("minimal_template"), false);
    EXPECT_TRUE(b4->at("missing_components").empty());
    EXPECT_FALSE(b4->at("promotion_condition").get<std::string>().empty());
    EXPECT_EQ(solutions.value.at("solutions").back().at("id"), "B-8");
    EXPECT_EQ(solutions.value.at("solutions").back().at("status"),
              "catalogue");
    EXPECT_TRUE(solutions.value.at("solutions").back().at("runnable"));
    EXPECT_FALSE(solutions.value.at("solutions")
                     .back()
                     .at("promotion_condition")
                     .get<std::string>()
                     .empty());
    const ApiResult healthcare =
        call("get_solution", {{"solution", "B-6"}});
    ASSERT_TRUE(healthcare.ok);
    EXPECT_TRUE(healthcare.value.at("solution").at("runnable"));
    EXPECT_EQ(healthcare.value.at("solution").at("status"), "catalogue");
    EXPECT_FALSE(healthcare.value.at("solution")
                     .at("promotion_condition")
                     .get<std::string>()
                     .empty());
    // The #617 action-parity slice: the rest of the governed product loop,
    // each requiring orchestration:<name>; run/testsuite_run are kind:job.
    // ...and the governed serving slice (#648): exposure is an
    // orchestration:<name> action like every other mutation.
    for (const char* name :
         {"create_project", "delete_project", "capture_intent", "import_n8n",
          "compile_intent", "compile_n8n", "compile_python", "approve_plan", "gate", "deploy", "run",
          "add_dataset", "snapshot", "testsuite_run", "expose_model", "unexpose_model",
          "expose_index", "unexpose_index", "expose_pipeline", "unexpose_pipeline",
          "create_workspace_profile",
          "update_workspace_profile", "create_plan", "update_plan",
          "record_action", "record_outcome", "create_policy", "update_policy",
          "decide", "approve_decision", "create_experiment", "assign",
          "log_run_metrics", "log_run_params", "log_run_artifact",
          "create_study", "stop_study", "stop_trial", "promote_trial",
          "delete_study", "delete_dataset", "delete_model", "delete_run", "delete_index",
          "train_model", "retrain_model", "register_model", "move_model_to_project",
          "create_solution", "run_solution",
          "publish_report", "enqueue_action_task", "deliver_action_task", "execute_closed_loop"}) {
        const auto entry = registry.find(name);
        ASSERT_TRUE(entry) << name;
        EXPECT_EQ(entry->required_operation.domain,
                  zetta::governance::CapabilityDomain::Orchestration)
            << name;
        EXPECT_EQ(entry->required_operation.action, name);
    }
    EXPECT_EQ(registry.find("run")->kind, CapabilityKind::Job);
    EXPECT_EQ(registry.find("run_solution")->kind, CapabilityKind::Job);
    EXPECT_EQ(registry.find("train_model")->kind, CapabilityKind::Job);
    EXPECT_EQ(registry.find("retrain_model")->kind, CapabilityKind::Job);
    EXPECT_EQ(registry.find("testsuite_run")->kind, CapabilityKind::Job);
    EXPECT_FALSE(registry.find("no_such_tool"));
}

TEST(RoiReadoutCapability, JourneyReturnsUnclaimableWithoutHoldoutOutcomes) {
    zetta::test::TempDir directory;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    const auto project = node.create_project("renewal-project");
    ASSERT_TRUE(project);
    zetta::ir::ExecutionGraph graph;
    ASSERT_TRUE(graph.add_node(zetta::ir::StepKind::Connect, "source"));
    ASSERT_TRUE(project->create_graph("production", std::move(graph)));

    zetta::solutions::ExperimentValue experiment_value;
    experiment_value.name = "renewal-experiment";
    experiment_value.seed = 1108;
    experiment_value.allocations = {
        {"holdout", zetta::solutions::ExperimentTier::Holdout, 0.5},
        {"treatment", zetta::solutions::ExperimentTier::Treatment, 0.5},
    };
    const auto experiment = node.experiment_manager().create(
        std::move(experiment_value));
    ASSERT_TRUE(experiment);

    zetta::solutions::RoiReadoutSpec spec;
    spec.solution = "renewal-risk";
    spec.project = project->id();
    spec.graph = "production";
    spec.experiment = experiment->id;
    spec.roi_model = {"renewed", 100.0, 500.0, "USD"};
    zetta::solutions::RoiReadoutStore store{replication};
    std::string error;
    ASSERT_TRUE(store.configure(std::move(spec), "owner", &error)) << error;

    Registry registry;
    zetta::api::register_core_capabilities(registry);
    ApiContext context{node};
    const ApiResult result = registry.call(
        "get_roi_readout",
        nlohmann::json{{"solution", "renewal-risk"}},
        context);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("solution"), "renewal-risk");
    EXPECT_FALSE(result.value.at("incrementality").at("claimable").get<bool>());
    EXPECT_NE(
        result.value.at("incrementality").at("reason").get<std::string>().find(
            "no holdout outcomes"),
        std::string::npos);
    node.stop();
}

TEST_F(RegistryFixture, ExperimentJourneyProtectsHoldoutAndReportsExemptions) {
    const ApiResult created = call(
        "create_experiment",
        {{"name", "retention-control"},
         {"seed", 1105},
         {"allocations",
          {{{"name", "holdout"},
            {"tier", "holdout"},
            {"fraction", 0.5}},
           {{"name", "treatment"},
            {"tier", "treatment"},
            {"fraction", 0.5}}}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string experiment = created.value.at("id").get<std::string>();

    std::string holdout_entity;
    for (std::uint64_t index = 0; index < 100; ++index) {
        const std::string entity = "account-" + std::to_string(index);
        const ApiResult decision = call(
            "assign", {{"experiment", experiment},
                       {"entity", entity},
                       {"proposed_action", "send-offer"}});
        ASSERT_TRUE(decision.ok) << decision.error.message;
        if (decision.value.at("assignment").at("tier") == "holdout") {
            EXPECT_FALSE(decision.value.at("allowed").get<bool>());
            EXPECT_TRUE(decision.value.at("action").get<std::string>().empty());
            holdout_entity = entity;
            break;
        }
    }
    ASSERT_FALSE(holdout_entity.empty());

    const ApiResult exempted = call(
        "assign", {{"experiment", experiment},
                   {"entity", holdout_entity},
                   {"proposed_action", "send-required-notice"},
                   {"exempt", true},
                   {"exemption_reason", "contract requirement"}});
    ASSERT_TRUE(exempted.ok) << exempted.error.message;
    EXPECT_TRUE(exempted.value.at("allowed").get<bool>());
    EXPECT_EQ(exempted.value.at("action"), "send-required-notice");
    EXPECT_EQ(exempted.value.at("assignment").at("exemption_actor"), "local");
    EXPECT_EQ(exempted.value.at("assignment").at("exemption_reason"),
              "contract requirement");

    const ApiResult report =
        call("get_experiment", {{"experiment", experiment}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("sizes").at("exemptions"), 1);
    EXPECT_EQ(report.value.at("sizes").at("holdout_exemptions"), 1);
    EXPECT_GT(report.value.at("erosion").at("exemption_rate").get<double>(),
              0.0);
    EXPECT_GT(
        report.value.at("erosion").at("holdout_erosion_rate").get<double>(),
        0.0);
    EXPECT_FALSE(report.value.at("training_rows").empty());
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

// #1063: an offloadable capability (receive_events' long poll) runs on a pool
// worker, but the call's authorization + audit gate touches owner-thread state —
// the audit trail the tick reconciles. The gate used to run on the worker, and
// the lake bus's constant long polls crashed its Zetta every few minutes (a
// double free, or a bad access inside the tick's reconciliation). Workers
// hammer an offloadable read while this thread ticks: every call completes,
// every call is audited, and the handler itself still runs on the worker.
TEST_F(RegistryFixture, OffloadedCallsRunTheirGateOnTheOwnerThread) {
    node.tick();  // adopt this thread as the owner
    ASSERT_TRUE(node.on_owner_thread());
    const std::thread::id owner = std::this_thread::get_id();
    std::atomic<int> handled_off_owner{0};
    Capability poll{"offloaded_poll", "", "", nlohmann::json{{"type", "object"}}, {},
                    CapabilityKind::Read,
                    zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "query"},
                    false, false,
                    [&](const nlohmann::json&, ApiContext&) -> ApiResult {
                        if (std::this_thread::get_id() != owner) ++handled_off_owner;
                        return ApiResult::success({});
                    }};
    poll.offloadable = true;
    ASSERT_TRUE(registry.add(std::move(poll)));

    constexpr int kWorkers = 8;
    constexpr int kCalls = 200;
    const auto audit_before = node.audit_event_count();
    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            for (int i = 0; i < kCalls; ++i) {
                ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
                if (!registry.call("offloaded_poll", nlohmann::json::object(), context).ok) {
                    ++failed;
                }
            }
            ++done;
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (done.load() < kWorkers && std::chrono::steady_clock::now() < deadline) {
        node.tick();  // the tick reconciles the audit trail while the gates queue
    }
    for (auto& worker : workers) worker.join();
    ASSERT_EQ(done.load(), kWorkers);
    EXPECT_EQ(failed.load(), 0);
    EXPECT_EQ(handled_off_owner.load(), kWorkers * kCalls)
        << "the handler itself stays on the worker";
    EXPECT_EQ(node.audit_event_count(), audit_before + kWorkers * kCalls)
        << "every offloaded call is audited, once";
}

// The consent path and the activity hook follow the gate on the calling thread,
// so an offloadable entry may do neither (#1063).
TEST_F(RegistryFixture, RefusesAnOffloadableEntryThatMutatesOrNeedsConsent) {
    for (const bool mutates : {true, false}) {
        Capability entry{mutates ? "offloaded_write" : "offloaded_consent", "", "",
                         nlohmann::json{{"type", "object"}}, {}, CapabilityKind::Read,
                         {}, mutates, !mutates,
                         [](const nlohmann::json&, ApiContext&) {
                             return ApiResult::success({});
                         }};
        entry.offloadable = true;
        EXPECT_FALSE(registry.add(std::move(entry)));
    }
}

TEST_F(RegistryFixture, RefusesAnInvalidAwaitableRead) {
    Capability entry{"awaitable_action", "", "",
                     nlohmann::json{{"type", "object"}}, {},
                     CapabilityKind::Action, {}, false, false,
                     [](const nlohmann::json&, ApiContext&) {
                         return ApiResult::success({});
                     }};
    entry.awaitable_read = zetta::api::AwaitableRead{
        [](const nlohmann::json&) {
            return std::chrono::milliseconds(1);
        },
        [](const ApiResult&) { return true; }};
    EXPECT_FALSE(registry.add(std::move(entry)));
}

// A retrying awaitable (#1005, acquire_object_lease) may be a blocking action
// that mutates: each wake makes a whole governed call. It still may not be
// offloadable or consent-gated.
TEST_F(RegistryFixture, AcceptsARetryingAwaitableActionThatBlocks) {
    const auto entry = [](std::string name) {
        Capability capability{std::move(name), "", "", nlohmann::json{{"type", "object"}}, {},
                              CapabilityKind::Action, {}, /*mutates=*/true, false,
                              [](const nlohmann::json&, ApiContext&) {
                                  return ApiResult::success({});
                              }};
        capability.blocks = true;
        capability.awaitable_read = zetta::api::AwaitableRead{
            [](const nlohmann::json&) { return std::chrono::milliseconds(1); },
            [](const ApiResult&) { return true; },
            [](const ApiResult&) { return std::chrono::milliseconds(1); }};
        return capability;
    };
    EXPECT_TRUE(registry.add(entry("retrying_action")));
    Capability without_retry = entry("waiting_action");
    without_retry.awaitable_read->retry_within = nullptr;
    EXPECT_FALSE(registry.add(std::move(without_retry)));
    Capability consent = entry("consent_action");
    consent.blocks = false;
    consent.requires_consent = true;
    EXPECT_FALSE(registry.add(std::move(consent)));
}

TEST_F(RegistryFixture, RefusesReadSummariesOnStateChangingCapabilities) {
    Capability entry{"summarized_action", "", "",
                     nlohmann::json{{"type", "object"}}, {},
                     CapabilityKind::Action, {}, true, false,
                     [](const nlohmann::json&, ApiContext&) {
                         return ApiResult::success({});
                     }};
    entry.audit_policy = zetta::api::AuditPolicy::SummarizeAuthorizedReads;
    EXPECT_FALSE(registry.add(std::move(entry)));
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

TEST_F(RegistryFixture, DeniedAuditPurgesAreRecordedOneByOne) {
    auto principal = node.create_principal(
        "audit reader", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    auto key = principal->create_key(zetta::governance::unrestricted_scope());
    ASSERT_TRUE(key);
    ApiContext context{node, nullptr, key->id()};
    const auto before = node.audit_event_count();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = registry.call(
            "purge_audit",
            {{"before", "2026-01-01T00:00:00Z"}}, context);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "unauthorized");
    }
    ASSERT_EQ(node.audit_event_count(), before + 2);
    const auto events = node.audit_events();
    EXPECT_FALSE(events[events.size() - 2]->allowed());
    EXPECT_FALSE(events.back()->allowed());
    EXPECT_EQ(events[events.size() - 2]->target(), "api:purge_audit");
    EXPECT_EQ(events.back()->target(), "api:purge_audit");
}

TEST_F(RegistryFixture, SqlReadsTheSystemViews) {
    auto result =
        call("sql", nlohmann::json{{"statement", "SELECT * FROM system.datasets;"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.contains("rows"));
}

TEST_F(RegistryFixture, SnapshotMaterializesAReplicatedPointInTimeTable) {
    const auto database = node.create_database("features");
    ASSERT_TRUE(database);
    const auto events = database->create_table("events");
    ASSERT_TRUE(events);
    events->add_row({{"account", "a"}, {"observed_at", 1}, {"value", 10}});
    events->add_row({{"account", "a"}, {"observed_at", 3}, {"value", 30}});

    const ApiResult result = call(
        "snapshot",
        {{"database", "features"},
         {"source_table", "events"},
         {"target_table", "account_snapshots"},
         {"entity_column", "account"},
         {"time_column", "observed_at"},
         {"as_of", 2}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("materialized_rows"), 1u);
    EXPECT_EQ(result.value.at("future_rows_excluded"), 1u);
    const auto snapshots = database->find_table("account_snapshots");
    ASSERT_TRUE(snapshots);
    ASSERT_EQ(snapshots->row_count(), 1u);
    const auto row = snapshots->store().at(0);
    ASSERT_TRUE(row) << row.error();
    EXPECT_EQ(row->at("value"), 10);
    EXPECT_EQ(row->at("entity_id"), "a");
    EXPECT_EQ(row->at("as_of"), 2);
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

// #1185: list_projects answers a list page in one call. `projects` stays the
// names (existing callers), and `summaries` carries, in the same order, each
// project's id and name and each graph's id, name and approval, matching
// get_project's answer exactly. An internal project (lake#654) is in neither.
TEST_F(RegistryFixture, ListProjectsReturnsGraphSummariesMatchingGetProject) {
    ASSERT_TRUE(call("create_project", nlohmann::json{{"name", "churn"}}).ok);
    ASSERT_TRUE(call("create_project", nlohmann::json{{"name", "empty"}}).ok);
    ASSERT_TRUE(node.create_project("m (standalone model)", "", "", /*internal=*/true));
    for (const char* text : {"predict which accounts churn",
                             "predict which accounts renew"}) {
        auto captured = call("capture_intent",
                             nlohmann::json{{"project", "churn"}, {"text", text}});
        ASSERT_TRUE(captured.ok) << captured.error.message;
        auto compiled = call("compile_intent",
                             nlohmann::json{{"project", "churn"},
                                            {"input_id", captured.value.at("input_id")}});
        ASSERT_TRUE(compiled.ok) << compiled.error.message;
    }
    const auto churn = node.find_project("churn");
    ASSERT_TRUE(churn);
    ASSERT_GE(churn->graphs().size(), 2u);
    const std::string approved_graph = churn->graphs().front()->id();
    auto approved = call("approve_plan", nlohmann::json{{"project", "churn"},
                                                        {"graph", approved_graph}});
    ASSERT_TRUE(approved.ok) << approved.error.message;

    auto listed = call("list_projects", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    const auto& projects = listed.value.at("projects");
    const auto& summaries = listed.value.at("summaries");
    EXPECT_EQ(projects, nlohmann::json(node.project_names()));  // unchanged shape
    ASSERT_EQ(projects.size(), 2u);  // the internal project is left out
    ASSERT_EQ(summaries.size(), projects.size());

    // Each summary is exactly get_project's id, name and graphs' id/name/approval.
    for (std::size_t i = 0; i < summaries.size(); ++i) {
        const auto& summary = summaries[i];
        EXPECT_EQ(summary.at("name"), projects[i]);
        auto detail = call("get_project", nlohmann::json{{"name", projects[i]}});
        ASSERT_TRUE(detail.ok) << detail.error.message;
        EXPECT_EQ(summary.at("id"), detail.value.at("id"));
        const auto& graphs = summary.at("graphs");
        const auto& full = detail.value.at("graphs");
        ASSERT_EQ(graphs.size(), full.size());
        for (std::size_t g = 0; g < graphs.size(); ++g) {
            EXPECT_EQ(graphs[g],
                      (nlohmann::json{{"id", full[g].at("id")},
                                      {"name", full[g].at("name")},
                                      {"approval", full[g].at("approval")}}));
        }
    }

    const std::size_t at_churn = projects[0] == "churn" ? 0 : 1;
    const auto& churn_summary = summaries[at_churn];
    EXPECT_EQ(churn_summary.at("id"), churn->id());
    std::size_t approved_count = 0;
    for (const auto& graph : churn_summary.at("graphs")) {
        if (graph.at("approval") == "approved") {
            ++approved_count;
            EXPECT_EQ(graph.at("id"), approved_graph);
        } else {
            EXPECT_EQ(graph.at("approval"), "proposed");
        }
    }
    EXPECT_EQ(approved_count, 1u);
    EXPECT_TRUE(summaries[1 - at_churn].at("graphs").empty());
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

// lake#645: a dataset is deleted with its partitions and every index built over
// it; a dry run answers the same list and changes nothing.
TEST_F(RegistryFixture, DeleteDatasetRemovesItsPartitionsAndIndexes) {
    auto database = node.create_database("data");
    auto table = database->create_table("orders");
    ASSERT_TRUE(table);
    table->add_row({{"id", 1}, {"note", "first"}});
    table->add_row({{"id", 2}, {"note", "second"}});
    const std::string table_id = table->id();
    auto keeper = database->create_table("customers");
    ASSERT_TRUE(keeper);

    zetta::data::DatasetPartitionValue partition_value;
    partition_value.table = table_id;
    partition_value.row_range = {0, 2};
    auto partition = node.create_dataset_partition(std::move(partition_value));
    ASSERT_TRUE(partition);

    zetta::index::IndexValue over_orders;
    over_orders.table = "orders";
    over_orders.backend = "memory_search";
    over_orders.fields = {"note"};
    auto index = node.create_index(over_orders);
    ASSERT_TRUE(index);
    zetta::index::IndexValue over_customers = over_orders;
    over_customers.table = "customers";
    auto other_index = node.create_index(over_customers);
    ASSERT_TRUE(other_index);

    auto preview = call("delete_dataset", {{"dataset", "data.orders"}, {"dry_run", true}});
    ASSERT_TRUE(preview.ok) << preview.error.code << ": " << preview.error.message;
    EXPECT_EQ(preview.value.at("dataset"), "orders");
    EXPECT_EQ(preview.value.at("database"), "data");
    EXPECT_EQ(preview.value.at("table_id"), table_id);
    EXPECT_TRUE(preview.value.at("dry_run").get<bool>());
    EXPECT_EQ(preview.value.at("removed").at("partitions"), 1u);
    EXPECT_EQ(preview.value.at("removed").at("indexes"),
              nlohmann::json::array({index->id()}));
    // Nothing changed.
    EXPECT_TRUE(database->find_table("orders"));
    EXPECT_TRUE(node.find_index(index->id()));
    EXPECT_TRUE(node.find_dataset_partition(partition->id()));

    auto deleted = call("delete_dataset", {{"dataset", "orders"}, {"database", "data"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.code << ": " << deleted.error.message;
    EXPECT_FALSE(deleted.value.at("dry_run").get<bool>());
    EXPECT_EQ(deleted.value.at("removed"), preview.value.at("removed"));
    EXPECT_FALSE(database->find_table("orders"));
    EXPECT_FALSE(node.find_index(index->id()));
    EXPECT_FALSE(node.find_dataset_partition(partition->id()));
    // The other dataset and its index are untouched.
    EXPECT_TRUE(database->find_table("customers"));
    EXPECT_TRUE(node.find_index(other_index->id()));
    auto read = call("get_dataset", {{"dataset", "orders"}});
    EXPECT_EQ(read.error.code, "not_found");

    auto again = call("delete_dataset", {{"dataset", "orders"}});
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.error.code, "not_found");
    // By table id too.
    auto by_id = call("delete_dataset", {{"dataset", keeper->id()}, {"dry_run", true}});
    ASSERT_TRUE(by_id.ok) << by_id.error.message;
    EXPECT_EQ(by_id.value.at("dataset"), "customers");
}

// zetta#1258: a run records the tables it reads when it is created (its
// pipeline's connect steps, and a dataset it is handed), a study records its
// dataset, and delete_dataset refuses the dataset while any of them is in
// flight, its dry run too, naming them.
TEST_F(RegistryFixture, DeleteDatasetRefusesWhileARunOrAStudyReadsIt) {
    auto database = node.create_database("data");
    auto table = database->create_table("orders");
    ASSERT_TRUE(table);
    ASSERT_TRUE(database->create_table("customers"));

    auto project = node.create_project("reader");
    ASSERT_TRUE(project);
    zetta::ir::ExecutionGraph graph;
    auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    ASSERT_TRUE(connect);
    connect->attributes = {{"source", "data"}, {"table", "orders"}};
    auto pipeline = project->create_graph("training", std::move(graph));
    ASSERT_TRUE(pipeline);

    // The pipeline's run records the table before it reads anything.
    auto run = node.create_run(project->id(), pipeline->id());
    ASSERT_TRUE(run);
    ASSERT_EQ(run->reads().size(), 1u);
    EXPECT_EQ(run->reads().front().database, "data");
    EXPECT_EQ(run->reads().front().table, "orders");
    EXPECT_EQ(run->reads().front().table_id, table->id());
    // ...and it survives the replicated/on-disk round trip.
    const auto parsed = zetta::pipeline::parse_run_value(run->metadata());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->reads, run->reads());
    // A table it does not read is not recorded.
    EXPECT_FALSE(zetta::pipeline::reads_table(run->reads(), "data", "customers", ""));

    for (const bool dry_run : {true, false}) {
        auto refused = call("delete_dataset", {{"dataset", "data.orders"}, {"dry_run", dry_run}});
        EXPECT_FALSE(refused.ok);
        EXPECT_EQ(refused.error.code, "engine_error");
        EXPECT_NE(refused.error.message.find(run->id()), std::string::npos)
            << refused.error.message;
        EXPECT_NE(refused.error.message.find("pending"), std::string::npos)
            << refused.error.message;
    }
    EXPECT_TRUE(database->find_table("orders"));
    // A dataset the run does not read is deleted as before.
    EXPECT_TRUE(call("delete_dataset", {{"dataset", "data.customers"}, {"dry_run", true}}).ok);

    run->set_status(zetta::pipeline::RunStatus::Running);
    EXPECT_EQ(call("delete_dataset", {{"dataset", "data.orders"}}).error.code, "engine_error");
    run->set_status(zetta::pipeline::RunStatus::Succeeded);

    // A run handed the dataset (no pipeline of its own) records it too.
    auto handed = node.create_run("");
    ASSERT_TRUE(handed);
    EXPECT_TRUE(handed->reads().empty());
    handed->add_reads(node.tables_read({}, {}, "data.orders"));
    handed->add_reads(node.tables_read({}, {}, "data.orders"));  // once
    ASSERT_EQ(handed->reads().size(), 1u);
    handed->set_status(zetta::pipeline::RunStatus::Running);
    auto by_handed = call("delete_dataset", {{"dataset", table->id()}, {"dry_run", true}});
    EXPECT_EQ(by_handed.error.code, "engine_error");
    EXPECT_NE(by_handed.error.message.find(handed->id()), std::string::npos)
        << by_handed.error.message;
    handed->set_status(zetta::pipeline::RunStatus::Failed);

    // A running study on the dataset keeps it, though no trial is in flight.
    zetta::ml::StudySpec spec;
    spec.name = "orders-tuning";
    spec.dataset = "data.orders";
    spec.methodology = "M-1";
    spec.objective.metric = "validation.score";
    spec.objective.validation_split = "validation";
    zetta::ml::StudyParameter parameter;
    parameter.name = "x";
    parameter.minimum = 0.0;
    parameter.maximum = 1.0;
    spec.search_space = {parameter};
    std::string error;
    auto study = node.create_study(std::move(spec), &error);
    ASSERT_TRUE(study) << error;
    ASSERT_EQ(study->value().reads.size(), 1u);
    EXPECT_EQ(study->value().reads.front().table_id, table->id());
    const auto study_parsed = zetta::ml::parse_study_value(study->metadata());
    ASSERT_TRUE(study_parsed);
    EXPECT_EQ(study_parsed->reads, study->value().reads);
    auto by_study = call("delete_dataset", {{"dataset", "data.orders"}, {"dry_run", true}});
    EXPECT_EQ(by_study.error.code, "engine_error");
    EXPECT_NE(by_study.error.message.find("orders-tuning"), std::string::npos)
        << by_study.error.message;
    study->stop("done");

    // Nothing in flight reads it: the delete goes through.
    auto deleted = call("delete_dataset", {{"dataset", "data.orders"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.code << ": " << deleted.error.message;
    EXPECT_FALSE(database->find_table("orders"));
}

// zetta#1272: a deployment records the tables its placed graph reads, and the
// run the deployment controller starts for it records them, so delete_dataset
// refuses them while that run is pending or running. The deployment has no
// stored project or pipeline to read them from.
TEST_F(RegistryFixture, DeleteDatasetRefusesWhileADeploymentRunReadsIt) {
    auto database = node.create_database("data");
    auto table = database->create_table("orders");
    ASSERT_TRUE(table);
    zetta::ir::ExecutionGraph graph;
    auto connect = graph.add_node(zetta::ir::StepKind::Connect, "load_orders");
    auto clean = graph.add_node(zetta::ir::StepKind::Clean, "dedupe");
    auto model = graph.add_node(zetta::ir::StepKind::Model, "churn_model");
    connect->attributes = {{"source", "data"}, {"table", "orders"}};
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);

    auto deployment = node.deploy_graph(graph, zetta::pipeline::DeploymentTarget::local(),
                                        "churn");
    ASSERT_TRUE(deployment);
    ASSERT_EQ(deployment->reads().size(), 1u);
    EXPECT_EQ(deployment->reads().front().table_id, table->id());
    // Replicated and on disk with the deployment.
    EXPECT_EQ(zetta::pipeline::table_reads_from_json(deployment->metadata().at("reads")),
              deployment->reads());

    node.deployment_controller().reconcile();
    std::shared_ptr<zetta::pipeline::Run> run;
    for (const auto& candidate : node.runs()) {
        if (candidate->deployment() == deployment->id()) run = candidate;
    }
    ASSERT_TRUE(run);
    ASSERT_EQ(run->status(), zetta::pipeline::RunStatus::Running);
    EXPECT_EQ(run->reads(), deployment->reads());

    auto refused = call("delete_dataset", {{"dataset", "data.orders"}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "engine_error");
    EXPECT_NE(refused.error.message.find(run->id()), std::string::npos)
        << refused.error.message;
    EXPECT_TRUE(database->find_table("orders"));

    run->set_status(zetta::pipeline::RunStatus::Succeeded);
    auto deleted = call("delete_dataset", {{"dataset", "data.orders"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.code << ": " << deleted.error.message;
}

// zetta#1272: GraphExecutor records what the graph it executes reads before
// the first step runs, though the graph is not a stored pipeline, so a
// delete_dataset that comes while a step runs is refused.
TEST_F(RegistryFixture, DeleteDatasetRefusesWhileAGraphExecutorRunReadsIt) {
    auto database = node.create_database("data");
    auto table = database->create_table("orders");
    ASSERT_TRUE(table);
    zetta::ir::ExecutionGraph graph;
    auto connect = graph.add_node(zetta::ir::StepKind::Connect, "load_orders");
    connect->attributes = {{"source", "data"}, {"table", "orders"}};

    // A step that tries the delete while it runs.
    struct DeletingStep final : zetta::pipeline::StepExecutor {
        RegistryFixture* fixture = nullptr;
        mutable ApiResult attempted;
        zetta::pipeline::StepOutcome run_step(const zetta::ir::Node&) const override {
            attempted = fixture->call("delete_dataset", {{"dataset", "data.orders"}});
            return {};
        }
    };
    auto step = std::make_shared<DeletingStep>();
    step->fixture = this;
    const zetta::pipeline::ExecutionResult result =
        zetta::pipeline::GraphExecutor(step).execute(node, graph, "", "");
    ASSERT_TRUE(result.run);
    EXPECT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.run->reads().size(), 1u);
    EXPECT_EQ(result.run->reads().front().table_id, table->id());
    EXPECT_FALSE(step->attempted.ok);
    EXPECT_EQ(step->attempted.error.code, "engine_error");
    EXPECT_NE(step->attempted.error.message.find(result.run->id()), std::string::npos)
        << step->attempted.error.message;
    EXPECT_TRUE(database->find_table("orders"));
}

TEST_F(RegistryFixture, DeleteModelStopsServingItAndKeepsItsRun) {
    auto run = node.create_run("");
    ASSERT_TRUE(run);
    run->set_status(zetta::pipeline::RunStatus::Succeeded);
    zetta::ml::ModelValue value;
    value.kind = "classifier";
    value.name = "churn-model";
    value.run = run->id();
    auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);
    model->set_exposed(true);

    auto preview = call("delete_model", {{"model_id", "churn-model"}, {"dry_run", true}});
    ASSERT_TRUE(preview.ok) << preview.error.code << ": " << preview.error.message;
    EXPECT_EQ(preview.value.at("model_id"), model->id());
    EXPECT_EQ(preview.value.at("name"), "churn-model");
    EXPECT_TRUE(preview.value.at("removed").at("was_exposed").get<bool>());
    EXPECT_EQ(preview.value.at("kept").at("run"), run->id());
    EXPECT_TRUE(node.find_model(model->id()));
    EXPECT_TRUE(model->exposed());

    const std::string id = model->id();
    auto deleted = call("delete_model", {{"model_id", id}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_FALSE(node.find_model(id));
    EXPECT_TRUE(node.find_run(run->id()));
    EXPECT_EQ(call("get_model", {{"model_id", id}}).error.code, "not_found");
    EXPECT_EQ(call("delete_model", {{"model_id", id}}).error.code, "not_found");
}

TEST_F(RegistryFixture, DeleteModelByNameRefusesAnAmbiguousName) {
    for (int i = 0; i < 2; ++i) {
        zetta::ml::ModelValue value;
        value.kind = "classifier";
        value.name = "twin";
        ASSERT_TRUE(node.create_model(std::move(value)));
    }
    auto refused = call("delete_model", {{"model_id", "twin"}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "engine_error");
    EXPECT_EQ(node.models().size(), 2u);
}

TEST_F(RegistryFixture, DeleteRunRefusesWhileInFlightAndKeepsItsModels) {
    auto run = node.create_run("");
    ASSERT_TRUE(run);
    run->set_status(zetta::pipeline::RunStatus::Running);
    zetta::ml::ModelValue value;
    value.kind = "classifier";
    value.run = run->id();
    auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);

    auto refused = call("delete_run", {{"run_id", run->id()}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "engine_error");
    EXPECT_NE(refused.error.message.find("running"), std::string::npos)
        << refused.error.message;
    EXPECT_TRUE(node.find_run(run->id()));

    run->set_status(zetta::pipeline::RunStatus::Succeeded);
    auto preview = call("delete_run", {{"run_id", run->id()}, {"dry_run", true}});
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_EQ(preview.value.at("removed").at("status"), "succeeded");
    EXPECT_EQ(preview.value.at("kept").at("models"), nlohmann::json::array({model->id()}));
    EXPECT_TRUE(node.find_run(run->id()));

    const std::string id = run->id();
    auto deleted = call("delete_run", {{"run_id", id}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value.at("run_id"), id);
    EXPECT_FALSE(node.find_run(id));
    EXPECT_TRUE(node.find_model(model->id()));
    EXPECT_EQ(call("delete_run", {{"run_id", id}}).error.code, "not_found");
}

TEST_F(RegistryFixture, DeleteIndexStopsServingItAndKeepsTheDataset) {
    auto database = node.create_database("data");
    ASSERT_TRUE(database->create_table("articles"));
    zetta::index::IndexValue value;
    value.table = "articles";
    value.backend = "memory_search";
    value.fields = {"title"};
    auto index = node.create_index(value);
    ASSERT_TRUE(index);
    index->set_exposed(true);

    auto preview = call("delete_index", {{"index_id", index->id()}, {"dry_run", true}});
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_EQ(preview.value.at("table"), "articles");
    EXPECT_TRUE(preview.value.at("removed").at("was_exposed").get<bool>());
    EXPECT_TRUE(node.find_index(index->id()));

    const std::string id = index->id();
    auto deleted = call("delete_index", {{"index_id", id}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_FALSE(node.find_index(id));
    EXPECT_TRUE(database->find_table("articles"));
    EXPECT_EQ(call("delete_index", {{"index_id", id}}).error.code, "not_found");
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

    zetta::solutions::MonitorSpec monitor_spec;
    monitor_spec.solution = "renewal-risk";
    monitor_spec.inputs = {
        {"tenure", {12.0}, {0.5, 0.5}},
    };
    monitor_spec.guardrails = {
        {"cost", 0.0, 100.0},
    };
    auto monitor = node.create_monitor(std::move(monitor_spec));
    ASSERT_TRUE(monitor);

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
        {"get_monitor", "solution", monitor->id(), monitor->metadata()},
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

    auto monitor_by_solution = call(
        "get_monitor",
        nlohmann::json{{"solution", "renewal-risk"}});
    ASSERT_TRUE(monitor_by_solution.ok) << monitor_by_solution.error.message;
    EXPECT_EQ(monitor_by_solution.value.at("id"), monitor->id());
}

// Journey (#616): the list_* siblings return compact structured rows over the
// same live registries the system.* views compute from, with the ids feeding
// the get_* detail reads.
TEST_F(RegistryFixture, JourneyListReadsReturnCompactRows) {
    const auto project = node.create_project("churn-list");
    ASSERT_TRUE(project);
    const auto pipeline = project->create_graph(
        "training", zetta::ir::ExecutionGraph{});
    ASSERT_TRUE(pipeline);
    auto run = node.create_run(project->id(), pipeline->id());
    ASSERT_TRUE(run);
    std::string error;
    ASSERT_TRUE(run->log_metrics(1, {{"validation.auc", 0.91}}, {}, &error))
        << error;
    zetta::ml::ModelValue model_value;
    model_value.name = "churn-model";
    model_value.kind = "classifier";
    model_value.project = project->id();
    model_value.run = run->id();
    model_value.metrics = {{"auc", 0.91}};
    model_value.feature_schema = {{"features", {"age", "tenure"}}};
    auto model = node.create_model(std::move(model_value));
    ASSERT_TRUE(model);
    model->set_exposed(true);
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
    EXPECT_EQ(runs.value.at("runs")[0].at("project_name"), "churn-list");
    EXPECT_EQ(runs.value.at("runs")[0].at("pipeline_name"), "training");
    EXPECT_EQ(runs.value.at("runs")[0].at("metric_summary")
                  .at("validation.auc")
                  .at("final"),
              0.91);

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
    EXPECT_EQ(models.value.at("models")[0].at("project_name"), "churn-list");
    EXPECT_EQ(models.value.at("models")[0].at("pipeline_name"), "training");
    EXPECT_EQ(models.value.at("models")[0].at("run_id"), run->id());
    EXPECT_EQ(models.value.at("models")[0].at("metrics").at("auc"), 0.91);
    EXPECT_EQ(models.value.at("models")[0].at("feature_schema")
                  .at("features").size(),
              2u);
    EXPECT_TRUE(models.value.at("models")[0].at("exposed").get<bool>());
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
    // #1021: get_dataset's columns are typed objects. No order was recorded
    // for this table, so they come first-seen (the stored keys, sorted) and the
    // answer says so.
    EXPECT_EQ(fetched.value.at("column_order"), "first_seen");
    EXPECT_EQ(fetched.value.at("columns"),
              (nlohmann::json::array(
                  {{{"name", "action"}, {"type", "string"}, {"nullable", false},
                    {"null_fraction", 0.0}},
                   {{"name", "user"}, {"type", "string"}, {"nullable", false},
                    {"null_fraction", 0.0}}})));
    EXPECT_TRUE(fetched.value.at("partitions").empty());

    auto listed = call("list_datasets", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("datasets").size(), 1u);
    EXPECT_EQ(listed.value.at("datasets")[0].at("name"), "events");
    // list_datasets keeps the names (one string per column), in the same order.
    EXPECT_EQ(listed.value.at("datasets")[0].at("columns"),
              nlohmann::json::array({"action", "user"}));

    // A recorded source order (a CSV header, a SELECT list) leads; a typed
    // column reports the share of rows that hold null for it or omit it.
    auto ordered = database->create_table("signups");
    ASSERT_TRUE(ordered);
    ordered->set_column_order({"user", "plan", "seats"});
    ordered->add_row({{"user", "u1"}, {"plan", "pro"}, {"seats", 3}});
    ordered->add_row({{"user", "u2"}, {"plan", nullptr}, {"seats", 2.5}});
    ordered->add_row({{"user", "u3"}, {"seats", 1}});
    ordered->add_row({{"user", "u4"}, {"plan", "free"}, {"seats", 1}, {"note", "x"}});
    auto signups = call("get_dataset", nlohmann::json{{"dataset", "data.signups"}});
    ASSERT_TRUE(signups.ok) << signups.error.message;
    EXPECT_EQ(signups.value.at("column_order"), "source");
    const nlohmann::json& columns = signups.value.at("columns");
    ASSERT_EQ(columns.size(), 4u);
    EXPECT_EQ(columns[0].at("name"), "user");
    EXPECT_EQ(columns[1].at("name"), "plan");
    EXPECT_EQ(columns[2].at("name"), "seats");
    EXPECT_EQ(columns[3].at("name"), "note") << "a column the order does not name follows";
    EXPECT_EQ(columns[0].at("type"), "string");
    EXPECT_FALSE(columns[0].at("nullable").get<bool>());
    EXPECT_EQ(columns[1].at("type"), "string");
    EXPECT_TRUE(columns[1].at("nullable").get<bool>());
    EXPECT_DOUBLE_EQ(columns[1].at("null_fraction").get<double>(), 0.5);
    EXPECT_EQ(columns[2].at("type"), "number") << "integers widen to number beside a real";
    EXPECT_DOUBLE_EQ(columns[3].at("null_fraction").get<double>(), 0.75);
    auto signups_listed = call("list_datasets", nlohmann::json::object());
    ASSERT_TRUE(signups_listed.ok) << signups_listed.error.message;
    for (const nlohmann::json& dataset : signups_listed.value.at("datasets")) {
        if (dataset.at("name") != "signups") continue;
        EXPECT_EQ(dataset.at("columns"),
                  nlohmann::json::array({"user", "plan", "seats", "note"}));
        EXPECT_EQ(dataset.at("column_order"), "source");
    }
    ASSERT_TRUE(database->drop_table("signups"));

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

    // #1221: database.table, the form analyze_dataset takes, names the same
    // dataset; a database field that disagrees with it is refused.
    auto dotted = call("get_dataset", nlohmann::json{{"dataset", "data.events"}});
    ASSERT_TRUE(dotted.ok) << dotted.error.message;
    EXPECT_EQ(dotted.value.at("database"), "data");
    EXPECT_EQ(dotted.value.at("rows"), 2u);
    auto staged = call("get_dataset", nlohmann::json{{"dataset", "staging.events"},
                                                     {"database", "staging"}});
    ASSERT_TRUE(staged.ok) << staged.error.message;
    EXPECT_EQ(staged.value.at("database"), "staging");
    auto disagree = call("get_dataset", nlohmann::json{{"dataset", "data.events"},
                                                       {"database", "staging"}});
    EXPECT_FALSE(disagree.ok);
    EXPECT_EQ(disagree.error.code, "invalid_input");
    EXPECT_EQ(disagree.error.path, "/database");
    auto no_such = call("get_dataset", nlohmann::json{{"dataset", "data.nothing"}});
    EXPECT_FALSE(no_such.ok);
    EXPECT_EQ(no_such.error.code, "not_found");
    // A table whose own name holds a dot is still found by that name.
    ASSERT_TRUE(database->create_table("v1.events"));
    auto exact = call("get_dataset", nlohmann::json{{"dataset", "v1.events"}});
    ASSERT_TRUE(exact.ok) << exact.error.message;
    EXPECT_EQ(exact.value.at("name"), "v1.events");
}

// #1021: get_dataset's typed columns say nothing about the values of a column
// the caller may not read (a column-scoped sql:read): the column keeps its name
// and answers type, nullable and null_fraction as null, as SQL masks it.
TEST_F(RegistryFixture, GetDatasetMasksTheTypesOfColumnsTheCallerMayNotRead) {
    node.ensure_builtin_roles();
    auto database = node.create_database("sales");
    ASSERT_TRUE(database);
    auto table = database->create_table("customers");
    ASSERT_TRUE(table);
    table->set_column_order({"id", "name", "ssn"});
    table->add_row({{"id", 1}, {"name", "a"}, {"ssn", "111"}});
    table->add_row({{"id", 2}, {"name", "b"}, {"ssn", nullptr}});

    ASSERT_TRUE(node.create_role("clerk"));
    ASSERT_TRUE(node.grant_permissions("clerk", {"api:query", "mcp:call"}));
    ASSERT_TRUE(node.grant_scoped_permissions(
        "clerk", {"sql:read"},
        zetta::governance::Resource::column("sales", "customers", {"id", "name"})));
    const auto user = node.create_user("cleo");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), node.find_role("clerk")->id()));

    ApiContext context{node, nullptr, user->id(), trace, &node.span_recorder()};
    context.acting_is_user = true;
    const ApiResult masked = registry.call(
        "get_dataset", nlohmann::json{{"dataset", "customers"}}, context);
    ASSERT_TRUE(masked.ok) << masked.error.message;
    const nlohmann::json& columns = masked.value.at("columns");
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_EQ(columns[0].at("name"), "id");
    EXPECT_EQ(columns[0].at("type"), "integer");
    EXPECT_EQ(columns[1].at("type"), "string");
    EXPECT_EQ(columns[2],
              (nlohmann::json{{"name", "ssn"},
                              {"type", nullptr},
                              {"nullable", nullptr},
                              {"null_fraction", nullptr}}))
        << "the hidden column's name only";

    // The trusted local caller reads every column.
    const ApiResult whole = call("get_dataset", nlohmann::json{{"dataset", "customers"}});
    ASSERT_TRUE(whole.ok) << whole.error.message;
    EXPECT_EQ(whole.value.at("columns")[2].at("type"), "string");
    EXPECT_DOUBLE_EQ(whole.value.at("columns")[2].at("null_fraction").get<double>(), 0.5);
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

    // A second upload under a taken name is refused (#1221) unless the caller
    // asks for the next free name, and then the output says which it asked for.
    auto taken = call("upload_dataset",
                      nlohmann::json{{"table_name", "accounts"},
                                     {"format", "csv"},
                                     {"content", "account\n9\n"}});
    ASSERT_FALSE(taken.ok);
    EXPECT_EQ(taken.error.code, "conflict");
    EXPECT_EQ(taken.error.path, "/table_name");
    EXPECT_EQ(taken.error.message,
              "a dataset named 'accounts' already exists in datasets; choose another "
              "name, or pass if_exists: \"rename\" to save it under the next free name");
    auto again = call("upload_dataset",
                      nlohmann::json{{"table_name", "accounts"},
                                     {"format", "csv"},
                                     {"if_exists", "rename"},
                                     {"content", "account\n9\n"}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value.at("table"), "accounts_2");
    EXPECT_EQ(again.value.at("renamed_from"), "accounts");
    EXPECT_FALSE(csv.value.contains("renamed_from"));
}

// #1221: upload_dataset applies lake's rules (lake PR #459) in lake's words: a
// table or database name SQL can query unquoted, and a CSV whose every row has
// the header's field count — a ragged row is refused with its line, never
// padded with nulls or cut short. Nothing is created by a refusal.
TEST_F(RegistryFixture, UploadDatasetRefusesUnsafeNamesAndRaggedCsv) {
    const auto upload = [&](const std::string& table, const std::string& content,
                            const std::string& database = "datasets") {
        return call("upload_dataset", nlohmann::json{{"table_name", table},
                                                     {"database", database},
                                                     {"format", "csv"},
                                                     {"content", content}});
    };
    const auto refused = [](const ApiResult& result, const std::string& path,
                            const std::string& message) {
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "invalid_input");
        EXPECT_EQ(result.error.path, path);
        EXPECT_EQ(result.error.message, message);
    };
    refused(upload("my-table", "a\n1\n"), "/table_name",
            "table_name: 'my-table' can hold only letters, digits and underscores "
            "(not '-'), so SQL can query it");
    refused(upload("1st", "a\n1\n"), "/table_name",
            "table_name: '1st' must start with a letter or an underscore");
    refused(upload("Select", "a\n1\n"), "/table_name",
            "table_name: 'Select' is a SQL keyword; choose another name");
    refused(upload(std::string(64, 'a'), "a\n1\n"), "/table_name",
            "table_name: a name is at most 63 characters");
    refused(upload("fine", "a\n1\n", "my db"), "/database",
            "database: 'my db' can hold only letters, digits and underscores "
            "(not ' '), so SQL can query it");

    refused(upload("short_row", "a,b\n1,2\n3\n"), "/content",
            "the CSV file is malformed: line 3: 1 fields, but the header has 2 columns");
    refused(upload("long_row", "a,b\r\n1,2\r\n\r\n3,4,5\r\n"), "/content",
            "the CSV file is malformed: line 4: 3 fields, but the header has 2 columns");
    refused(upload("unnamed", "a,,c\n1,2,3\n"), "/content",
            "the CSV file is malformed: line 1: header column 2 has no name");
    refused(upload("twice", "a,b,a\n1,2,3\n"), "/content",
            "the CSV file is malformed: line 1: the header names column 'a' twice");
    refused(upload("open_quote", "a,b\n\"1,2\n"), "/content",
            "the CSV file is malformed: line 2: a quoted field is not closed (a row must "
            "stay on one line)");

    EXPECT_FALSE(node.find_database("datasets"));
    EXPECT_FALSE(node.find_database("my db"));

    // A quoted comma is one field, so this row matches its header and lands.
    auto quoted = upload("_quoted_ok2", "a,b\n\"x, y\",2\n");
    ASSERT_TRUE(quoted.ok) << quoted.error.message;
    EXPECT_EQ(quoted.value.at("rows"), 1u);
}

// #1221: a table records what its rows were loaded from. The storage format
// (`format`) is JSON rows for every table, so the upload's own format is kept
// as `source_format` — on get_dataset, list_datasets and system.datasets.
TEST_F(RegistryFixture, UploadedDatasetKeepsItsSourceFormat) {
    ASSERT_TRUE(call("upload_dataset", nlohmann::json{{"table_name", "from_csv"},
                                                      {"format", "csv"},
                                                      {"content", "a\n1\n"}})
                    .ok);
    ASSERT_TRUE(call("upload_dataset", nlohmann::json{{"table_name", "from_jsonl"},
                                                      {"format", "jsonl"},
                                                      {"content", "{\"a\":1}\n"}})
                    .ok);
    node.create_database("plain")->create_table("by_sql")->add_row({{"a", 1}});

    auto csv = call("get_dataset", nlohmann::json{{"dataset", "from_csv"}});
    ASSERT_TRUE(csv.ok) << csv.error.message;
    EXPECT_EQ(csv.value.at("source_format"), "csv");
    EXPECT_EQ(csv.value.at("format"), "jsonl");
    auto jsonl = call("get_dataset", nlohmann::json{{"dataset", "from_jsonl"}});
    ASSERT_TRUE(jsonl.ok) << jsonl.error.message;
    EXPECT_EQ(jsonl.value.at("source_format"), "jsonl");
    auto plain = call("get_dataset", nlohmann::json{{"dataset", "by_sql"}});
    ASSERT_TRUE(plain.ok) << plain.error.message;
    EXPECT_EQ(plain.value.at("source_format"), "");

    auto listed = call("list_datasets", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    std::map<std::string, std::string> listed_formats;
    for (const auto& dataset : listed.value.at("datasets")) {
        listed_formats[dataset.at("name").get<std::string>()] =
            dataset.at("source_format").get<std::string>();
    }
    EXPECT_EQ(listed_formats["from_csv"], "csv");
    EXPECT_EQ(listed_formats["from_jsonl"], "jsonl");

    auto view = call("sql", nlohmann::json{{"statement",
                                            "SELECT dataset, source_format FROM "
                                            "system.datasets WHERE dataset = 'from_csv';"}});
    ASSERT_TRUE(view.ok) << view.error.message;
    ASSERT_EQ(view.value.at("rows").size(), 1u) << view.value.dump();
    EXPECT_NE(view.value.at("rows").dump().find("\"csv\""), std::string::npos)
        << view.value.dump();
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

// #1408: client-supplied trace ids and tool grants are not diagnostic rights.
struct TraceAccessFixture : RegistryFixture {
    void SetUp() override {
        RegistryFixture::SetUp();
        node.ensure_builtin_roles();
        zetta::observability::ScopedSpan victim(
            &node.span_recorder(), trace, "api:search");
        victim.set_attribute("actor", "another-user");
        victim.set_attribute("intent", "confidential customer query");
        victim.finish(false, "confidential failure detail");

        zetta::observability::CrashReport crash;
        crash.id = "prior-crash";
        crash.signal_number = 11;
        crash.signal_name = "SIGSEGV";
        crash.crashed_at = "2026-10-22T00:00:00Z";
        crash.version = zetta::kVersion;
        crash.platform = zetta::observability::platform_string();
        crash.stacktrace = {"confidential stack frame"};
        crash.recent_spans = {zetta::observability::to_json(
            node.span_recorder().snapshot().front())};
        std::filesystem::create_directories(dir.path() / "crashes");
        std::ofstream(dir.path() / "crashes" / "prior-crash.json")
            << zetta::observability::to_json(crash).dump();
        const auto reports = zetta::observability::load_crash_reports(
            (dir.path() / "crashes").string());
        ASSERT_EQ(reports.size(), 1u);
        ASSERT_TRUE(reports.front().report);
    }

    std::string user_with_role(const std::string& role_name) {
        const auto user = node.create_user("user-" + role_name);
        EXPECT_TRUE(user);
        const auto role = node.find_role(role_name);
        EXPECT_TRUE(role);
        if (!user || !role) return {};
        EXPECT_TRUE(node.assign_role(user->id(), role->id()));
        return user->id();
    }

    ApiResult call_as(const std::string& actor, bool is_user,
                      const std::string& name, nlohmann::json input) {
        // Deliberately join the victim's trace, as an inbound traceparent can.
        ApiContext context{node, nullptr, actor, trace, &node.span_recorder()};
        context.acting_is_user = is_user;
        return registry.call(name, input, context);
    }

    nlohmann::json stored_bundle(const ApiResult& result) {
        const auto database = node.find_database("feedback");
        if (!database || !result.ok) return {};
        const auto bytes = database->large_objects().read(
            result.value.at("feedback_id").get<std::string>());
        EXPECT_TRUE(bytes);
        return bytes ? nlohmann::json::parse(*bytes) : nlohmann::json{};
    }
};

TEST_F(TraceAccessFixture, NativeReadersWritersAndToolGranteesCannotReadTraces) {
    ASSERT_TRUE(node.create_role("trace-tools"));
    for (const char* name : {"list_traces", "get_trace"}) {
        ASSERT_TRUE(node.grant_scoped_permissions(
            "trace-tools", {"mcp:call"}, zetta::governance::Resource::tool(name)));
    }
    for (const char* role : {"reader", "writer", "trace-tools"}) {
        SCOPED_TRACE(role);
        const auto user = user_with_role(role);
        for (const auto& [name, input] :
             std::vector<std::pair<std::string, nlohmann::json>>{
                 {"list_traces", {{"limit", 500}}},
                 {"get_trace", {{"trace_id", trace.trace_id}}},
                 {"get_trace", {{"trace_id", std::string(32, 'a')}}}}) {
            const auto denied = call_as(user, true, name, input);
            EXPECT_FALSE(denied.ok) << name;
            EXPECT_EQ(denied.error.code, "unauthorized") << name;
            EXPECT_TRUE(denied.value.is_null());
        }
    }
}

TEST_F(TraceAccessFixture, ExplicitDiagnosticGrantAllowsTracesAndRevocationDenies) {
    ASSERT_TRUE(node.create_role("diagnostics"));
    ASSERT_TRUE(node.grant_permissions("diagnostics", {"mcp:call", "cluster:admin"}));
    const auto user = user_with_role("diagnostics");
    for (const auto& [name, input] :
         std::vector<std::pair<std::string, nlohmann::json>>{
             {"list_traces", {{"limit", 1}}},
             {"get_trace", {{"trace_id", trace.trace_id}}}}) {
        const auto allowed = call_as(user, true, name, input);
        ASSERT_TRUE(allowed.ok) << allowed.error.message;
        ASSERT_FALSE(allowed.value.at("spans").empty());
    }
    const auto detail = call_as(user, true, "get_trace", {{"trace_id", trace.trace_id}});
    ASSERT_TRUE(detail.ok);
    EXPECT_NE(detail.value.dump().find("confidential customer query"), std::string::npos);
    auto feedback = call_as(user, true, "submit_feedback", {{"description", "debug"}});
    ASSERT_TRUE(feedback.ok) << feedback.error.message;
    const auto bundle = stored_bundle(feedback);
    EXPECT_FALSE(bundle.at("traces").empty());
    EXPECT_FALSE(bundle.at("recent_errors").empty());
    EXPECT_EQ(bundle.at("crashes").size(), 1u);

    ASSERT_TRUE(node.revoke_permissions("diagnostics", {"cluster:admin"}));
    EXPECT_FALSE(call_as(user, true, "list_traces", {{"limit", 1}}).ok);
    EXPECT_FALSE(call_as(user, true, "get_trace", {{"trace_id", trace.trace_id}}).ok);
    feedback = call_as(user, true, "submit_feedback", {{"description", "after revoke"}});
    ASSERT_TRUE(feedback.ok) << feedback.error.message;
    EXPECT_TRUE(stored_bundle(feedback).at("traces").empty());
    EXPECT_TRUE(stored_bundle(feedback).at("recent_errors").empty());
    EXPECT_TRUE(stored_bundle(feedback).at("crashes").empty());
}

TEST_F(TraceAccessFixture, LegacyKeysNeedAnExplicitDiagnosticGrantToo) {
    for (const bool diagnostic : {false, true}) {
        SCOPED_TRACE(diagnostic);
        const auto principal = node.create_principal(diagnostic ? "operator" : "reader");
        ASSERT_TRUE(principal);
        zetta::governance::CapabilitySet capabilities;
        capabilities.grant(zetta::governance::CapabilityDomain::Api, "query");
        if (diagnostic) capabilities.grant(zetta::governance::CapabilityDomain::Api,
                                           "cluster_admin");
        const auto key = principal->create_key(zetta::governance::unrestricted_scope(),
                                                capabilities);
        ASSERT_TRUE(key);
        EXPECT_EQ(call_as(key->id(), false, "list_traces", {{"limit", 1}}).ok, diagnostic);
        EXPECT_EQ(call_as(key->id(), false, "get_trace", {{"trace_id", trace.trace_id}}).ok,
                  diagnostic);
        const auto submitted = call_as(key->id(), false, "submit_feedback",
                                        {{"description", "legacy feedback"}});
        ASSERT_TRUE(submitted.ok) << submitted.error.message;
        const auto bundle = stored_bundle(submitted);
        EXPECT_EQ(!bundle.at("traces").empty(), diagnostic);
        EXPECT_EQ(!bundle.at("recent_errors").empty(), diagnostic);
        EXPECT_EQ(!bundle.at("crashes").empty(), diagnostic);
    }
}

TEST_F(TraceAccessFixture, OrdinaryFeedbackOmitsAllDiagnosticsEvenForNamedTraces) {
    for (const char* role : {"reader", "writer"}) {
        const auto user = user_with_role(role);
        for (const bool explicit_trace : {false, true}) {
            SCOPED_TRACE(role);
            SCOPED_TRACE(explicit_trace);
            nlohmann::json input{{"description", "please improve this"}};
            if (explicit_trace) input["trace_ids"] = {trace.trace_id};
            const auto submitted = call_as(user, true, "submit_feedback", input);
            ASSERT_TRUE(submitted.ok) << submitted.error.message;
            for (const char* count : {"traces_attached", "errors_attached", "crashes_attached"}) {
                EXPECT_EQ(submitted.value.at(count), 0);
            }
            const auto bundle = stored_bundle(submitted);
            EXPECT_EQ(bundle.at("description"), "please improve this");
            EXPECT_EQ(bundle.at("actor"), user);
            EXPECT_TRUE(bundle.at("traces").empty());
            EXPECT_TRUE(bundle.at("recent_errors").empty());
            EXPECT_TRUE(bundle.at("crashes").empty());
            EXPECT_EQ(bundle.dump().find("confidential"), std::string::npos);
        }
    }
}

TEST_F(TraceAccessFixture, FeedbackBuilderOmitsDiagnosticsByDefault) {
    zetta::observability::FeedbackBundleOptions options;
    options.description = "ordinary feedback";
    options.trace_ids = {trace.trace_id};
    const auto bundle = zetta::observability::build_feedback_bundle(node, options);
    ASSERT_TRUE(bundle);
    EXPECT_TRUE(bundle->at("traces").empty());
    EXPECT_TRUE(bundle->at("recent_errors").empty());
    EXPECT_TRUE(bundle->at("crashes").empty());

    // Diagnostic opt-in is trusted internal state, never accepted from the wire.
    const auto reader = user_with_role("reader");
    const auto rejected = call_as(reader, true, "submit_feedback",
                                  {{"description", "feedback"},
                                   {"include_diagnostics", true}});
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error.code, "invalid_input");
}

// #1455: a caller without the diagnostic grant attaches its OWN traces and the
// errors in them, never another actor's trace (even one it joined or named),
// and never a crash report.
TEST_F(TraceAccessFixture, OrdinaryFeedbackAttachesOnlyTheCallersOwnTraces) {
    const auto reader = user_with_role("reader");
    // The reader's own work, in a trace of its own.
    const zetta::observability::TraceContext own = zetta::observability::make_root_trace();
    {
        ApiContext context{node, nullptr, reader, own, &node.span_recorder()};
        context.acting_is_user = true;
        registry.call("server_info", nlohmann::json::object(), context);
        // A refused call is an error span in the reader's own trace.
        registry.call("list_traces", {{"limit", 1}}, context);
    }
    ApiContext context{node, nullptr, reader, own, &node.span_recorder()};
    context.acting_is_user = true;
    const auto submitted = registry.call(
        "submit_feedback",
        {{"description", "mine only"}, {"trace_ids", {own.trace_id, trace.trace_id}}},
        context);
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    const auto bundle = stored_bundle(submitted);
    ASSERT_EQ(bundle.at("traces").size(), 1u);
    EXPECT_EQ(bundle.at("traces").at(0).at("trace_id"), own.trace_id);
    EXPECT_FALSE(bundle.at("recent_errors").empty());
    for (const auto& span : bundle.at("recent_errors")) {
        EXPECT_EQ(span.at("trace_id"), own.trace_id);
    }
    EXPECT_TRUE(bundle.at("crashes").empty());
    EXPECT_EQ(bundle.dump().find("confidential"), std::string::npos);

    // With no trace ids, the most recent traces are filtered the same way.
    const auto recent = registry.call("submit_feedback", {{"description", "recent"}}, context);
    ASSERT_TRUE(recent.ok) << recent.error.message;
    const auto recent_bundle = stored_bundle(recent);
    for (const auto& attached : recent_bundle.at("traces")) {
        EXPECT_EQ(attached.at("trace_id"), own.trace_id);
    }
    EXPECT_EQ(recent_bundle.dump().find("confidential"), std::string::npos);

    // The ownership rule itself: one foreign actor makes the trace not owned.
    zetta::observability::Span mine;
    mine.attributes = {{"actor", reader}};
    zetta::observability::Span theirs;
    theirs.attributes = {{"actor", "another-user"}};
    zetta::observability::Span unnamed;
    EXPECT_TRUE(zetta::observability::trace_is_owned_by({mine, unnamed}, reader));
    EXPECT_FALSE(zetta::observability::trace_is_owned_by({mine, theirs}, reader));
    EXPECT_FALSE(zetta::observability::trace_is_owned_by({unnamed}, reader));
}

// #1455: export_path writes a file on the node, so it needs feedback:export,
// not api:query. A reader is refused before anything is stored; admin may.
TEST_F(TraceAccessFixture, FeedbackExportNeedsItsOwnRight) {
    const auto reader = user_with_role("reader");
    const auto refused = call_as(reader, true, "submit_feedback",
                                 {{"description", "export me"},
                                  {"export_path", "reader-bundle.json"}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "unauthorized");
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "exports" / "reader-bundle.json"));

    const auto admin = user_with_role("admin");
    const auto exported = call_as(admin, true, "submit_feedback",
                                  {{"description", "export me"},
                                   {"export_path", "admin-bundle.json"}});
    ASSERT_TRUE(exported.ok) << exported.error.message;
    EXPECT_TRUE(exported.value.contains("exported_to"));
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

// #1063: a poke releases the owning loop's idle wait at once, so work queued
// outside the bridge (Node::run_on_owner) runs on the next tick instead of
// after the idle interval.
TEST(MainThreadBridge, APokeReleasesTheIdleWaitAtOnce) {
    zetta::api::MainThreadBridge bridge;
    std::thread poker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bridge.poke();
    });
    const auto started = std::chrono::steady_clock::now();
    bridge.wait_for_work(std::chrono::seconds(10));
    const auto waited = std::chrono::steady_clock::now() - started;
    poker.join();
    EXPECT_LT(waited, std::chrono::seconds(5)) << "the poke must end the idle wait";
    // The poke is consumed: the next idle wait runs its full (short) interval.
    const auto again = std::chrono::steady_clock::now();
    bridge.wait_for_work(std::chrono::milliseconds(50));
    EXPECT_GE(std::chrono::steady_clock::now() - again, std::chrono::milliseconds(40));
}

// Node::run_on_owner calls the owner wakeup when it queues work, so an idle
// loop is woken rather than left to its next timed tick (#1063).
TEST(NodeOwnerWakeup, QueuedOwnerWorkWakesTheLoop) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    node.tick();  // adopt this thread as the owner
    zetta::api::MainThreadBridge bridge;
    node.set_owner_wakeup([&bridge] { bridge.poke(); });
    std::atomic<bool> ran{false};
    std::thread worker([&] { node.run_on_owner([&] { ran = true; }); });
    const auto started = std::chrono::steady_clock::now();
    bridge.wait_for_work(std::chrono::seconds(10));  // the loop's idle wait
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5))
        << "queued owner work must wake the idle loop";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ran.load() && std::chrono::steady_clock::now() < deadline) node.tick();
    worker.join();
    EXPECT_TRUE(ran.load());
    node.set_owner_wakeup({});
    node.stop();
}

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

TEST(MainThreadBridge, CancellationWaitsForQueuedOwnerWork) {
    for (const bool cancel_before_queueing : {false, true}) {
        SCOPED_TRACE(cancel_before_queueing);
        zetta::api::MainThreadBridge bridge;
        std::atomic<bool> work_ran{false};
        std::atomic<bool> coroutine_resumed{false};
        std::atomic<bool> completed{false};
        std::stop_source cancellation;
        if (cancel_before_queueing) cancellation.request_stop();

        zetta::runtime::spawn(
            zetta::runtime::with_stop_token(
                await_bridge_work(bridge, work_ran, coroutine_resumed),
                cancellation.get_token()),
            [&] { completed.store(true); });
        const auto queued_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (bridge.pending() == 0 &&
               std::chrono::steady_clock::now() < queued_deadline) {
            std::this_thread::yield();
        }
        EXPECT_EQ(bridge.pending(), 1u);
        cancellation.request_stop();
        const auto cancelled_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (!completed.load() &&
               std::chrono::steady_clock::now() < cancelled_deadline) {
            std::this_thread::yield();
        }
        EXPECT_FALSE(coroutine_resumed.load())
            << "cancellation must not release borrowed request state";
        EXPECT_FALSE(work_ran.load());

        bridge.drain();
        const auto completed_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!completed.load() &&
               std::chrono::steady_clock::now() < completed_deadline) {
            bridge.drain();
            std::this_thread::yield();
        }
        EXPECT_TRUE(completed.load());
        EXPECT_TRUE(work_ran.load());
        EXPECT_TRUE(coroutine_resumed.load());
        EXPECT_EQ(bridge.pending(), 0u);
    }
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

    // Named relative to the node's export directory (#1399).
    const std::string export_path = "bundle-export.json";
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
    std::ifstream exported(zetta::data::export_root(node) / export_path);
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

// #1414: discovery reads use saved connections; choosing an inline destination
// requires the same authority as creating one, for both identity systems.
TEST_F(RegistryFixture, ConnectorDiscoveryRequiresApprovalForInlineDestinations) {
    struct RunnerScope {
        zetta::connect::PostgresQueryRunner previous = zetta::connect::postgres_query_runner();
        ~RunnerScope() { zetta::connect::install_postgres_query_runner(std::move(previous)); }
    } restore;
    int calls = 0;
    zetta::connect::install_postgres_query_runner([&](const auto&, const auto&, auto) {
        ++calls;
        return zetta::connect::ConnectorResult{};
    });
    const nlohmann::json connector{{"type", "postgres"},
                                   {"host", "db.workspace.svc"},
                                   {"port", "5432"},
                                   {"user", "reader"},
                                   {"token", "test"},
                                   {"database", "app"}};
    const auto config = zetta::connect::ConnectorConfig::from_json(connector);
    ASSERT_TRUE(config);
    const std::string save_error = zetta::connect::save_connection(node, "approved", *config);
    ASSERT_TRUE(save_error.empty()) << save_error;
    const auto role = node.create_role("discoverer");
    ASSERT_TRUE(role);
    auto permissions = role->permissions();
    permissions.capabilities.grant(zetta::governance::CapabilityDomain::Api, "query");
    permissions.capabilities.grant(zetta::governance::CapabilityDomain::Orchestration,
                                   "list_source_tables");
    // Using a saved connection also needs connections:use on it (#1339).
    permissions.capabilities.grant(zetta::governance::CapabilityDomain::Data,
                                   "use_connection");
    role->set_metadata(role->name(), permissions, role->created());
    const auto user = node.create_user("reader");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), node.find_role("discoverer")->id()));
    ApiContext context{node, nullptr, user->id(), trace, &node.span_recorder()};
    context.acting_is_user = true;
    auto result = registry.call("list_source_tables", {{"connector", connector}}, context);
    EXPECT_EQ(result.error.code, "unauthorized") << result.error.message;
    EXPECT_EQ(calls, 0);
    result = registry.call("list_source_tables", {{"connection", "approved"}}, context);
    EXPECT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(calls, 1);
    permissions.capabilities.grant(zetta::governance::CapabilityDomain::Orchestration,
                                   "create_connection");
    role->set_metadata(role->name(), permissions, role->created());
    result = registry.call("list_source_tables", {{"connector", connector}}, context);
    EXPECT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(calls, 2);

    const auto principal =
        node.create_principal("legacy-reader", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    zetta::governance::CapabilitySet grants;
    grants.grant(zetta::governance::CapabilityDomain::Orchestration, "list_source_tables");
    const auto key =
        principal->create_key(zetta::governance::unrestricted_scope(), std::move(grants));
    ASSERT_TRUE(key);
    context.acting_key = key->id();
    context.acting_is_user = false;
    result = registry.call("list_source_tables", {{"connector", connector}}, context);
    EXPECT_EQ(result.error.code, "unauthorized") << result.error.message;
    EXPECT_EQ(calls, 2);
    result = registry.call("list_source_tables", {{"connection", "approved"}}, context);
    EXPECT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(calls, 3);
}
