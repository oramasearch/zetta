// SPDX-License-Identifier: MIT

// The three canonical MCP journeys of #619 as DATA tables (the journey shape
// of docs/testing/mcp-journeys.md), shared verbatim by layer 1
// (test_action_journeys.cpp — in-process registry) and layer 2
// (test_mcp_server.cpp — real JSON-RPC over the loopback server). No new JSON
// format: the tables are in-code structs; the only JSON is capability input /
// output, already schema-validated at the registry boundary.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "journey.hpp"
#include "zetta/ir/execution_graph.hpp"

namespace zetta::test::journey::canonical {

// The staged fixture for gate_blocked(): a leakage-annotated churn-ish graph
// (the #309 data_leakage check refuses a feature dated after the as_of
// cutoff). Both layers create project "leaky" + graph "leaky graph" from it.
inline zetta::ir::ExecutionGraph leakage_graph() {
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

// UC-1 happy path: create project → intent → compile → gate → deploy → run →
// ranked at-risk list + drivers read back. Every mutating step is followed by
// a read-back over the same tools an agent has; the near-data question ("did
// the run train where the data lives?") is answered by one get_trace over the
// train job's trace; `actor` is the acting AccessKey id the audit read-back
// expects on every action of the loop (RULE-3).
inline Journey uc1_happy_path(const std::string& actor) {
    Journey journey{"uc1-happy-path", {}};
    journey.steps = {
        {"create the project", "create_project", {{"name", "churn-q3"}}},
        {"read the project back through list_projects", "list_projects",
         nlohmann::json::object(),
         {.checks = {contains_element("/projects", "churn-q3")}}},
        {"capture the intent", "capture_intent",
         {{"project", "churn-q3"},
          {"text",
           "Predict which customers will churn next quarter and rank the "
           "at-risk accounts with their churn drivers"}},
         {.checks = {equals("/kind", "intent"), not_empty("/input_id")}},
         {{"input_id", "/input_id"}}},
        {"read the intent back through get_project", "get_project",
         {{"name", "churn-q3"}},
         {.checks = {any_element("/inputs", {{"/id", "$input_id"}})}}},
        {"compile the intent to a pipeline named for it (#1238)", "compile_intent",
         {{"project", "churn-q3"}, {"input_id", "$input_id"}},
         {.checks = {equals("/name", "Predict which customers will churn next quarter"),
                     equals("/template", "the M-1 churn template"),
                     at_least("/steps", 6),
                     equals("/approval", "proposed")}},
         {{"graph", "/name"}}},
        {"approve the proposed plan (review-before-build, #885)", "approve_plan",
         {{"project", "churn-q3"}, {"graph", "$graph"}},
         {.checks = {equals("/approval", "approved"),
                     not_empty("/approved_at")}}},
        {"gate the compiled graph", "gate",
         {{"project", "churn-q3"}, {"graph", "$graph"}},
         {.checks = {equals("/block_deploy", false), not_empty("/checks")}},
         {{"gate_trace", "trace_id"}}},
        {"deploy the gated graph", "deploy",
         {{"project", "churn-q3"}, {"graph", "$graph"}},
         {.checks = {not_empty("/deployment_id")}}},
        {"read the placement back through system.deployments", "sql",
         {{"statement", "SELECT * FROM system.deployments;"}},
         {.checks = {size_equals("/rows", 1)}}},
        {"run the deployed graph (kind: job — the handle returns immediately)",
         "run",
         {{"project", "churn-q3"}, {"graph", "$graph"}, {"top_k", 5}},
         {.checks = {equals("/engine", "churn"), not_empty("/run_id")}},
         {{"run_id", "/run_id"}}},
        {"poll the run to its terminal status, then read the outputs",
         "get_run",
         {{"run_id", "$run_id"}},
         {.checks =
              {equals("/status", "succeeded"), not_empty("/logs"),
               any_element("/outputs", {{"/kind", "model"}}),
               any_element("/outputs", {{"/kind", "index"}}),
               predicate(
                   "an output ref carries the drivers artifact",
                   [](const nlohmann::json& outputs) {
                       for (const nlohmann::json& output : outputs) {
                           if (output.value("ref", std::string{})
                                   .starts_with("drivers:")) {
                               return true;
                           }
                       }
                       return false;
                   },
                   "/outputs")}},
         {},
         Poll{"/status", {"succeeded", "failed"}}},
        {"read the registered model back through list_models", "list_models",
         nlohmann::json::object(),
         {.checks = {any_element("/models", {{"/name", "churn_model"}})}}},
        {"the per-step compute landed as jobs", "list_jobs",
         nlohmann::json::object(), {.checks = {size_at_least("/jobs", 3)}}},
        {"find the train job through system.jobs", "sql",
         {{"statement",
           "SELECT job_id FROM system.jobs WHERE kind = 'train';"}},
         {.checks = {size_at_least("/rows", 1)}},
         {{"train_job", "/rows/0/job_id"}}},
        {"the train job carries its own trace", "get_job",
         {{"job_id", "$train_job"}},
         {.checks = {not_empty("/trace_id")}},
         {{"train_trace", "/trace_id"}}},
        {"the run went near-data: one get_trace shows the coordinated train "
         "job and its collective rounds",
         "get_trace",
         {{"trace_id", "$train_trace"}},
         {.checks = {any_element("/spans", {{"/name", "job:train"}}),
                     any_element("/spans",
                                 {{"/name", "collective:allreduce"}})}}},
        {"the api calls are spans on the caller's trace", "get_trace",
         {{"trace_id", "$gate_trace"}},
         {.checks = {any_element("/spans", {{"/name", "api:gate"}})}}},
        {"every action of the loop is audited under the acting key", "sql",
         {{"statement", "SELECT * FROM system.audit;"}},
         {.checks = {predicate(
              "an allowed audit row exists for each action under actor " +
                  actor,
              [actor](const nlohmann::json& rows) {
                  for (const char* target :
                       {"api:create_project", "api:capture_intent",
                        "api:compile_intent", "api:approve_plan", "api:gate",
                        "api:deploy", "api:run"}) {
                      bool audited = false;
                      for (const nlohmann::json& row : rows) {
                          if (row.value("target", std::string{}) == target &&
                              row.value("actor", std::string{}) == actor &&
                              row.value("allowed", false)) {
                              audited = true;
                              break;
                          }
                      }
                      if (!audited) return false;
                  }
                  return true;
              },
              "/rows")}}},
    };
    return journey;
}

// The gate-blocked negative: a leakage-annotated graph (project "leaky",
// graph "leaky graph" — staged by the fixture) is refused with the failing
// check's reason by gate, deploy, AND run; the read-backs prove nothing was
// placed and nothing executed.
inline Journey gate_blocked() {
    Journey journey{"gate-blocked-deploy", {}};
    journey.steps = {
        // Human approval (#885) precedes the machine gate — approving a plan is
        // sign-off to build, NOT a claim it is correct; the gate is what catches
        // the leak below.
        {"approve the plan so it can be verified", "approve_plan",
         {{"project", "leaky"}, {"graph", "leaky graph"}},
         {.checks = {equals("/approval", "approved")}}},
        {"the gate reports the leak and blocks deploy", "gate",
         {{"project", "leaky"}, {"graph", "leaky graph"}},
         {.checks = {equals("/block_deploy", true), not_empty("/checks")}}},
        {"deploy is refused with the failing check's reason", "deploy",
         {{"project", "leaky"}, {"graph", "leaky graph"}},
         {.ok = false,
          .error_code = "engine_error",
          .error_contains = {"gate blocked deploy", "data_leakage",
                             "spend_after_cutoff"}}},
        {"nothing was placed", "sql",
         {{"statement", "SELECT * FROM system.deployments;"}},
         {.checks = {size_equals("/rows", 0)}}},
        {"run is refused by the same gate", "run",
         {{"project", "leaky"}, {"graph", "leaky graph"}},
         {.ok = false,
          .error_code = "engine_error",
          .error_contains = {"blocked by gate"}}},
        {"nothing executed", "sql",
         {{"statement", "SELECT * FROM system.runs;"}},
         {.checks = {size_equals("/rows", 0)}}},
    };
    return journey;
}

// Dataset ingest + placement inspection: a local jsonl file becomes a
// partitioned table whose placement reads back through get_dataset AND the
// system.dataset_partitions view; the ingest lands in the audit trail; an
// unsupported format is refused with the engine's reason.
inline Journey dataset_ingest(const std::string& jsonl_path,
                              const std::string& unsupported_path) {
    Journey journey{"dataset-ingest-placement", {}};
    journey.steps = {
        {"ingest the jsonl file as a partitioned table", "add_dataset",
         {{"path", jsonl_path}, {"partition_count", 4}},
         {.checks = {equals("/rows", 24), equals("/format", "jsonl"),
                     size_equals("/partitions", 4)}},
         {{"table", "/table"}}},
        {"read the placement back through get_dataset", "get_dataset",
         {{"dataset", "$table"}},
         {.checks = {equals("/rows", 24), size_equals("/partitions", 4)}}},
        {"inspect the partition catalog through system.dataset_partitions",
         "sql",
         {{"statement", "SELECT * FROM system.dataset_partitions;"}},
         {.checks = {size_equals("/rows", 4),
                     predicate(
                         "every partition covers at least one row",
                         [](const nlohmann::json& rows) {
                             for (const nlohmann::json& row : rows) {
                                 if (row.value("row_count", 0) < 1) {
                                     return false;
                                 }
                             }
                             return true;
                         },
                         "/rows")}}},
        {"the ingest is audited as dataset_added", "sql",
         {{"statement", "SELECT * FROM system.audit;"}},
         {.checks = {predicate(
              "some audit row records dataset_added",
              [](const nlohmann::json& rows) {
                  for (const nlohmann::json& row : rows) {
                      if (row.value("action", std::string{}) ==
                          "dataset_added") {
                          return true;
                      }
                  }
                  return false;
              },
              "/rows")}}},
        {"an unsupported extension is refused with the engine's reason",
         "add_dataset",
         {{"path", unsupported_path}},
         {.ok = false, .error_code = "engine_error"}},
    };
    return journey;
}

// The #969 loop, propose → approve → run → report → introspect, as data. A
// person asks in plain language; the engine PROPOSES the run with an impact
// preview and creates nothing; approving creates exactly what the preview named
// and runs it; the report and the per-account explanation read back what the
// run produced. Step 2 is the negative read-back that makes the approval real:
// before approval there is no Project and no run. Expects a fresh node, and the
// CSV `accounts_csv` (with `account_count` rows) landed as `chat_loop.accounts`
// by the journey itself.
inline Journey chat_loop_happy_path(const std::string& accounts_csv,
                                    std::int64_t account_count) {
    Journey journey{"chat-loop-propose-approve-introspect", {}};
    journey.steps = {
        {"land the accounts as an internal dataset", "upload_dataset",
         {{"database", "chat_loop"},
          {"table_name", "accounts"},
          {"format", "csv"},
          {"content", accounts_csv}}},
        {"propose the run from a plain-language ask", "propose_run",
         {{"message",
           "Predict which customers will churn next quarter and rank the at-risk accounts"},
          {"dataset_path", "chat_loop.accounts"},
          {"holdout_fraction", 0.25},
          {"top_k", 10}},
         {.checks = {equals("/approval", "proposed"),
                     equals("/preview/creates_project", true),
                     not_empty("/preview/project"),
                     not_empty("/preview/graph"),
                     size_at_least("/preview/steps", 3),
                     equals("/preview/dataset/path", "chat_loop.accounts"),
                     equals("/decide/approve/tool", "approve_run_proposal"),
                     equals("/decide/reject/tool", "reject_run_proposal")}},
         {{"proposal_id", "/proposal_id"},
          {"project", "/preview/project"},
          {"graph", "/preview/graph"}}},
        {"read-back: the proposed Project does NOT exist before approval",
         "list_projects", nlohmann::json::object(),
         {.checks = {size_equals("/projects", 0)}}},
        {"read-back: nothing ran before approval", "sql",
         {{"statement", "SELECT * FROM system.runs;"}},
         {.checks = {size_equals("/rows", 0)}}},
        {"read the open proposal back", "get_run_proposal",
         {{"proposal_id", "$proposal_id"}},
         {.checks = {equals("/approval", "proposed"), equals("/preview/project", "$project")}}},
        {"approve it: exactly the previewed Project and Pipeline, then the run",
         "approve_run_proposal", {{"proposal_id", "$proposal_id"}},
         {.checks = {equals("/approval", "approved"),
                     equals("/project", "$project"),
                     equals("/graph", "$graph"),
                     not_empty("/run/run_id"),
                     not_empty("/decided_by")}},
         {{"run_id", "/run/run_id"}}},
        {"poll the run to its terminal status; it emits a report", "get_run",
         {{"run_id", "$run_id"}},
         {.checks = {equals("/status", "succeeded"),
                     any_element("/outputs", {{"/kind", "model"}}),
                     any_element("/outputs", {{"/kind", "report"}})}},
         {},
         Poll{"/status", {"succeeded", "failed"}}},
        {"read-back: system.runs agrees", "sql",
         {{"statement", "SELECT * FROM system.runs;"}},
         {.checks = {size_equals("/rows", 1)}}},
        {"read-back: the previewed Project now exists", "list_projects",
         nlohmann::json::object(),
         {.checks = {contains_element("/projects", "$project")}}},
        {"read-back: the proposal records the run it started", "get_run_proposal",
         {{"proposal_id", "$proposal_id"}},
         {.checks = {equals("/approval", "approved"), equals("/run_id", "$run_id")}}},
        {"approving the same proposal twice is refused and creates nothing",
         "approve_run_proposal", {{"proposal_id", "$proposal_id"}},
         {.ok = false,
          .error_code = "conflict",
          .error_contains = {"already approved", "nothing was created again"},
          .error_path = "/proposal_id"}},
        {"read-back: still one run", "sql",
         {{"statement", "SELECT * FROM system.runs;"}},
         {.checks = {size_equals("/rows", 1)}}},
        {"the report: ranked accounts, the landed dataset, and a held-out figure or "
         "the reason there is none",
         "get_run_report", {{"run_id", "$run_id"}},
         {.checks =
              {not_empty("/at_risk"),
               equals("/dataset/accounts", account_count),
               predicate(
                   "model.held_out_auc is present, or `missing` names held_out_auc with a "
                   "reason",
                   [](const nlohmann::json& report) {
                       const nlohmann::json model =
                           report.value("model", nlohmann::json::object());
                       if (model.contains("held_out_auc") && model.at("held_out_auc").is_number()) {
                           return true;
                       }
                       for (const nlohmann::json& item :
                            report.value("missing", nlohmann::json::array())) {
                           if (item.value("field", std::string{}).find("held_out_auc") !=
                                   std::string::npos &&
                               !item.value("why", std::string{}).empty()) {
                               return true;
                           }
                       }
                       return false;
                   })}},
         {{"model", "/model/id"}, {"top_entity", "/at_risk/0/entity"}}},
        {"explain the top-ranked account: the contributions add up to its score, "
         "strongest first",
         "explain",
         {{"model", "$model"}, {"dataset_path", "chat_loop.accounts"}, {"entity", "$top_entity"}},
         {.checks = {equals("/entity", "$top_entity"),
                     not_empty("/contributions"),
                     predicate("|residual| is about 0",
                               [](const nlohmann::json& residual) {
                                   return residual.is_number() &&
                                          std::abs(residual.get<double>()) < 1e-6;
                               },
                               "/residual"),
                     predicate("contributions are ordered by |contribution|, largest first",
                               [](const nlohmann::json& contributions) {
                                   double previous = std::numeric_limits<double>::infinity();
                                   for (const nlohmann::json& item : contributions) {
                                       const double size =
                                           std::abs(item.value("contribution", 0.0));
                                       if (size > previous + 1e-12) return false;
                                       previous = size;
                                   }
                                   return true;
                               },
                               "/contributions")}}},
    };
    return journey;
}

// The #969 loop's refusals, each asserting the engine's reason rather than bare
// failure: approving a proposal that was never made, approving a rejected one
// (and nothing is created), introspecting a run id that does not exist, and
// asking for a report from a run that produced none (`bare_run_id`, a run the
// fixture created with no model). Expects a fresh node.
inline Journey chat_loop_refusals(const std::string& bare_run_id) {
    Journey journey{"chat-loop-refusals", {}};
    journey.steps = {
        {"approving a proposal that was never made is refused", "approve_run_proposal",
         {{"proposal_id", "never-proposed"}},
         {.ok = false,
          .error_code = "not_found",
          .error_contains = {"no run proposal 'never-proposed'", "nothing was proposed"},
          .error_path = "/proposal_id"}},
        {"a proposal naming a dataset the node does not hold is refused", "propose_run",
         {{"message", "Predict which customers will churn"},
          {"dataset_path", "nowhere.accounts"}},
         {.ok = false,
          .error_code = "invalid_input",
          .error_contains = {"no dataset 'nowhere.accounts'"},
          .error_path = "/dataset_path"}},
        {"propose a run", "propose_run",
         {{"message", "Predict which customers will churn"}},
         {.checks = {equals("/approval", "proposed"),
                     equals("/preview/dataset/description", "the built-in demo dataset")}},
         {{"proposal_id", "/proposal_id"}}},
        {"reject it (the cancel path)", "reject_run_proposal",
         {{"proposal_id", "$proposal_id"}, {"reason", "not this quarter"}},
         {.checks = {equals("/approval", "rejected"),
                     equals("/rejection_reason", "not this quarter")}}},
        {"approving a rejected proposal is refused", "approve_run_proposal",
         {{"proposal_id", "$proposal_id"}},
         {.ok = false,
          .error_code = "conflict",
          .error_contains = {"was rejected", "propose the run again"},
          .error_path = "/proposal_id"}},
        {"read-back: a rejected proposal created no Project", "list_projects",
         nlohmann::json::object(),
         {.checks = {size_equals("/projects", 0)}}},
        {"read-back: and ran nothing (the one row is the fixture's bare run)", "sql", {{"statement", "SELECT * FROM system.runs;"}},
         {.checks = {size_equals("/rows", 1)}}},
        {"introspecting a run id that does not exist is refused", "get_run_report",
         {{"run_id", "no-such-run"}},
         {.ok = false, .error_code = "not_found", .error_contains = {"no-such-run"}}},
        {"a report from a run that produced none says why", "get_run_report",
         {{"run_id", bare_run_id}},
         {.ok = false, .error_code = "not_found", .error_contains = {"model"}}},
    };
    return journey;
}

}  // namespace zetta::test::journey::canonical
