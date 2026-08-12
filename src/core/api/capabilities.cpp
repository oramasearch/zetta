#include "zetta/api/capabilities.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "zetta/api/background.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/ml/explain_row.hpp"
#include "zetta/ml/score_table.hpp"
#include "zetta/data/promote_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/agent/agent_store.hpp"
#include "zetta/agent/chat.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/tokenizer/bpe.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/feedback.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/pipeline/n8n_compile.hpp"
#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/project_input.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/query/service.hpp"
#include "zetta/testkit/async_case_run.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/version.hpp"

namespace zetta::api {

namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json empty_object_schema() {
    return nlohmann::json{{"type", "object"}, {"additionalProperties", false}};
}

// Output schemas (#618) are draft-07 like the input schemas (schema-as-code):
// each documents its handler's success value — rendered as the MCP tool's
// outputSchema and as the REST/OpenAPI 200 response body's `value`. The
// handler stays the source of the value; journeys assert the two agree.

// The typed detail reads return exactly the entity's already-validated value
// JSON (its embedded *_value_schema()) plus the id — compose the output
// schema from the SAME embedded schema so the two can never drift.
nlohmann::json entity_value_with_id_schema(nlohmann::json schema) {
    schema["properties"]["id"] = {{"type", "string"},
                                  {"description", "the entity's uuid"}};
    nlohmann::json required = schema.value("required", nlohmann::json::array());
    required.push_back("id");
    schema["required"] = std::move(required);
    return schema;
}

// --- server_info -------------------------------------------------------------

const nlohmann::json& server_info_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& server_info_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"version", "node_id", "cluster_members", "databases", "projects",
          "runs", "models", "jobs"}},
        {"additionalProperties", false},
        {"properties",
         {{"version", {{"type", "string"}}},
          {"node_id", {{"type", "string"}}},
          {"cluster_members", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"databases", {{"type", "integer"}}},
          {"projects", {{"type", "integer"}}},
          {"runs", {{"type", "integer"}}},
          {"models", {{"type", "integer"}}},
          {"jobs", {{"type", "integer"}}}}}};
    return schema;
}

ApiResult handle_server_info(const nlohmann::json&, ApiContext& context) {
    nlohmann::json output;
    output["version"] = zetta::kVersion;
    output["node_id"] = context.node.node_id();
    output["cluster_members"] = context.node.cluster_members();
    output["databases"] = context.node.database_count();
    output["projects"] = context.node.project_count();
    output["runs"] = context.node.runs().size();
    output["models"] = context.node.models().size();
    output["jobs"] = context.node.jobs().size();
    return ApiResult::success(std::move(output));
}

// --- versions ----------------------------------------------------------------

// "Which version am I actually talking to" — asked of the SOFTWARE and of the
// MODEL, and the two answers come from different places. Four checkpoints were
// served side by side for comparison once and the Ask Orama rail answered from
// the oldest; the replies read as a model regression for an hour before anyone
// suspected the wrong agent was answering. `server_info` reports the binary and
// nothing about the agents, so it could not have settled that. This can.
//
// `assist_agent` is the one the rail picks, and it is deliberately reported the
// same way the rail picks it (first served) rather than described, so this
// cannot drift away from the truth it exists to report — see #981 for making
// that choice explicit rather than positional.

const nlohmann::json& versions_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& versions_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"software", "agents"}},
        {"additionalProperties", false},
        {"properties",
         {{"software",
           {{"type", "object"},
            {"required", {"zetta", "node_id"}},
            {"properties",
             {{"zetta", {{"type", "string"}}}, {"node_id", {{"type", "string"}}}}}}},
          {"agents",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "name", "model", "checkpoint"}},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"model", {{"type", "string"}}},
                {"checkpoint", {{"type", "string"}}}}}}}}},
          {"assist_agent", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_versions(const nlohmann::json&, ApiContext& context) {
    nlohmann::json output;
    output["software"] = {{"zetta", zetta::kVersion}, {"node_id", context.node.node_id()}};

    nlohmann::json agents = nlohmann::json::array();
    std::string assist;
    if (context.node.replication() != nullptr) {
        zetta::agent::AgentStore store(*context.node.replication());
        for (const zetta::agent::AgentRecord& agent : store.list()) {
            // The checkpoint is the last path element of `model` — that is what
            // distinguishes v12 from v13 when both are named "Base agent".
            std::string checkpoint = agent.model;
            if (const std::size_t slash = checkpoint.find_last_of('/');
                slash != std::string::npos && slash + 1 < checkpoint.size()) {
                checkpoint = checkpoint.substr(slash + 1);
            }
            agents.push_back({{"id", agent.id},
                              {"name", agent.name},
                              {"model", agent.model},
                              {"checkpoint", std::move(checkpoint)}});
            if (assist.empty()) assist = agent.id;
        }
    }
    output["agents"] = std::move(agents);
    if (!assist.empty()) output["assist_agent"] = assist;
    return ApiResult::success(std::move(output));
}

// --- sql ---------------------------------------------------------------------

const nlohmann::json& sql_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"statement"}},
        {"additionalProperties", false},
        {"properties",
         {{"statement",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "one SQL statement (Postgres subset; the system.* "
                            "views are all readable here)"}}},
          {"database",
           {{"type", "string"},
            {"description",
             "default database for unqualified table names (optional)"}}}}}};
    return schema;
}

const nlohmann::json& sql_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"messages", "columns", "rows"}},
        {"additionalProperties", false},
        {"properties",
         {{"messages", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"rows",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description", "one result row keyed by column name"}}}}}}}};
    return schema;
}

ApiResult handle_sql(const nlohmann::json& input, ApiContext& context) {
    // Thread the gate-resolved identity (#745) into the engine so role-management
    // DDL is gated on roles:manage (#746). Read SQL still only needs the sql
    // capability's required op (mcp:call); the finer gate is per-statement inside.
    zetta::query::QueryService service(context.node, context.cluster,
                                       context.acting_key, context.acting_is_user);
    const std::string database = input.value("database", "");
    auto outcome = service.run(input.at("statement").get<std::string>(), database);
    if (!outcome.ok()) {
        return ApiResult::failure("engine_error", outcome.error);
    }
    nlohmann::json output;
    output["messages"] = outcome.messages;
    output["columns"] = outcome.columns;
    output["rows"] = outcome.rows;
    return ApiResult::success(std::move(output));
}

// --- search --------------------------------------------------------------------

const nlohmann::json& search_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"target", "query"}},
        {"additionalProperties", false},
        {"properties",
         {{"target",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "an index id, or a table name with a Search index over it"}}},
          {"query",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the free-text query"}}},
          {"field",
           {{"type", "string"},
            {"description",
             "restrict matching to one indexed field (optional)"}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 1000},
            {"description", "cap on the global ranking (default 10)"}}}}}};
    return schema;
}

// The search success value's shape is declared ONCE, beside the served
// route's rendering (serving.hpp, #648): search_response_schema() /
// search_result_to_json() are shared by this capability and serve_search.

// The thin caller of THE search entry point (Node::search, #641) — the same
// path the GUI drives, so MCP/REST and the GUI can never diverge.
ApiResult handle_search(const nlohmann::json& input, ApiContext& context) {
    zetta::index::SearchRequest request;
    request.target = input.at("target").get<std::string>();
    request.query = input.at("query").get<std::string>();
    request.field = input.value("field", std::string{});
    request.top_k = input.value("top_k", 10);

    const zetta::index::SearchResult result =
        context.node.search(request, context.recorder, &context.trace);
    if (!result.ok()) {
        return ApiResult::failure("engine_error", result.status.message);
    }
    return ApiResult::success(search_result_to_json(result));
}

// --- list_projects -----------------------------------------------------------

const nlohmann::json& list_projects_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_projects_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"projects"}},
        {"additionalProperties", false},
        {"properties",
         {{"projects", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}};
    return schema;
}

ApiResult handle_list_projects(const nlohmann::json&, ApiContext& context) {
    nlohmann::json output;
    output["projects"] = context.node.project_names();
    return ApiResult::success(std::move(output));
}

// --- typed detail reads (#616 read parity) -----------------------------------
//
// Each get_* returns the entity's already-validated value JSON — the exact
// shape `metadata()` serializes (= the CRDT value the boundary schema
// validated) — with the id added, so MCP, the on-disk projection, and the
// system.* SQL views can never drift (RULE-7: same live engine source).

nlohmann::json id_input_schema(std::string_view field,
                               std::string_view description) {
    return nlohmann::json{
        {"type", "object"},
        {"required", {field}},
        {"additionalProperties", false},
        {"properties",
         {{field, {{"type", "string"},
                   {"minLength", 1},
                   {"description", description}}}}}};
}

const nlohmann::json& get_project_schema() {
    static const nlohmann::json schema =
        id_input_schema("name", "the project's name, or its uuid");
    return schema;
}

const nlohmann::json& get_project_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"id", "name", "created", "updated", "owner", "description", "inputs", "graphs"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"created", {{"type", "string"}}},
          {"updated", {{"type", "string"}}},
          {"owner", {{"type", "string"}}},
          {"description", {{"type", "string"}}},
          {"inputs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description",
               "one project input's validated metadata plus its id"},
              {"required", {"id", "kind"}},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"kind", {{"type", "string"}}}}}}}}},
          {"graphs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "name"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"approval",
                 {{"type", "string"}, {"enum", {"proposed", "approved"}},
                  {"description", "plan-approval state (#885)"}}},
                {"approved_by", {{"type", "string"}}},
                {"approved_at", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_get_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    auto project = context.node.find_project(name);
    if (!project) {
        // Entity refs elsewhere (runs, jobs, models) carry the project *uuid*,
        // so accept it as the handle too.
        for (const std::string& candidate : context.node.project_names()) {
            auto by_name = context.node.find_project(candidate);
            if (by_name && by_name->id() == name) {
                project = std::move(by_name);
                break;
            }
        }
    }
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    nlohmann::json output = project->metadata();
    output["id"] = project->id();
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto& project_input : project->inputs()) {
        nlohmann::json entry = project_input->metadata();
        entry["id"] = project_input->id();
        inputs.push_back(std::move(entry));
    }
    output["inputs"] = std::move(inputs);
    nlohmann::json graphs = nlohmann::json::array();
    for (const auto& graph : project->graphs()) {
        graphs.push_back({{"id", graph->id()},
                          {"name", graph->name()},
                          {"approval", graph->approval_status()},
                          {"approved_by", graph->approved_by()},
                          {"approved_at", graph->approved_at()}});
    }
    output["graphs"] = std::move(graphs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_run_schema() {
    static const nlohmann::json schema =
        id_input_schema("run_id", "the run's uuid (system.runs.run_id)");
    return schema;
}

const nlohmann::json& get_run_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::pipeline::run_value_schema());
    return schema;
}

ApiResult handle_get_run(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("run_id").get<std::string>();
    const auto run = context.node.find_run(id);
    if (!run) {
        return ApiResult::failure("not_found", "no run with id '" + id + "'");
    }
    nlohmann::json output = run->metadata();
    output["id"] = run->id();
    return ApiResult::success(std::move(output));
}

// ---------------------------------------------------------------------------
// get_run_report (#958) — the facts a run FOUND, assembled at query time.
//
// The artifact a report is written from. Deliberately NOT a stored document: every
// number here already lives in replicated state — the Model record carries the
// metrics and what the model was trained from, the at-risk table carries the
// ranked list with its evidence — so assembling it on read means there is no
// second copy to drift from the first, and a report generated a month later
// reflects the same facts as one generated at the end of the run.
//
// The rule this serves: **the engine computes every number and the narrator only
// arranges them.** A field a writer has to work out is a field this artifact is
// missing. `majority_class_accuracy` is here because exactly that happened once.
// ---------------------------------------------------------------------------

const nlohmann::json& get_run_report_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the run's uuid (system.runs.run_id, or the `report` output's ref)"}}},
          {"limit",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "how many at-risk entities to return, highest risk first (default 20). "
             "The whole population is ranked; this caps what comes back"}}}}}};
    return schema;
}

const nlohmann::json& get_run_report_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "model"}},
        {"properties",
         {{"run", {{"type", "object"},
                   {"description", "run id, graph, status, and when it finished"}}},
          {"dataset",
           {{"type", "object"},
            {"description",
             "what the model was built from: accounts, events, churn_rate, "
             "majority_class_accuracy (the accuracy a constant answer scores), and "
             "the feature columns. Absent when the model records no dataset facts"}}},
          {"model",
           {{"type", "object"},
            {"description",
             "id, name, and the metrics. `held_out_*` is present only when the run "
             "asked for a holdout; when it is absent the only figures available are "
             "in-sample and they flatter (#953)"}}},
          {"drivers",
           {{"type", "array"},
            {"description", "feature -> weight, strongest first. A weight is what "
                            "the model leaned on, not a cause"}}},
          {"at_risk",
           {{"type", "array"},
            {"description",
             "the ranked entities with their risk and their evidence values, "
             "capped by `limit`"}}},
          {"caveats",
           {{"type", "array"},
            {"description",
             "figures that are PRESENT but must not be read as results — a holdout "
             "too small to mean anything, a perfect score. `missing` says what the "
             "run cannot tell you; this says what it can and you should not believe"}}},
          {"missing",
           {{"type", "array"},
            {"description",
             "fields a report would want that this run cannot supply, each saying "
             "why. Stated rather than omitted so a writer does not quietly "
             "substitute its own number"}}}}}};
    return schema;
}

// The at-risk table this run wrote, by the run's own `dataset` outputs. Named
// `churn_results.at_risk_*`, which is what run_act materializes.
std::shared_ptr<zetta::data::Table> run_at_risk_table(
    Node& node, const zetta::pipeline::Run& run) {
    for (const zetta::pipeline::RunOutput& output : run.outputs()) {
        const std::size_t dot = output.ref.find('.');
        if (output.kind != "dataset" || dot == std::string::npos) continue;
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(output.ref.substr(0, dot));
        if (!database) continue;
        if (auto table = database->find_table(output.ref.substr(dot + 1))) {
            return table;
        }
    }
    return nullptr;
}

// A row count as a plain integer string — "4", not "4.000000".
std::string fixed_rows(double value) {
    return std::to_string(static_cast<long long>(value));
}

ApiResult handle_get_run_report(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("run_id").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Run> run = context.node.find_run(id);
    if (!run) {
        return ApiResult::failure("not_found", "no run with id '" + id + "'");
    }
    const std::size_t limit = input.value("limit", std::size_t{20});

    nlohmann::json report;
    nlohmann::json missing = nlohmann::json::array();
    report["run"] = {{"id", run->id()},
                     {"graph", run->graph()},
                     {"project", run->project()},
                     {"status", to_string(run->status())},
                     {"finished", run->updated()}};

    // The model output names the Model this run trained; everything numeric hangs
    // off it.
    std::shared_ptr<zetta::ml::Model> model;
    for (const zetta::pipeline::RunOutput& output : run->outputs()) {
        if (output.kind == "model") model = context.node.find_model(output.ref);
        if (model) break;
    }
    if (!model) {
        return ApiResult::failure(
            "not_found",
            "run '" + id +
                "' has no resolvable model output, so there are no measured "
                "numbers to report");
    }

    const nlohmann::json& metrics = model->metrics();
    const auto number = [&metrics](const char* key) -> std::optional<double> {
        const auto found = metrics.find(key);
        if (found == metrics.end() || !found->is_number()) return std::nullopt;
        return found->get<double>();
    };

    nlohmann::json model_facts = {{"id", model->id()}, {"name", model->name()}};
    if (const auto held = number("held_out_auc")) {
        model_facts["held_out_auc"] = *held;
        if (const auto value = number("held_out_log_loss")) model_facts["held_out_log_loss"] = *value;
        if (const auto value = number("held_out_accuracy")) model_facts["held_out_accuracy"] = *value;
        if (const auto value = number("held_out_rows")) model_facts["held_out_rows"] = *value;
    } else {
        // Said out loud rather than left blank. A report that leads with a
        // held-out figure has to know when there isn't one, or it leads with the
        // in-sample number instead and calls it quality (#953).
        missing.push_back(
            {{"field", "held_out_auc"},
             {"why", "this run set no holdout_fraction, so nothing was measured on "
                     "rows the model had not seen. Every figure below is in-sample "
                     "and flatters. Re-run with holdout_fraction to get one"}});
    }
    if (const auto value = number("in_sample_auc")) model_facts["in_sample_auc"] = *value;
    else if (const auto legacy = number("auc")) model_facts["in_sample_auc"] = *legacy;
    if (const auto value = number("in_sample_log_loss")) model_facts["in_sample_log_loss"] = *value;
    else if (const auto legacy = number("log_loss")) model_facts["in_sample_log_loss"] = *legacy;
    if (const auto value = number("in_sample_accuracy")) model_facts["in_sample_accuracy"] = *value;
    else if (const auto legacy = number("accuracy")) model_facts["in_sample_accuracy"] = *legacy;
    if (const auto value = number("in_sample_rows")) model_facts["train_rows"] = *value;
    report["model"] = std::move(model_facts);

    nlohmann::json dataset = nlohmann::json::object();
    if (const auto value = number("dataset_accounts")) dataset["accounts"] = *value;
    if (const auto value = number("dataset_events")) dataset["events"] = *value;
    if (const auto value = number("dataset_churn_rate")) dataset["churn_rate"] = *value;
    if (const auto value = number("dataset_majority_class_accuracy")) {
        dataset["majority_class_accuracy"] = *value;
    }
    nlohmann::json features = nlohmann::json::array();
    nlohmann::json drivers = nlohmann::json::array();
    for (const zetta::ml::ModelDriver& driver : model->drivers()) {
        drivers.push_back({{"feature", driver.feature}, {"weight", driver.importance}});
        features.push_back(driver.feature);
    }
    if (!features.empty()) dataset["features"] = std::move(features);
    if (!dataset.empty()) report["dataset"] = std::move(dataset);
    else {
        missing.push_back({{"field", "dataset"},
                           {"why", "this run's model records no dataset facts; it "
                                   "predates them being written (#958)"}});
    }
    if (!drivers.empty()) report["drivers"] = std::move(drivers);

    // The ranked list, read back from the table the run materialized — so the
    // report and anything querying `churn_results` cannot disagree.
    if (const std::shared_ptr<zetta::data::Table> table =
            run_at_risk_table(context.node, *run)) {
        std::vector<nlohmann::json> rows;
        const zetta::storage::StorageEngine& store = table->store();
        for (std::size_t index = 0; index < store.size(); ++index) {
            if (const auto row = store.at(index)) rows.push_back(*row);
        }
        std::sort(rows.begin(), rows.end(),
                  [](const nlohmann::json& left, const nlohmann::json& right) {
                      return left.value("rank", 0.0) < right.value("rank", 0.0);
                  });
        nlohmann::json at_risk = nlohmann::json::array();
        for (const nlohmann::json& row : rows) {
            if (at_risk.size() >= limit) break;
            nlohmann::json entry = {{"rank", row.value("rank", 0)},
                                    {"entity", row.value("account", 0)},
                                    {"risk", row.value("risk", 0.0)}};
            // Evidence is whatever columns beyond the fixed ones the run wrote —
            // the population's strongest drivers, the same columns on every row.
            nlohmann::json evidence = nlohmann::json::object();
            for (const auto& [key, value] : row.items()) {
                if (key == "rank" || key == "account" || key == "risk" ||
                    key == "dominant_driver" || key == "driver_effect" || key == "id") {
                    continue;
                }
                evidence[key] = value;
            }
            if (!evidence.empty()) entry["evidence"] = std::move(evidence);
            at_risk.push_back(std::move(entry));
        }
        report["at_risk"] = std::move(at_risk);
    } else {
        missing.push_back({{"field", "at_risk"},
                           {"why", "this run materialized no at-risk table"}});
    }

    // Figures that are PRESENT but must not be read as results (#948 step 8's rule,
    // applied to the report itself). `missing` says what the run cannot tell you;
    // this says what it can tell you and you should not believe.
    //
    // The case that forced it: the built-in demo dataset produces a held-out AUC of
    // 1.000 on FOUR rows. Nothing was wrong — the pipeline worked and the data is
    // trivially separable — but a perfect score presented without objection is a
    // number that invites a false conclusion, which is worse than a missing one.
    nlohmann::json caveats = nlohmann::json::array();
    if (const std::optional<double> rows = number("held_out_rows")) {
        // Below this, a ranking metric is noise: one row changing side moves AUC by
        // a visible amount, so the figure says more about the split than the model.
        constexpr double kMeaningfulHoldout = 30.0;
        if (*rows > 0.0 && *rows < kMeaningfulHoldout) {
            caveats.push_back(
                {{"field", "held_out_auc"},
                 {"why", "measured on only " + fixed_rows(*rows) +
                             " held-out rows. Below about 30 a ranking metric is "
                             "noise — one row changing side moves it visibly — so "
                             "this figure describes the split more than the model. "
                             "Do not quote it as a result"}});
        }
    }
    if (const std::optional<double> auc = number("held_out_auc")) {
        if (*auc >= 0.999) {
            caveats.push_back(
                {{"field", "held_out_auc"},
                 {"why", "a perfect score. On real data this is almost always a "
                         "trivially separable demo set or a leaked feature, not a "
                         "model that solved the problem. Check what the top driver "
                         "is before showing this to anyone"}});
        }
    }
    if (!caveats.empty()) report["caveats"] = std::move(caveats);
    if (!missing.empty()) report["missing"] = std::move(missing);
    return ApiResult::success(std::move(report));
}

const nlohmann::json& get_model_schema() {
    static const nlohmann::json schema =
        id_input_schema("model_id", "the model's uuid (system.models.model_id)");
    return schema;
}

const nlohmann::json& get_model_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::ml::model_value_schema());
    return schema;
}

ApiResult handle_get_model(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) {
        return ApiResult::failure("not_found", "no model with id '" + id + "'");
    }
    nlohmann::json output = model->metadata();
    output["id"] = model->id();
    return ApiResult::success(std::move(output));
}

// --- model_health (Monitor, #983) -------------------------------------------------------------
//
// The first Monitor capability. The agent is meant to watch a deployed model and say whether it
// can still be trusted, and until now it had no tool that answers that — so it could only
// describe monitoring, which is #976 at the methodology level.
//
// Deliberately computed from what a Model record ALREADY carries. A drift capability needing a
// stored reference distribution is a schema change and a migration; this needs neither, and the
// question it answers is the one asked first: **is the number I was given honest, and is it still
// the number?**
const nlohmann::json& model_health_schema() {
    static const nlohmann::json schema =
        id_input_schema("model_id", "the model's uuid (system.models.model_id)");
    return schema;
}

const nlohmann::json& model_health_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "trustworthy", "findings"}},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"trustworthy", {{"type", "boolean"}}},
          {"held_out_auc", {{"type", "number"}}},
          {"in_sample_auc", {{"type", "number"}}},
          {"overfit_gap", {{"type", "number"}}},
          {"calibrated", {{"type", "boolean"}}},
          {"findings", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}};
    return schema;
}

ApiResult handle_model_health(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) return ApiResult::failure("not_found", "no model with id '" + id + "'");

    const nlohmann::json metadata = model->metadata();
    const nlohmann::json metrics =
        metadata.contains("metrics") ? metadata.at("metrics") : nlohmann::json::object();
    const auto number = [&metrics](const char* key) -> std::optional<double> {
        const auto entry = metrics.find(key);
        if (entry == metrics.end() || !entry->is_number()) return std::nullopt;
        return entry->get<double>();
    };

    nlohmann::json output;
    output["model_id"] = id;
    std::vector<std::string> findings;
    bool trustworthy = true;

    const std::optional<double> held_out = number("held_out_auc");
    const std::optional<double> in_sample = number("in_sample_auc");
    if (held_out) output["held_out_auc"] = *held_out;
    if (in_sample) output["in_sample_auc"] = *in_sample;

    if (!held_out) {
        // The single most important thing this can say. Without a holdout the only figure is
        // measured on the rows the model was fit on, and it flatters — an in-sample 0.868 reached
        // a client report once where the held-out figure was 0.726.
        trustworthy = false;
        findings.push_back(
            "No held-out metric. This model was trained without a holdout, so every quality "
            "figure it carries was measured on the rows it learned from and cannot be quoted. "
            "Retrain with holdout_fraction set.");
    } else if (in_sample) {
        const double gap = *in_sample - *held_out;
        output["overfit_gap"] = gap;
        if (gap > 0.1) {
            trustworthy = false;
            findings.push_back(
                "In-sample exceeds held-out by " + std::to_string(gap) +
                ". The model has memorized rather than generalized; the held-out figure is the "
                "only one that describes new accounts.");
        }
    }

    const auto calibrated = metadata.find("calibrated");
    const bool is_calibrated =
        calibrated != metadata.end() && calibrated->is_boolean() && calibrated->get<bool>();
    output["calibrated"] = is_calibrated;
    if (!is_calibrated) {
        // Not disqualifying — ranking is a legitimate use — but a threshold set on an
        // uncalibrated score means something different from what the person setting it expects.
        findings.push_back(
            "Scores are not calibrated: they rank accounts but are not probabilities, so a "
            "threshold like 0.8 does not mean an 80% chance.");
    }

    output["trustworthy"] = trustworthy;
    output["findings"] = findings;
    return ApiResult::success(std::move(output));
}

// --- propose_improvement (Improve, #983) ------------------------------------------------------
//
// The Improve verb, and the one property that makes it gradeable: a proposal MUST carry its
// predicted effect. Kernel D7-2 — the metric the customer pays for is the metric the agent learns
// on — so "retrain it" with no predicted effect is not a proposal, it is a suggestion, and
// nothing can score it or hold it to account afterwards.
//
// It refuses rather than guesses. A proposal whose effect cannot be predicted from what the model
// actually carries returns `predictable: false` with the reason, because an invented number here
// would be worse than silence: it would be a number the customer plans against.
const nlohmann::json& propose_improvement_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "change"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}, {"minLength", 1}}},
          {"change",
           {{"type", "string"},
            {"enum", {"add_holdout", "calibrate", "retrain"}},
            {"description", "the change being proposed"}}}}}};
    return schema;
}

const nlohmann::json& propose_improvement_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "change", "predictable", "rationale"}},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"change", {{"type", "string"}}},
          {"predictable", {{"type", "boolean"}}},
          {"predicted_effect", {{"type", "string"}}},
          {"rationale", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_propose_improvement(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const std::string change = input.at("change").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) return ApiResult::failure("not_found", "no model with id '" + id + "'");

    const nlohmann::json metadata = model->metadata();
    const nlohmann::json metrics =
        metadata.contains("metrics") ? metadata.at("metrics") : nlohmann::json::object();
    const auto number = [&metrics](const char* key) -> std::optional<double> {
        const auto entry = metrics.find(key);
        if (entry == metrics.end() || !entry->is_number()) return std::nullopt;
        return entry->get<double>();
    };

    nlohmann::json output;
    output["model_id"] = id;
    output["change"] = change;

    const std::optional<double> held_out = number("held_out_auc");
    const std::optional<double> in_sample = number("in_sample_auc");

    if (change == "add_holdout") {
        if (held_out) {
            output["predictable"] = false;
            output["rationale"] =
                "This model already has a held-out figure, so adding a holdout changes nothing.";
        } else if (in_sample) {
            // The one effect that IS predictable, because it is not a guess about the model — it
            // is a statement about what the current number means.
            output["predictable"] = true;
            output["predicted_effect"] =
                "The reported quality will FALL, and that fall is the point: the current figure "
                "of " + std::to_string(*in_sample) +
                " was measured on the rows the model was fit on. A held-out figure describes "
                "accounts it has never seen, and it is the only one that can be quoted.";
            output["rationale"] =
                "Retrain with holdout_fraction set. Expect a lower number and a truthful one.";
        } else {
            output["predictable"] = false;
            output["rationale"] =
                "This model carries no quality figure at all, so there is nothing to compare a "
                "holdout against.";
        }
    } else if (change == "calibrate") {
        output["predictable"] = false;
        output["rationale"] =
            "Calibration changes what the scores MEAN, not how well they rank, so the ranking "
            "metric will not move. Whether the resulting probabilities are honest can only be "
            "measured after fitting the calibrator on held-out rows.";
    } else {
        // "retrain" with no stated reason. Refusing here is the point: an agent that predicts an
        // improvement from a retrain it has no evidence for is the failure this capability
        // exists to prevent.
        output["predictable"] = false;
        output["rationale"] =
            "A retrain with no change to the data or the pipeline has no predictable effect. Say "
            "what would differ - more history, a new feature, a different cutoff - and the effect "
            "of THAT can be reasoned about.";
    }
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_job_schema() {
    static const nlohmann::json schema =
        id_input_schema("job_id", "the compute job's uuid (system.jobs.job_id)");
    return schema;
}

const nlohmann::json& get_job_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::compute::compute_job_value_schema());
    return schema;
}

ApiResult handle_get_job(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("job_id").get<std::string>();
    const auto job = context.node.find_job(id);
    if (!job) {
        return ApiResult::failure("not_found", "no job with id '" + id + "'");
    }
    nlohmann::json output = job->metadata();
    output["id"] = job->id();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_index_schema() {
    static const nlohmann::json schema =
        id_input_schema("index_id", "the index's uuid (system.indexes.index_id)");
    return schema;
}

const nlohmann::json& get_index_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::index::index_value_schema());
    return schema;
}

ApiResult handle_get_index(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("index_id").get<std::string>();
    const auto index = context.node.find_index(id);
    if (!index) {
        return ApiResult::failure("not_found", "no index with id '" + id + "'");
    }
    nlohmann::json output = index->metadata();
    output["id"] = index->id();
    return ApiResult::success(std::move(output));
}

// --- get_dataset / list_datasets ----------------------------------------------

// A DatasetSummary as JSON — the full computed shape, with the per-partition
// placement kept structured (the SQL view flattens it to strings for tabular
// reading; the tool keeps the arrays an agent can walk).
nlohmann::json dataset_summary_json(const zetta::data::DatasetSummary& dataset) {
    nlohmann::json partitions = nlohmann::json::array();
    for (const zetta::data::DatasetPlacement& placement : dataset.partitions) {
        partitions.push_back({{"partition_id", placement.partition_id},
                              {"blob_ref", placement.blob_ref},
                              {"row_start", placement.row_start},
                              {"row_end", placement.row_end},
                              {"owner_nodes", placement.owner_nodes}});
    }
    return nlohmann::json{{"name", dataset.name},
                          {"database", dataset.database},
                          {"table_id", dataset.table_id},
                          {"format", dataset.format},
                          {"replication", dataset.replication},
                          {"rows", dataset.rows},
                          {"size_bytes", dataset.size_bytes},
                          {"columns", dataset.columns},
                          {"partitions", std::move(partitions)},
                          // #837: whether this node can see the WHOLE dataset.
                          // An agent that reads `rows` must be able to tell a
                          // total from a fraction.
                          {"degraded", dataset.degraded},
                          {"degraded_reason", dataset.degraded_reason},
                          {"expected_partitions", dataset.expected_partitions},
                          {"expected_rows", dataset.expected_rows}};
}

// The schema of dataset_summary_json — shared by get_dataset (the value) and
// list_datasets (the array items).
const nlohmann::json& dataset_summary_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"name", "database", "table_id", "format", "replication", "rows",
          "size_bytes", "columns", "partitions", "degraded", "degraded_reason",
          "expected_partitions", "expected_rows"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"database", {{"type", "string"}}},
          {"table_id", {{"type", "string"}}},
          {"format", {{"type", "string"}}},
          {"replication", {{"type", "integer"}}},
          {"rows", {{"type", "integer"}}},
          {"size_bytes", {{"type", "integer"}}},
          {"columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"partitions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"partition_id", "blob_ref", "row_start", "row_end",
                "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"blob_ref", {{"type", "string"}}},
                {"row_start", {{"type", "integer"}}},
                {"row_end", {{"type", "integer"}}},
                {"owner_nodes",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}}}}}}}}}},
          {"degraded", {{"type", "boolean"}}},
          {"degraded_reason", {{"type", "string"}}},
          {"expected_partitions", {{"type", "integer"}}},
          {"expected_rows", {{"type", "integer"}}}}}};
    return schema;
}

const nlohmann::json& get_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"dataset"}},
        {"additionalProperties", false},
        {"properties",
         {{"dataset",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the dataset name (= the table name)"}}},
          {"database",
           {{"type", "string"},
            {"description", "the owning database — required only when the "
                            "same dataset name exists in several databases"}}}}}};
    return schema;
}

ApiResult handle_get_dataset(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("dataset").get<std::string>();
    const std::string database = input.value("database", "");
    std::vector<zetta::data::DatasetSummary> matches;
    for (zetta::data::DatasetSummary& dataset : context.node.datasets()) {
        if (dataset.name != name) continue;
        if (!database.empty() && dataset.database != database) continue;
        matches.push_back(std::move(dataset));
    }
    if (matches.empty()) {
        return ApiResult::failure(
            "not_found", "no dataset named '" + name + "'" +
                             (database.empty() ? "" : " in database '" + database + "'"));
    }
    if (matches.size() > 1) {
        std::string databases;
        for (const zetta::data::DatasetSummary& match : matches) {
            if (!databases.empty()) databases += ", ";
            databases += match.database;
        }
        return ApiResult::failure(
            "engine_error", "dataset '" + name + "' exists in several databases (" +
                                databases + ") — pass \"database\" to pick one");
    }
    return ApiResult::success(dataset_summary_json(matches.front()));
}

// --- list_* siblings (#616) ----------------------------------------------------
//
// Compact structured listings over the same live Node registries the system.*
// SQL views compute from — for the places sql's flattened tabular rows aren't
// ergonomic for an agent (structured arrays, stable ids to feed the get_* reads).

const nlohmann::json& list_runs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_runs_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"runs"}},
        {"additionalProperties", false},
        {"properties",
         {{"runs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "project", "graph", "deployment", "status", "created",
                "updated", "log_count", "output_count"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"deployment", {{"type", "string"}}},
                {"status", {{"type", "string"}}},
                {"created", {{"type", "string"}}},
                {"updated", {{"type", "string"}}},
                {"log_count", {{"type", "integer"}}},
                {"output_count", {{"type", "integer"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_runs(const nlohmann::json&, ApiContext& context) {
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& run : context.node.runs()) {
        runs.push_back({{"id", run->id()},
                        {"project", run->project()},
                        {"graph", run->graph()},
                        {"deployment", run->deployment()},
                        {"status", std::string(to_string(run->status()))},
                        {"created", run->created()},
                        {"updated", run->updated()},
                        {"log_count", run->logs().size()},
                        {"output_count", run->outputs().size()}});
    }
    nlohmann::json output;
    output["runs"] = std::move(runs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_models_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_models_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"models"}},
        {"additionalProperties", false},
        {"properties",
         {{"models",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "name", "kind", "project", "run", "created",
                "has_weights"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"kind", {{"type", "string"}}},
                {"project", {{"type", "string"}}},
                {"run", {{"type", "string"}}},
                {"created", {{"type", "string"}}},
                {"has_weights", {{"type", "boolean"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_models(const nlohmann::json&, ApiContext& context) {
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : context.node.models()) {
        models.push_back({{"id", model->id()},
                          {"name", model->name()},
                          {"kind", model->kind()},
                          {"project", model->project()},
                          {"run", model->run()},
                          {"created", model->created()},
                          {"has_weights", model->has_weights()}});
    }
    nlohmann::json output;
    output["models"] = std::move(models);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_jobs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_jobs_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"jobs"}},
        {"additionalProperties", false},
        {"properties",
         {{"jobs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "kind", "status", "project", "graph", "run", "created",
                "updated", "detail"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"kind", {{"type", "string"}}},
                {"status", {{"type", "string"}}},
                {"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"run", {{"type", "string"}}},
                {"created", {{"type", "string"}}},
                {"updated", {{"type", "string"}}},
                {"detail", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_jobs(const nlohmann::json&, ApiContext& context) {
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& job : context.node.jobs()) {
        jobs.push_back({{"id", job->id()},
                        {"kind", std::string(to_string(job->kind()))},
                        {"status", std::string(to_string(job->status()))},
                        {"project", job->project()},
                        {"graph", job->graph()},
                        {"run", job->run()},
                        {"created", job->created()},
                        {"updated", job->updated()},
                        {"detail", job->detail()}});
    }
    nlohmann::json output;
    output["jobs"] = std::move(jobs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_indexes_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_indexes_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"indexes"}},
        {"additionalProperties", false},
        {"properties",
         {{"indexes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "table", "kind", "backend", "fields",
                            "created"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"table", {{"type", "string"}}},
                {"kind", {{"type", "string"}}},
                {"backend", {{"type", "string"}}},
                {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"created", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

// The index registry as a tool (#642) — read from the same live source
// (Node::indexes()) the system.indexes SQL view computes from (RULE-7), so an
// agent can discover what `search` can target before searching.
ApiResult handle_list_indexes(const nlohmann::json&, ApiContext& context) {
    nlohmann::json indexes = nlohmann::json::array();
    for (const auto& index : context.node.indexes()) {
        indexes.push_back({{"id", index->id()},
                           {"table", index->table()},
                           {"kind", std::string(index->kind_name())},
                           {"backend", index->backend()},
                           {"fields", index->fields()},
                           {"created", index->created()}});
    }
    nlohmann::json output;
    output["indexes"] = std::move(indexes);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_datasets_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_datasets_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"datasets"}},
        {"additionalProperties", false},
        {"properties",
         {{"datasets", {{"type", "array"}, {"items", dataset_summary_schema()}}}}}};
    return schema;
}

ApiResult handle_list_datasets(const nlohmann::json&, ApiContext& context) {
    nlohmann::json datasets = nlohmann::json::array();
    for (const zetta::data::DatasetSummary& dataset : context.node.datasets()) {
        datasets.push_back(dataset_summary_json(dataset));
    }
    nlohmann::json output;
    output["datasets"] = std::move(datasets);
    return ApiResult::success(std::move(output));
}

// --- create_project ----------------------------------------------------------

const nlohmann::json& create_project_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {
             {"name", {{"type", "string"}, {"minLength", 1}}},
             {"owner",
              {{"type", "string"},
               {"description", "owner key id recorded on the project (optional)"}}},
             {"description",
              {{"type", "string"},
               {"description",
                "a one-line human description of the project's intent (optional)"}}},
         }}};
    return schema;
}

const nlohmann::json& create_project_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "owner"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"owner", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_create_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::string owner =
        input.value("owner", context.acting_key.empty() ? std::string("local")
                                                        : context.acting_key);
    const std::string description = input.value("description", std::string{});
    auto project = context.node.create_project(name, owner, description);
    if (!project) {
        return ApiResult::failure(
            "engine_error",
            "create_project rejected '" + name + "' (empty name or name clash)");
    }
    nlohmann::json output;
    output["name"] = name;
    output["owner"] = owner;
    return ApiResult::success(std::move(output));
}

// --- expose_model / expose_index (#648, docs/api/serving.md) ------------------
//
// Exposure flips the entity's REPLICATED `exposed` flag (Model::set_exposed /
// Index::set_exposed): the CRDT value is the source of truth, so every node of
// the cluster converges on serving — or refusing — the entity's HTTP endpoint.
// Idempotent both ways; un-exposing is the same one action with `false`.

const nlohmann::json& expose_model_schema() {
    static const nlohmann::json schema = id_input_schema(
        "model",
        "the model's uuid (system.models.model_id), or its display name");
    return schema;
}

const nlohmann::json& expose_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "exposed", "endpoint"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"exposed", {{"type", "boolean"}}},
          {"endpoint",
           {{"type", "string"},
            {"description",
             "the served route (by id — the unambiguous spelling; the display "
             "name also resolves)"}}}}}};
    return schema;
}

ApiResult set_model_exposure(const nlohmann::json& input, ApiContext& context,
                             bool exposed) {
    const std::string target = input.at("model").get<std::string>();
    const auto model = resolve_served_model(context.node, target);
    if (!model) {
        return ApiResult::failure("not_found",
                                  "no model named '" + target + "'");
    }
    model->set_exposed(exposed);
    nlohmann::json output;
    output["model_id"] = model->id();
    output["name"] = model->name();
    output["exposed"] = model->exposed();
    output["endpoint"] = model_endpoint_path(model->id());
    return ApiResult::success(std::move(output));
}

ApiResult handle_expose_model(const nlohmann::json& input,
                              ApiContext& context) {
    return set_model_exposure(input, context, /*exposed=*/true);
}

ApiResult handle_unexpose_model(const nlohmann::json& input,
                                ApiContext& context) {
    return set_model_exposure(input, context, /*exposed=*/false);
}

const nlohmann::json& expose_index_schema() {
    static const nlohmann::json schema = id_input_schema(
        "index",
        "the index's uuid (system.indexes.index_id), or an indexed table's "
        "name (resolves like `search`: the newest Search-kind index over it)");
    return schema;
}

const nlohmann::json& expose_index_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"index_id", "table", "exposed", "endpoint"}},
        {"additionalProperties", false},
        {"properties",
         {{"index_id", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"exposed", {{"type", "boolean"}}},
          {"endpoint",
           {{"type", "string"},
            {"description",
             "the served route (by id — the unambiguous spelling; the table "
             "name also resolves)"}}}}}};
    return schema;
}

ApiResult set_index_exposure(const nlohmann::json& input, ApiContext& context,
                             bool exposed) {
    const std::string target = input.at("index").get<std::string>();
    const auto index =
        zetta::index::resolve_search_target(context.node, target);
    if (!index) {
        return ApiResult::failure("not_found",
                                  "no index named '" + target + "'");
    }
    index->set_exposed(exposed);
    nlohmann::json output;
    output["index_id"] = index->id();
    output["table"] = index->table();
    output["exposed"] = index->exposed();
    output["endpoint"] = index_endpoint_path(index->id());
    return ApiResult::success(std::move(output));
}

ApiResult handle_expose_index(const nlohmann::json& input,
                              ApiContext& context) {
    return set_index_exposure(input, context, /*exposed=*/true);
}

ApiResult handle_unexpose_index(const nlohmann::json& input,
                                ApiContext& context) {
    return set_index_exposure(input, context, /*exposed=*/false);
}

// --- create_index (https://github.com/oramasearch/zetta/issues/825) ----------
//
// The public create path for an index over a loaded table: it records the Index
// entity (Node::create_index) then reconciles so this node's per-partition BM25F
// sub-indexes build immediately. With add_dataset's default (partitioned)
// placement the table has placed partitions the node owns, so `search` then takes
// the distributed path and returns ranked hits. This is the tool search()'s own
// resolution names ("create one with create_index").

const nlohmann::json& create_index_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"table"}},
        {"additionalProperties", false},
        {"properties",
         {{"table",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the source table to index (load it first with add_dataset)"}}},
          {"fields",
           {{"type", "array"},
            {"minItems", 1},
            {"uniqueItems", true},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"description", "the fields to index (default: all of the table's columns)"}}},
          {"kind",
           {{"type", "string"},
            {"enum", {"search", "vector", "feature"}},
            {"description",
             "index kind (default search — the only kind with a built-in "
             "backend today)"}}},
          {"backend",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the IndexBackend that builds it (default oramacore_fulltext, "
             "BM25F full-text; memory_search is the exact-match reference)"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the table's database — needed only when the same table name "
             "exists in several databases"}}}}}};
    return schema;
}

const nlohmann::json& create_index_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"index_id", "table", "kind", "backend", "fields", "exposed"}},
        {"additionalProperties", false},
        {"properties",
         {{"index_id", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"kind", {{"type", "string"}}},
          {"backend", {{"type", "string"}}},
          {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"exposed", {{"type", "boolean"}}}}}};
    return schema;
}

// Resolve a table by name — within `database` when named, else the first database
// holding a table of that name (the index_step_executor resolution).
std::shared_ptr<zetta::data::Table> resolve_index_source_table(
    Node& node, const std::string& table, const std::string& database_hint) {
    if (!database_hint.empty()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(database_hint);
        return database ? database->find_table(table) : nullptr;
    }
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(name);
        if (database) {
            if (const std::shared_ptr<zetta::data::Table> found =
                    database->find_table(table)) {
                return found;
            }
        }
    }
    return nullptr;
}

ApiResult handle_create_index(const nlohmann::json& input, ApiContext& context) {
    const std::string table = input.at("table").get<std::string>();
    std::vector<std::string> fields =
        input.value("fields", std::vector<std::string>{});
    const std::string kind_name = input.value("kind", std::string{"search"});
    const std::string backend =
        input.value("backend", std::string{"oramacore_fulltext"});
    const std::string database_hint = input.value("database", std::string{});

    // (1) The source table must exist — an index is built over its partitions.
    const std::shared_ptr<zetta::data::Table> source_table =
        resolve_index_source_table(context.node, table, database_hint);
    if (!source_table) {
        return ApiResult::failure(
            "not_found",
            "table '" + table + "' not found — load it first with add_dataset");
    }

    // Default the indexed fields to ALL of the table's columns (from its first
    // row) when none are named — indexing everything is the sane default; a
    // caller passes fields only to narrow it.
    if (fields.empty() && source_table->store().size() > 0) {
        const zetta::storage::ParquetLazyPtr first_row = source_table->store().at(0);
        if (first_row && first_row->is_object())
            for (auto it = first_row->begin(); it != first_row->end(); ++it)
                fields.push_back(it.key());
    }
    if (fields.empty()) {
        return ApiResult::failure(
            "invalid_input",
            "no fields to index — the table has no rows yet; load data first or "
            "pass fields explicitly");
    }

    // (2) The kind is schema-constrained; parse it back to the enum.
    const std::optional<zetta::index::IndexKind> kind =
        zetta::index::index_kind_from_name(kind_name);
    if (!kind) {
        return ApiResult::failure("invalid_input",
                                  "unknown index kind '" + kind_name + "'");
    }

    zetta::index::IndexValue value;
    value.table = table;
    value.kind = *kind;
    value.backend = backend;
    value.fields = fields;

    // (3) The backend must resolve for this kind, or the recorded index could
    // never build — fail with the reason rather than persist a dead entity.
    if (!context.node.index_registry().resolve(value.spec())) {
        return ApiResult::failure(
            "invalid_input", "no index backend named '" + backend +
                                 "' implements kind '" + kind_name +
                                 "' on this node");
    }

    // (4) Record the entity, then reconcile so this node's per-partition
    // sub-indexes build now (search would also reconcile before querying).
    const std::shared_ptr<zetta::index::Index> index =
        context.node.create_index(std::move(value));
    if (!index) {
        return ApiResult::failure(
            "engine_error",
            "could not record the index (needs a table, a backend, and >= 1 "
            "field)");
    }
    context.node.reconcile_search_indexes();

    nlohmann::json output;
    output["index_id"] = index->id();
    output["table"] = index->table();
    output["kind"] = std::string(index->kind_name());
    output["backend"] = index->backend();
    output["fields"] = index->fields();
    output["exposed"] = index->exposed();
    return ApiResult::success(std::move(output));
}

// --- list_traces / get_trace ---------------------------------------------

const nlohmann::json& list_traces_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"limit",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 500},
            {"description", "most recent spans to return (default 50)"}}}}}};
    return schema;
}

// One recorded span as observability::to_json renders it — the same shape the
// system.spans view flattens. Kept open: spans carry free-form attributes.
const nlohmann::json& span_json_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"trace_id", "span_id", "name", "ok", "duration_ms"}},
        {"properties",
         {{"trace_id", {{"type", "string"}}},
          {"span_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"ok", {{"type", "boolean"}}},
          {"duration_ms", {{"type", "number"}}},
          {"attributes", {{"type", "object"}}}}}};
    return schema;
}

const nlohmann::json& list_traces_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"spans"}},
        {"additionalProperties", false},
        {"properties",
         {{"spans", {{"type", "array"}, {"items", span_json_schema()}}}}}};
    return schema;
}

ApiResult handle_list_traces(const nlohmann::json& input, ApiContext& context) {
    if (!context.recorder) {
        return ApiResult::failure("engine_error",
                                  "no span recorder is wired on this node");
    }
    const std::size_t limit = input.value("limit", 50);
    nlohmann::json spans = nlohmann::json::array();
    for (const auto& span : context.recorder->recent(limit)) {
        spans.push_back(zetta::observability::to_json(span));
    }
    nlohmann::json output;
    output["spans"] = std::move(spans);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_trace_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"trace_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"trace_id",
           {{"type", "string"}, {"minLength", 32}, {"maxLength", 32}}}}}};
    return schema;
}

const nlohmann::json& get_trace_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"trace_id", "spans"}},
        {"additionalProperties", false},
        {"properties",
         {{"trace_id", {{"type", "string"}}},
          {"spans", {{"type", "array"}, {"items", span_json_schema()}}}}}};
    return schema;
}

ApiResult handle_get_trace(const nlohmann::json& input, ApiContext& context) {
    if (!context.recorder) {
        return ApiResult::failure("engine_error",
                                  "no span recorder is wired on this node");
    }
    const auto trace_spans =
        context.recorder->trace(input.at("trace_id").get<std::string>());
    nlohmann::json spans = nlohmann::json::array();
    for (const auto& span : trace_spans) {
        spans.push_back(zetta::observability::to_json(span));
    }
    nlohmann::json output;
    output["trace_id"] = input.at("trace_id");
    output["spans"] = std::move(spans);
    return ApiResult::success(std::move(output));
}

// --- submit_feedback -----------------------------------------------------------

const nlohmann::json& submit_feedback_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"description"}},
        {"additionalProperties", false},
        {"properties",
         {{"description",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "what happened / what you wish for, verbatim"}}},
          {"trace_ids",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "traces to attach (optional; default = the most "
                            "recent traces on this node)"}}},
          {"export_path",
           {{"type", "string"},
            {"description",
             "local file path to ALSO write the bundle to — the explicit "
             "user export; nothing leaves the cluster on its own "
             "(optional)"}}}}}};
    return schema;
}

const nlohmann::json& submit_feedback_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"feedback_id", "name", "database", "size", "sha256",
          "traces_attached", "errors_attached", "crashes_attached"}},
        {"additionalProperties", false},
        {"properties",
         {{"feedback_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"database", {{"type", "string"}}},
          {"size", {{"type", "integer"}}},
          {"sha256", {{"type", "string"}}},
          {"traces_attached", {{"type", "integer"}}},
          {"errors_attached", {{"type", "integer"}}},
          {"crashes_attached", {{"type", "integer"}}},
          {"exported_to",
           {{"type", "string"},
            {"description",
             "present only when export_path was given and the export "
             "succeeded"}}}}}};
    return schema;
}

// The database that holds feedback bundles as large objects (replicated
// metadata + peer-transferred body, like every large object).
constexpr std::string_view kFeedbackDatabase = "feedback";

ApiResult handle_submit_feedback(const nlohmann::json& input,
                                 ApiContext& context) {
    zetta::observability::FeedbackBundleOptions options;
    options.description = input.at("description").get<std::string>();
    if (const auto trace_ids = input.find("trace_ids");
        trace_ids != input.end()) {
        options.trace_ids = trace_ids->get<std::vector<std::string>>();
    }
    options.actor =
        context.acting_key.empty() ? std::string("local") : context.acting_key;

    std::string reason;
    const auto bundle =
        zetta::observability::build_feedback_bundle(context.node, options, &reason);
    if (!bundle) {
        return ApiResult::failure("engine_error", reason);
    }

    auto database = context.node.find_database(std::string(kFeedbackDatabase));
    if (!database) {
        database = context.node.create_database(std::string(kFeedbackDatabase));
    }
    if (!database) {
        return ApiResult::failure(
            "engine_error", "submit_feedback: could not open the '" +
                                std::string(kFeedbackDatabase) + "' database");
    }
    const std::string name =
        "feedback-" + bundle->at("submitted_at").get<std::string>() + ".json";
    const auto stored =
        database->large_objects().import_bytes(name, bundle->dump(2));
    if (!stored) {
        return ApiResult::failure(
            "engine_error", "submit_feedback: could not store the bundle");
    }

    nlohmann::json output;
    output["feedback_id"] = stored->id;
    output["name"] = stored->name;
    output["database"] = std::string(kFeedbackDatabase);
    output["size"] = stored->size;
    output["sha256"] = stored->sha256;
    output["traces_attached"] = bundle->at("traces").size();
    output["errors_attached"] = bundle->at("recent_errors").size();
    output["crashes_attached"] = bundle->at("crashes").size();

    // The explicit export (and the ONLY way bytes leave the store): the user
    // asked for a copy at a path of their choosing.
    if (const auto export_path = input.find("export_path");
        export_path != input.end()) {
        const std::string destination = export_path->get<std::string>();
        if (!database->large_objects().export_file(stored->id, destination)) {
            return ApiResult::failure(
                "engine_error",
                "submit_feedback: bundle stored as " + stored->id +
                    " but the export to '" + destination + "' failed");
        }
        output["exported_to"] = destination;
    }
    return ApiResult::success(std::move(output));
}

// --- governed action parity (#617) ---------------------------------------------
//
// The mutating half of the product loop, each a thin caller of the SAME engine
// entry point the GUI drives (RULE-1/RULE-3 cannot be bypassed by construction:
// authorize + audit happen in Registry::call, the gate inside the engine seams).
// Long work is `kind: job`: submit, return the engine's durable Run id, poll
// progress through the read surface (get_run / system.runs) — no request-scoped
// blocking, no new job infrastructure. Errors carry the engine's path-qualified
// reason verbatim. See docs/api/governed-actions.md.

std::string actor_of(const ApiContext& context) {
    return context.acting_key.empty() ? std::string("local") : context.acting_key;
}

// Resolve a project by name or uuid (the get_project convention: entity refs
// elsewhere carry the project uuid, so accept it as the handle too).
std::shared_ptr<zetta::pipeline::Project> resolve_project(
    ApiContext& context, const std::string& name) {
    auto project = context.node.find_project(name);
    if (project) return project;
    for (const std::string& candidate : context.node.project_names()) {
        auto by_name = context.node.find_project(candidate);
        if (by_name && by_name->id() == name) return by_name;
    }
    return nullptr;
}

// Resolve a stored graph on a project by name or uuid.
std::shared_ptr<zetta::pipeline::ProjectGraph> resolve_graph(
    const zetta::pipeline::Project& project, const std::string& name) {
    auto graph = project.find_graph(name);
    if (graph) return graph;
    return project.find_graph_by_id(name);
}

nlohmann::json input_json(const zetta::pipeline::ProjectInput& input) {
    return nlohmann::json{{"input_id", input.id()},
                          {"kind", std::string(to_string(input.kind()))},
                          {"filename", input.filename()},
                          {"content_hash", input.content_hash()},
                          {"size", input.size()}};
}

nlohmann::json graph_json(const zetta::pipeline::ProjectGraph& graph) {
    return nlohmann::json{{"graph_id", graph.id()},
                          {"name", graph.name()},
                          {"steps", graph.graph().node_count()},
                          {"edges", graph.graph().edge_count()},
                          {"approval", graph.approval_status()}};
}

// --- get_graph / save_graph: the visual graph workspace's read + write path ---

const nlohmann::json& graph_ref_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the owning project's name or uuid"}}},
          {"graph",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the graph's name or uuid"}}}}}};
    return schema;
}

const nlohmann::json& graph_structure_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"graph_id", "name", "project", "nodes", "edges"}},
        {"additionalProperties", false},
        {"properties",
         {{"graph_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"project", {{"type", "string"}}},
          {"approval",
           {{"type", "string"}, {"enum", {"proposed", "approved"}},
            {"description", "plan-approval state (#885)"}}},
          {"approved_by", {{"type", "string"}}},
          {"approved_at", {{"type", "string"}}},
          {"nodes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description", "a step: { id, kind, name }"}}}}},
          {"edges",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description", "a connection: { from, to, kind }"}}}}}}}};
    return schema;
}

ApiResult handle_get_graph(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found",
                                  "no graph named '" + graph_name +
                                      "' on project '" + project->name() + "'");
    }
    nlohmann::json output = zetta::ir::to_json(graph->graph());  // { nodes, edges }
    output["graph_id"] = graph->id();
    output["name"] = graph->name();
    output["project"] = project->name();
    // Plan-approval state (#885) so the Graph Workspace / Proposed Solution
    // screen shows proposed vs approved and enables the approve action.
    output["approval"] = graph->approval_status();
    output["approved_by"] = graph->approved_by();
    output["approved_at"] = graph->approved_at();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& save_graph_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph", "nodes", "edges"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the owning project's name or uuid"}}},
          {"graph",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the graph's name or uuid (created if absent)"}}},
          {"nodes",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"description", "the steps: each { id, kind, name }"}}},
          {"edges",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"description", "the connections: each { from, to, kind }"}}},
          {"new_name",
           {{"type", "string"}, {"description", "rename the graph (optional)"}}}}}};
    return schema;
}

ApiResult handle_save_graph(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + project_name + "'");
    }
    nlohmann::json document;
    document["nodes"] = input.at("nodes");
    document["edges"] = input.at("edges");
    std::string parse_error;
    std::optional<zetta::ir::ExecutionGraph> parsed =
        zetta::ir::from_json(document, &parse_error);
    if (!parsed) {
        return ApiResult::failure("invalid_input", "invalid graph: " + parse_error);
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const std::string new_name = input.value("new_name", std::string{});
    std::shared_ptr<zetta::pipeline::ProjectGraph> saved;
    if (const auto existing = resolve_graph(*project, graph_name)) {
        saved = project->update_graph(existing->name(), std::move(*parsed), new_name);
    } else {
        saved = project->create_graph(graph_name, std::move(*parsed));
    }
    if (!saved) {
        return ApiResult::failure(
            "engine_error",
            "could not save the graph (empty name, or a name collision)");
    }
    nlohmann::json output = graph_json(*saved);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- approve_plan (#885, review-before-build) -----------------------------------

const nlohmann::json& approve_plan_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the owning project's name or uuid"}}},
          {"graph",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the stored plan's name or uuid to approve"}}}}}};
    return schema;
}

const nlohmann::json& approve_plan_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph", "approval", "approved_by", "approved_at"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}}},
          {"graph", {{"type", "string"}}},
          {"approval", {{"type", "string"}, {"enum", {"proposed", "approved"}}}},
          {"approved_by", {{"type", "string"}}},
          {"approved_at", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_approve_plan(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const auto existing = resolve_graph(*project, graph_name);
    if (!existing) {
        return ApiResult::failure("not_found",
                                  "no graph named '" + graph_name +
                                      "' on project '" + project->name() + "'");
    }
    // Governed + audited inside the engine (Project::approve_graph): captures the
    // approval into the CRDT (peers reconcile it) and records the audit event.
    const auto approved = project->approve_graph(
        existing->name(), actor_of(context), zetta::util::now_iso8601());
    if (!approved) {
        return ApiResult::failure("engine_error", "could not approve the plan");
    }
    nlohmann::json output;
    output["project"] = project->name();
    output["graph"] = approved->name();
    output["approval"] = approved->approval_status();
    output["approved_by"] = approved->approved_by();
    output["approved_at"] = approved->approved_at();
    return ApiResult::success(std::move(output));
}

// The schema of input_json + the owning project — the output of the two
// input-producing actions (capture_intent, import_n8n).
const nlohmann::json& project_input_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"input_id", "kind", "filename", "content_hash", "size", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"input_id", {{"type", "string"}}},
          {"kind", {{"type", "string"}}},
          {"filename", {{"type", "string"}}},
          {"content_hash", {{"type", "string"}}},
          {"size", {{"type", "integer"}}},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

// The schema of graph_json + the owning project — the output of the three
// compilers (compile_intent, compile_n8n, compile_python).
const nlohmann::json& project_graph_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"graph_id", "name", "steps", "edges", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"graph_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"steps", {{"type", "integer"}}},
          {"edges", {{"type", "integer"}}},
          {"approval",
           {{"type", "string"}, {"enum", {"proposed", "approved"}},
            {"description", "plan-approval state (#885); a fresh plan is proposed"}}},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

// A ValidationReport as its ADL to_json renders it (the shape mirrored by
// schemas/validation_report.schema.json). Kept open at the report level: the
// gate's checks evolve with the check catalog.
const nlohmann::json& validation_report_json_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"checks", "block_deploy"}},
        {"properties",
         {{"checks",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"name", "status"}},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"status",
                 {{"type", "string"}, {"enum", {"pass", "warn", "fail"}}}},
                {"reason", {{"type", "string"}}}}}}}}},
          {"verdict",
           {{"type", "string"}, {"enum", {"pass", "warn", "fail"}}}},
          {"block_deploy", {{"type", "boolean"}}}}}};
    return schema;
}

// The first failing check's name + reason — the line the Deploy panel shows
// when the gate blocks (RULE-1), surfaced verbatim over MCP.
std::string first_gate_failure(const zetta::pipeline::ValidationReport& report) {
    for (const zetta::pipeline::CheckResult& check : report.checks) {
        if (check.status == zetta::pipeline::CheckStatus::Fail) {
            return check.name + ": " + check.reason;
        }
    }
    return "gate blocked deploy";
}

// --- capture_intent ------------------------------------------------------------

const nlohmann::json& capture_intent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "text"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"text",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the natural-language intent, verbatim"}}}}}};
    return schema;
}

ApiResult handle_capture_intent(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    auto captured = project->capture_intent(input.at("text").get<std::string>(),
                                            actor_of(context));
    if (!captured) {
        return ApiResult::failure(
            "engine_error",
            "capture_intent rejected the text (empty/whitespace-only intent, "
            "or the input folder could not be created)");
    }
    nlohmann::json output = input_json(*captured);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- import_n8n ------------------------------------------------------------------

const nlohmann::json& import_n8n_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "content"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"content",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the n8n workflow export JSON, verbatim (stored "
                            "byte-for-byte after schema validation)"}}},
          {"filename",
           {{"type", "string"},
            {"description",
             "recorded as the artifact's filename (default workflow.json)"}}}}}};
    return schema;
}

ApiResult handle_import_n8n(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const zetta::pipeline::N8nImportResult imported =
        zetta::pipeline::import_n8n_workflow(
            *project, input.at("content").get<std::string>(),
            input.value("filename", "workflow.json"), actor_of(context));
    if (!imported.ok()) {
        return ApiResult::failure("engine_error", imported.error);
    }
    nlohmann::json output = input_json(*imported.input);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- compile_intent / compile_n8n / compile_python -------------------------------
//
// Three thin compilers onto the same destination: a named, editable, replicated
// ProjectGraph (Project::create_graph — which audits the graph change). Each
// mirrors its engine entry point: Project::compile_intent_to_graph (#311),
// compile_n8n_workflow (#308), input::PythonFrontend (#520/#522).

const nlohmann::json& compile_intent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"input_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the captured intent's uuid (capture_intent). Optional: omit it and the "
             "project's most recently captured intent compiles, which is what you want "
             "in the common case of describing a goal and building it"}}},
          {"name",
           {{"type", "string"},
            {"description",
             "the stored graph's name (default '<shape> skeleton')"}}}}}};
    return schema;
}

ApiResult handle_compile_intent(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    // Threading a uuid from capture_intent into compile_intent made two calls out of
    // one act, and forgetting it failed with a schema error naming a property rather
    // than anything about intents. Omit it and the project's most recent intent
    // compiles — the common case by a wide margin.
    std::string input_id = input.value("input_id", std::string{});
    if (input_id.empty()) {
        for (const std::shared_ptr<zetta::pipeline::ProjectInput>& candidate :
             project->inputs()) {
            if (candidate) input_id = candidate->id();  // inputs() is insertion order
        }
        if (input_id.empty()) {
            return ApiResult::failure(
                "not_found",
                "project '" + project->name() +
                    "' has no captured intent to compile — describe the goal first "
                    "(capture_intent)");
        }
    }
    auto intent_input = project->find_input(input_id);
    if (!intent_input) {
        return ApiResult::failure("not_found", "no input with id '" + input_id +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    if (intent_input->kind() != zetta::pipeline::ProjectInputKind::Intent) {
        return ApiResult::failure(
            "engine_error", "input '" + input_id +
                                "' is an artifact, not a captured intent — "
                                "compile it with compile_n8n");
    }
    auto graph =
        project->compile_intent_to_graph(input_id, input.value("name", ""));
    if (!graph) {
        return ApiResult::failure(
            "engine_error",
            "compile_intent could not store a graph from input '" + input_id +
                "' (payload body not present locally, empty intent, or the "
                "graph name collides with an existing graph)");
    }
    nlohmann::json output = graph_json(*graph);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& compile_n8n_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "input_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"input_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored n8n artifact input's uuid (import_n8n)"}}},
          {"name",
           {{"type", "string"},
            {"description", "the stored graph's name (default 'n8n workflow')"}}}}}};
    return schema;
}

ApiResult handle_compile_n8n(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string input_id = input.at("input_id").get<std::string>();
    auto artifact = project->find_input(input_id);
    if (!artifact) {
        return ApiResult::failure("not_found", "no input with id '" + input_id +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    std::string reason;
    auto compiled = zetta::pipeline::compile_n8n_workflow(*artifact, &reason);
    if (!compiled) {
        return ApiResult::failure("engine_error", reason);
    }
    auto graph = project->create_graph(input.value("name", "n8n workflow"),
                                       std::move(*compiled));
    if (!graph) {
        return ApiResult::failure(
            "engine_error",
            "the graph name collides with an existing graph on project '" +
                project->name() + "' (or is empty)");
    }
    nlohmann::json output = graph_json(*graph);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& compile_python_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "source"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"source",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the simulated-Python pipeline source (zetta lowers "
                            "it to the IR; it never runs Python)"}}},
          {"name",
           {{"type", "string"},
            {"description",
             "the stored graph's name (default 'python program')"}}}}}};
    return schema;
}

ApiResult handle_compile_python(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const zetta::input::FrontendResult lowered =
        zetta::input::PythonFrontend{}.compile(
            input.at("source").get<std::string>());
    if (!lowered.ok()) {
        return ApiResult::failure("engine_error", lowered.error);
    }
    auto graph = project->create_graph(input.value("name", "python program"),
                                       lowered.graph);
    if (!graph) {
        return ApiResult::failure(
            "engine_error",
            "the graph name collides with an existing graph on project '" +
                project->name() + "' (or is empty)");
    }
    nlohmann::json output = graph_json(*graph);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- gate ------------------------------------------------------------------------

const nlohmann::json& gate_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"graph",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored graph's name, or its uuid"}}}}}};
    return schema;
}

const nlohmann::json& gate_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json report = validation_report_json_schema();
        report["properties"]["project"] = {{"type", "string"}};
        report["properties"]["graph"] = {{"type", "string"}};
        report["required"].push_back("project");
        report["required"].push_back("graph");
        return report;
    }();
    return schema;
}

ApiResult handle_gate(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    // Review-before-build (#885): a proposed plan must be approved before it can
    // be verified. Approval is the human "yes, build this" the design's Proposed
    // Solution screen collects; approve_plan flips it.
    if (!graph->approved()) {
        return ApiResult::failure(
            "not_approved",
            "the plan '" + graph->name() +
                "' is proposed — approve it (approve_plan) before verification");
    }
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    // A blocked graph is a successful gate *evaluation* — the report carries
    // the verdict; deploy/run are where a blocked graph becomes an error.
    nlohmann::json output = report;  // ADL to_json: checks + verdict + block_deploy
    output["project"] = project->name();
    output["graph"] = graph->name();
    return ApiResult::success(std::move(output));
}

// --- deploy ----------------------------------------------------------------------

const nlohmann::json& deploy_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"graph",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored graph's name, or its uuid"}}},
          {"target",
           {{"type", "string"},
            {"enum", {"local", "kubernetes", "vpc", "cloud"}},
            {"description", "the placement dial (default local; only local is "
                            "functional in R1)"}}}}}};
    return schema;
}

const nlohmann::json& deploy_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"deployment_id", "graph", "target", "gate", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"deployment_id", {{"type", "string"}}},
          {"graph", {{"type", "string"}}},
          {"target", {{"type", "string"}}},
          {"gate", validation_report_json_schema()},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_deploy(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    // Review-before-build (#885): no deploy of a plan the human hasn't approved.
    // (This is orthogonal to the correctness gate below — approval is the human
    // sign-off; the gate is the machine verification. Both must pass.)
    if (!graph->approved()) {
        return ApiResult::failure(
            "not_approved",
            "the plan '" + graph->name() +
                "' is proposed — approve it (approve_plan) before deploy");
    }
    const auto kind = zetta::pipeline::deployment_kind_from_name(
        input.value("target", "local"));
    if (!kind) {
        return ApiResult::failure("invalid_input", "unknown deployment target",
                                  "/target");
    }
    // The gate report first, for the failing check's reason; the engine's own
    // deploy_graph re-runs the gate and audits BOTH outcomes (RULE-1 lives in
    // the engine seam — this handler cannot bypass it by construction).
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    auto deployment = context.node.deploy_graph(
        graph->graph(), zetta::pipeline::DeploymentTarget{*kind, {}, {}},
        graph->name());
    if (!deployment) {
        if (report.block_deploy) {
            return ApiResult::failure("engine_error",
                                      "gate blocked deploy: " +
                                          first_gate_failure(report));
        }
        return ApiResult::failure(
            "engine_error",
            *kind == zetta::pipeline::DeploymentKind::Local
                ? "deploy_graph refused the graph (empty graph)"
                : "deploy_graph refused the target ('" +
                      zetta::pipeline::to_string(*kind) +
                      "' is planned but not functional in R1 — use 'local')");
    }
    nlohmann::json output;
    output["deployment_id"] = deployment->id();
    output["graph"] = graph->name();
    output["target"] = zetta::pipeline::to_string(*kind);
    output["gate"] = report;
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- run (kind: job — THE canonical runner, #637) ---------------------------------

const nlohmann::json& run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"graph",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored graph's name, or its uuid"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "the data to train on, in either form: a `database.table` landed by "
             "a connection or a query (what add_dataset / create_dataset_from_query "
             "produce), or a churn dataset directory on the node's filesystem. "
             "Default: the built-in demo dataset"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"model_name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model's name (default churn_model)"}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "at-risk list / playbook cap (0 = all)"}}},
          {"holdout_fraction",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"description",
             "hold this fraction of rows OUT of training and measure the model "
             "on them separately (e.g. 0.3). Without it the run's only metrics "
             "are in-sample — scored on the rows it trained on — and flatter"}}},
          {"coordinate",
           {{"type", "boolean"},
            {"description", "fan the run out to the live membership (default "
                            "true; false is only correct on a single-member "
                            "cluster)"}}}}}};
    return schema;
}

const nlohmann::json& run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "engine", "status"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"description",
             "the durable engine handle — poll get_run / system.runs"}}},
          {"engine", {{"type", "string"}, {"enum", {"churn", "structural"}}}},
          {"status", {{"type", "string"}}},
          {"failed_step",
           {{"type", "string"},
            {"description", "structural runs only, on failure"}}},
          {"error",
           {{"type", "string"},
            {"description", "structural runs only, on failure"}}},
          {"poll",
           {{"type", "string"},
            {"description", "churn runs: where to read progress"}}},
          {"dataset",
           {{"type", "object"},
            {"description",
             "table datasets only: the resolved column contract the run trained "
             "under"}}},
          {"dataset_notes",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "what was inferred rather than given — read these before trusting the "
             "model"}}}}}};
    return schema;
}

ApiResult handle_run(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name +
                                                   "' on project '" +
                                                   project->name() + "'");
    }

    // The gate, where the GUI's run paths have it (the governed console gates
    // every run; the coordinated launch re-checks it on every member): RULE-1
    // holds over MCP exactly as everywhere else.
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    if (report.block_deploy) {
        return ApiResult::failure(
            "engine_error", "blocked by gate: " + first_gate_failure(report));
    }

    // Non-churn graphs run structurally (DefaultStepExecutor) — quick, within
    // the call, same as the governed console's fallback path.
    if (!zetta::usecases::churn::is_churn_graph(graph->graph())) {
        const zetta::pipeline::ExecutionResult executed =
            context.node.run_graph(graph->graph(), project->id(), graph->id());
        if (!executed.run) {
            return ApiResult::failure("engine_error",
                                      "the run record could not be created");
        }
        nlohmann::json output;
        output["run_id"] = executed.run->id();
        output["engine"] = "structural";
        output["status"] = std::string(to_string(executed.run->status()));
        if (!executed.ok) {
            output["failed_step"] = executed.failed_step;
            output["error"] = executed.error;
        }
        return ApiResult::success(std::move(output));
    }

    // Churn-shaped: THE canonical runner over the node's live coordinator
    // (#637/#580, SPMD across the membership via the coordinated launch #601),
    // driven off-thread (AsyncChurnRun, #624). kind: job — return the durable
    // Run id as soon as the record exists; progress/outcome ride system.runs /
    // system.jobs / get_run.
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding wired no background "
            "action owner");
    }
    // ONE dataset_path grammar (#947): a `database.table` landed by a connection
    // or a query, or a dataset directory on the node's filesystem. Before this,
    // only the directory form was accepted and a customer's own data — already
    // partitioned and healthy — could not reach the pipeline that trains on it.
    zetta::usecases::churn::ChurnDataset dataset;
    zetta::usecases::churn::ChurnTableInput table;
    nlohmann::json dataset_notes = nlohmann::json::array();
    nlohmann::json dataset_contract;
    if (const auto dataset_path = input.find("dataset_path");
        dataset_path != input.end()) {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind == zetta::data::DatasetPathKind::Table) {
            zetta::data::DatasetContract overrides;
            if (const auto given = input.find("dataset"); given != input.end()) {
                std::string reason;
                const std::optional<zetta::data::DatasetContract> parsed =
                    zetta::data::dataset_contract_from_json(*given, &reason);
                if (!parsed) return ApiResult::failure("invalid_input", reason);
                overrides = *parsed;
            }
            const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
                context.node, path, overrides,
                zetta::data::default_dataset_sampler(context.node, context.cluster));
            if (!resolved.ok()) {
                return ApiResult::failure("engine_error", resolved.error);
            }
            table.table_id = resolved.table_id;
            table.contract = resolved.contract;
            // What was INFERRED travels back with the run: an assumption the caller
            // never sees is one they discover inside a trained model.
            dataset_notes = resolved.notes;
            dataset_contract = to_json(resolved.contract);
        } else {
            auto loaded = zetta::usecases::churn::load_churn_dataset(path.directory);
            if (!loaded.ok()) {
                return ApiResult::failure("engine_error", loaded.error);
            }
            dataset = std::move(loaded.dataset);
        }
    } else {
        dataset = zetta::usecases::churn::demo_churn_dataset();
    }
    zetta::usecases::churn::ChurnSolutionOptions options;
    options.project = project->id();
    options.graph = graph->id();
    options.model_name = input.value("model_name", options.model_name);
    options.top_k = input.value("top_k", options.top_k);
    options.coordinate = input.value("coordinate", true);
    options.params.holdout_fraction =
        input.value("holdout_fraction", options.params.holdout_fraction);

    std::set<std::string> prior;
    for (const auto& existing : context.node.runs()) {
        if (existing) prior.insert(existing->id());
    }
    auto handle = table.empty()
                      ? zetta::usecases::churn::AsyncChurnRun::start(
                            context.node, graph->graph(), std::move(dataset), options)
                      : zetta::usecases::churn::AsyncChurnRun::start(
                            context.node, graph->graph(), std::move(table), options);
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });

    // The Run record is created by the worker through the owner gate; this
    // handler IS on the owner thread (the bridge contract), so pump the gate
    // until the durable handle exists — milliseconds for the demo dataset,
    // bounded for a large custom one. An early-failing run (no live
    // coordinator, empty dataset) surfaces its engine reason instead.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string run_id;
    while (std::chrono::steady_clock::now() < deadline) {
        context.node.drain_owner_work();
        for (const auto& candidate : context.node.runs()) {
            if (!candidate || prior.contains(candidate->id())) continue;
            if (candidate->project() != project->id()) continue;
            if (candidate->graph() != graph->id()) continue;
            run_id = candidate->id();
            break;
        }
        if (!run_id.empty()) break;
        if (handle->done()) {
            if (handle->result().execution.run) {
                run_id = handle->result().execution.run->id();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (run_id.empty()) {
        if (handle->done() && !handle->result().ok()) {
            return ApiResult::failure("engine_error",
                                      handle->result().execution.error);
        }
        return ApiResult::failure(
            "engine_error",
            "the run was submitted but its Run record did not appear in time "
            "— poll list_runs for a run labelled project '" + project->id() +
                "'");
    }
    nlohmann::json output;
    output["run_id"] = run_id;
    output["engine"] = "churn";
    output["status"] = "running";
    output["poll"] = "get_run / system.runs for status+logs; system.jobs for "
                     "the featurize/train/explain/score jobs";
    if (!dataset_contract.is_null()) {
        output["dataset"] = std::move(dataset_contract);
        output["dataset_notes"] = std::move(dataset_notes);
    }
    return ApiResult::success(std::move(output));
}

// --- score ---------------------------------------------------------------------

const nlohmann::json& score_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model"}},
        {"additionalProperties", false},
        {"properties",
         {{"model",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model's id, or its name"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "`database.table` to score every row of; omit when supplying `rows`"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"rows",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"description",
             "score these rows instead of a dataset - an account that is not in the "
             "data at all. Each row is keyed by the model's own feature columns"}}},
          {"entities",
           {{"type", "array"},
            {"items", {{"type", "integer"}}},
            {"description",
             "entity key per supplied row (default: the row's index)"}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "cap the ranked list (0 = all)"}}}}}};
    return schema;
}

const nlohmann::json& score_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"ranked", "feature_columns", "rows_scored"}},
        {"properties",
         {{"ranked",
           {{"type", "array"},
            {"description", "entities by risk, highest first"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"entity", {{"type", "integer"}}},
                {"risk", {{"type", "number"}}}}}}}}},
          {"feature_columns",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "what the model was trained on, in the order it expects"}}},
          {"drivers",
           {{"type", "array"},
            {"description", "population drivers, strongest first"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"feature", {{"type", "string"}}},
                {"importance", {{"type", "number"}}}}}}}}},
          {"rows_scored", {{"type", "integer"}}},
          {"partitions_read", {{"type", "integer"}}}}}};
    return schema;
}

ApiResult handle_score(const nlohmann::json& input, ApiContext& context) {
    const std::string wanted = input.at("model").get<std::string>();
    std::shared_ptr<zetta::ml::Model> model = context.node.find_model(wanted);
    if (!model) {
        for (const std::shared_ptr<zetta::ml::Model>& candidate : context.node.models()) {
            if (candidate && candidate->value().name == wanted) {
                model = candidate;
                break;
            }
        }
    }
    if (!model) {
        return ApiResult::failure("not_found", "no model '" + wanted + "' (or with that id)");
    }

    const auto rows = input.find("rows");
    const auto dataset_path = input.find("dataset_path");
    if (rows == input.end() && dataset_path == input.end()) {
        return ApiResult::failure(
            "invalid_input",
            "give either `dataset_path` (score a whole dataset) or `rows` (score rows "
            "that are not in it)");
    }

    const std::size_t top_k = input.value("top_k", std::size_t{0});
    zetta::ml::TableScoreResult scored;
    if (rows != input.end()) {
        std::vector<std::int64_t> entities;
        if (const auto given = input.find("entities"); given != input.end()) {
            for (const nlohmann::json& entity : *given) {
                if (!entity.is_number_integer()) {
                    return ApiResult::failure("invalid_input",
                                              "entities[]: must be integers");
                }
                entities.push_back(entity.get<std::int64_t>());
            }
        }
        scored = zetta::ml::score_rows(*model, rows->get<std::vector<nlohmann::json>>(),
                                       entities, top_k);
    } else {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind != zetta::data::DatasetPathKind::Table) {
            return ApiResult::failure(
                "invalid_input",
                "score reads a `database.table` dataset; a dataset directory has no "
                "partitions to score near");
        }
        zetta::data::DatasetContract overrides;
        if (const auto given = input.find("dataset"); given != input.end()) {
            std::string reason;
            const std::optional<zetta::data::DatasetContract> parsed =
                zetta::data::dataset_contract_from_json(*given, &reason);
            if (!parsed) return ApiResult::failure("invalid_input", reason);
            overrides = *parsed;
        }
        const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
            context.node, path, overrides,
            zetta::data::default_dataset_sampler(context.node, context.cluster));
        if (!resolved.ok()) return ApiResult::failure("engine_error", resolved.error);

        compute::CpuDevice device;
        scored = zetta::ml::score_table(context.node, *model, resolved.table_id,
                                        resolved.contract, device, top_k);
    }
    if (!scored.ok()) return ApiResult::failure("engine_error", scored.error);

    nlohmann::json ranked = nlohmann::json::array();
    for (const zetta::ml::EntityScore& score : scored.ranked) {
        ranked.push_back({{"entity", score.entity}, {"risk", score.risk}});
    }
    nlohmann::json drivers = nlohmann::json::array();
    for (const zetta::ml::ModelDriver& driver : scored.drivers) {
        drivers.push_back({{"feature", driver.feature}, {"importance", driver.importance}});
    }
    nlohmann::json output;
    output["ranked"] = std::move(ranked);
    output["feature_columns"] = scored.feature_columns;
    output["drivers"] = std::move(drivers);
    output["rows_scored"] = scored.rows_scored;
    output["partitions_read"] = scored.partitions_read;
    return ApiResult::success(std::move(output));
}

// --- explain -----------------------------------------------------------------------
//
// "Why is THIS one at risk" (#948 step 6, #960 item 5). `score`'s drivers are the
// POPULATION ranking — the same list for every account — so they cannot answer the
// question a person asks when they open the at-risk list. Two accounts with the same
// score can be there for opposite reasons.
// -----------------------------------------------------------------------------------

const nlohmann::json& explain_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model"}},
        {"additionalProperties", false},
        {"properties",
         {{"model",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model's id, or its name"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "`database.table` holding the entity; pair with `entity`"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"entity",
           {{"type", "integer"},
            {"description",
             "the entity key to explain, read from `dataset_path`"}}},
          {"row",
           {{"type", "object"},
            {"description",
             "explain this row instead of a dataset entity — an account that is not "
             "in the data at all. Keyed by the model's own feature columns"}}}}}};
    return schema;
}

const nlohmann::json& explain_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"contributions", "base_value", "raw", "risk"}},
        {"properties",
         {{"entity",
           {{"type", "integer"},
            {"description", "present when a dataset entity was explained"}}},
          {"base_value",
           {{"type", "number"},
            {"description",
             "the model's expected raw score — what an account with nothing notable "
             "about it would get. Contributions are movements away from this"}}},
          {"raw",
           {{"type", "number"},
            {"description",
             "this row's raw score. Equals base_value + the sum of contributions"}}},
          {"risk",
           {{"type", "number"},
            {"description",
             "the same prediction as a probability, comparable with a ranked list. "
             "NOT the sum of the contributions — probabilities are not additive"}}},
          {"residual",
           {{"type", "number"},
            {"description",
             "raw - (base_value + sum of contributions); ~0 when the explanation is "
             "exact. Exposed so a caller can check the arithmetic rather than trust it"}}},
          {"contributions",
           {{"type", "array"},
            {"description",
             "every feature, strongest influence first. `contribution` is signed in "
             "raw-score space: positive pushed this row's score up. A contribution "
             "says how much this row's value moved the model relative to a typical "
             "row — it does not say the column caused the outcome"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"feature", {{"type", "string"}}},
                {"value", {{"type", "number"}}},
                {"contribution", {{"type", "number"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_explain(const nlohmann::json& input, ApiContext& context) {
    const std::string wanted = input.at("model").get<std::string>();
    std::shared_ptr<zetta::ml::Model> model = context.node.find_model(wanted);
    if (!model) {
        for (const std::shared_ptr<zetta::ml::Model>& candidate : context.node.models()) {
            if (candidate && candidate->value().name == wanted) {
                model = candidate;
                break;
            }
        }
    }
    if (!model) {
        return ApiResult::failure("not_found", "no model '" + wanted + "' (or with that id)");
    }

    const auto row = input.find("row");
    const auto entity = input.find("entity");
    if (row == input.end() && entity == input.end()) {
        return ApiResult::failure(
            "invalid_input",
            "give either `entity` with `dataset_path` (explain one account in the "
            "data) or `row` (explain an account that is not in it)");
    }

    zetta::ml::RowExplanation explained;
    if (row != input.end()) {
        explained = zetta::ml::explain_row(*model, *row);
    } else {
        const auto dataset_path = input.find("dataset_path");
        if (dataset_path == input.end()) {
            return ApiResult::failure("invalid_input",
                                      "`entity` needs `dataset_path` to read it from");
        }
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind != zetta::data::DatasetPathKind::Table) {
            return ApiResult::failure(
                "invalid_input",
                "explain reads a `database.table` dataset; a dataset directory has no "
                "partitions to read near");
        }
        zetta::data::DatasetContract overrides;
        if (const auto given = input.find("dataset"); given != input.end()) {
            std::string reason;
            const std::optional<zetta::data::DatasetContract> parsed =
                zetta::data::dataset_contract_from_json(*given, &reason);
            if (!parsed) return ApiResult::failure("invalid_input", reason);
            overrides = *parsed;
        }
        const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
            context.node, path, overrides,
            zetta::data::default_dataset_sampler(context.node, context.cluster));
        if (!resolved.ok()) return ApiResult::failure("engine_error", resolved.error);

        compute::CpuDevice device;
        explained = zetta::ml::explain_entity(context.node, *model, resolved.table_id,
                                              resolved.contract, device,
                                              entity->get<std::int64_t>());
    }
    if (!explained.ok()) return ApiResult::failure("engine_error", explained.error);

    nlohmann::json contributions = nlohmann::json::array();
    for (const zetta::ml::FeatureContribution& contribution : explained.contributions) {
        contributions.push_back({{"feature", contribution.feature},
                                 {"value", contribution.value},
                                 {"contribution", contribution.contribution}});
    }
    nlohmann::json output;
    if (explained.entity) output["entity"] = *explained.entity;
    output["base_value"] = explained.base_value;
    output["raw"] = explained.raw;
    output["risk"] = explained.risk;
    output["residual"] = explained.residual;
    output["contributions"] = std::move(contributions);
    return ApiResult::success(std::move(output));
}

// --- deployment_profile (#974) ------------------------------------------------------
//
// Charles's ask, on the 2026-08-05 call: "given a Helm chart dependency, this is how
// you should configure this deployment instance… at that point I don't need anything
// from you." The goal is not that we write a customer a chart — it is that a coding
// agent pointed at a node over MCP can produce one without asking us anything.
//
// Computed from the LIVE node, never from a document. That is the whole value: a
// deployment doc drifts from the binary, and the drift is only discovered in someone
// else's cluster. Same rule as the `system.*` read views.
//
// It never emits a credential. It says how to seed one.
// -----------------------------------------------------------------------------------

const nlohmann::json& deployment_profile_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"target",
           {{"type", "string"},
            {"enum", {"generic", "helm"}},
            {"description",
             "`helm` adds a values-shaped block a chart can consume directly, instead "
             "of prose an agent has to interpret. Default `generic`"}}}}}};
    return schema;
}

const nlohmann::json& deployment_profile_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"ports", "storage", "clustering", "security", "requires"}},
        {"properties",
         {{"ports", {{"type", "array"}, {"description", "every port, and what it carries"}}},
          {"storage", {{"type", "object"}}},
          {"clustering", {{"type", "object"}}},
          {"security", {{"type", "object"}}},
          {"identity", {{"type", "object"}}},
          {"requires",
           {{"type", "object"},
            {"description",
             "what this node needs from the cluster — and, as `none_of`, the "
             "dependencies people assume it needs and it does not"}}},
          {"helm", {{"type", "object"}, {"description", "present when target=helm"}}}}}};
    return schema;
}

ApiResult handle_deployment_profile(const nlohmann::json& input, ApiContext& context) {
    const std::string target = input.value("target", std::string("generic"));

    nlohmann::json ports = nlohmann::json::array();
    ports.push_back(
        {{"port", context.cluster != nullptr ? 7777 : 7777},
         {"name", "peer"},
         {"carries", "cluster traffic between nodes"},
         {"flag", "--listen"},
         {"note",
          "`--listen` moves THIS port, not the API. Setting it and then calling the "
          "port you chose returns nothing, which reads exactly like a dead node — the "
          "most common misconfiguration we see"}});
    ports.push_back({{"port", 4800},
                     {"name", "api"},
                     {"carries", "REST /v1, MCP at /mcp, and the web UI"},
                     {"flag", "--mcp-listen"},
                     {"note", "what a client Service should target"}});

    nlohmann::json profile;
    profile["ports"] = std::move(ports);
    profile["storage"] = {
        {"data_dir", "--data-dir"},
        {"durable", true},
        {"note",
         "the node's durable state. A StatefulSet wants one PersistentVolumeClaim per "
         "member; it is not shareable between members"}};
    profile["clustering"] = {
        {"model", "equal peers — no leader, no election"},
        {"join", "--peer HOST:PORT (repeatable) plus --join-token"},
        {"members_now", context.cluster != nullptr
                            ? static_cast<long long>(context.cluster->peer_count() + 1)
                            : 1},
        {"dial_out_only",
         "--dial-out-only for a member with no routable address (NAT, private VNet). "
         "It must never advertise a private pod IP: when two clusters share a pod CIDR "
         "the dial reaches the wrong node and membership undercounts"}};
    profile["security"] = {
        {"peer_transport", "mTLS, always"},
        {"plaintext_mode", false},
        {"note",
         "there is no plaintext mode to disable or justify to a reviewer; one shared "
         "join token per cluster"},
        {"external_egress_required", false},
        {"egress_note",
         "the web UI serves no external assets — no CDN, no external fonts — so an "
         "air-gapped node works"}};
    profile["identity"] = {
        {"seed", "--admin-token, or ZETTA_ADMIN_TOKEN"},
        {"why",
         "a fresh node otherwise generates its owner token and prints it once, which an "
         "automated deploy cannot capture. Seeding makes the deploy deterministic"},
        {"never_emitted",
         "this capability reports how to seed a credential and never a credential"}};
    profile["requires"] = {
        {"runtime", nlohmann::json::array({"one statically linked binary", "a data directory"})},
        {"none_of",
         nlohmann::json::array({"external Postgres", "Redis", "Python at runtime",
                                "Node.js at runtime", "a package manager at runtime"})},
        {"note",
         "the `none_of` list is the point: enterprise build templates commonly carry all "
         "of these, and zetta uses none of them"}};

    if (target == "helm") {
        // Values-shaped, so a chart consumes it rather than an agent interpreting prose.
        profile["helm"] = {
            {"values",
             {{"image", {{"repository", "<your registry>/zetta"}, {"tag", "<version>"}}},
              {"service",
               {{"api", {{"port", 4800}}},
                {"peer", {{"port", 7777}, {"clusterIP", "None"}}}}},
              {"persistence", {{"enabled", true}, {"mountPath", "/var/lib/zetta"}}},
              {"env",
               {{"ZETTA_ADMIN_TOKEN", "<from a Secret — never inline>"},
                {"ZETTA_CLUSTER_TOKEN", "<from a Secret>"},
                {"ZETTA_SEED_PEER", "<headless-service>:7777"}}},
              {"args", nlohmann::json::array({"--headless", "--data-dir",
                                              "/var/lib/zetta"})}}},
            {"shape", "StatefulSet + a headless Service on the peer port + a client Service on the API port"},
            {"probes",
             {{"readiness", "GET /v1/server_info on the API port"},
              {"note", "the API answering is the readiness signal; the peer port is not"}}}};
    }
    return ApiResult::success(std::move(profile));
}

// --- add_dataset -------------------------------------------------------------------

const nlohmann::json& add_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"path"}},
        {"additionalProperties", false},
        {"properties",
         {{"path",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a parquet/jsonl/json/csv file on the node's filesystem"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "target database, created if absent (default "
                            "datasets)"}}},
          {"table_name",
           {{"type", "string"},
            {"description", "override the table name derived from the file"}}},
          {"partitioned",
           {{"type", "boolean"},
            {"description", "shard + place the rows across the cluster "
                            "(default true)"}}},
          {"partition_only",
           {{"type", "boolean"},
            {"description",
             "do NOT capture the rows into the replicated document (#833) — the "
             "data lives only in its partition bodies, which their owners hold. "
             "This is the right setting for connector-sized data: capturing rows "
             "replicates every one of them to every peer, and reconciling them "
             "back rewrites the whole local store per row, which saturates the "
             "node's owner loop. Reads go through the scatter-gather select. "
             "Default false, for back-compatibility with small replicated "
             "tables"}}},
          {"partition_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 4096},
            {"description",
            "partitions to write; omit to let the partition scheduler "
            "size them (a target body size, with a floor of partitions per "
            "node) instead of fixing a count"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "owners per partition (default 3, clamped to the node count)"}}}}}};
    return schema;
}

const nlohmann::json& add_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"database", "table", "format", "rows", "partitions"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"format", {{"type", "string"}}},
          {"rows", {{"type", "integer"}}},
          {"partitions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"partition_id", "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"owner_nodes",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}}}}}}}}}}}}};
    return schema;
}

ApiResult handle_add_dataset(const nlohmann::json& input, ApiContext& context) {
    zetta::data::AddDatasetOptions options;
    options.database = input.value("database", options.database);
    options.table_name = input.value("table_name", options.table_name);
    options.partitioned = input.value("partitioned", options.partitioned);
    options.partition_only = input.value("partition_only", options.partition_only);
    // Absent means AUTO — the scheduler targets a partition size instead of a
    // constant (#833). A value present is a manual override, honoured exactly.
    if (const auto it = input.find("partition_count"); it != input.end()) {
        options.partition_count = it->get<std::uint32_t>();
    }
    options.replication_factor =
        input.value("replication_factor", options.replication_factor);
    options.members = context.node.cluster_members();  // live placement roster

    const zetta::data::AddDatasetResult added = zetta::data::add_dataset_file(
        context.node, input.at("path").get<std::string>(), options);
    if (!added.ok()) {
        return ApiResult::failure("engine_error", added.error);
    }
    nlohmann::json partitions = nlohmann::json::array();
    for (const auto& partition : added.partitions) {
        if (!partition) continue;
        partitions.push_back({{"partition_id", partition->id()},
                              {"owner_nodes", partition->owner_nodes()}});
    }
    nlohmann::json output;
    output["database"] = added.database ? added.database->name() : options.database;
    output["table"] = added.table ? added.table->name() : "";
    output["format"] = zetta::data::dataset_file::format_name(added.format);
    output["rows"] = added.rows;
    output["partitions"] = std::move(partitions);
    return ApiResult::success(std::move(output));
}

// --- saved connections + create_dataset_from_query ----------------------------------

// The connector config as an embeddable sub-schema: the code-owned
// connector_config_schema() minus its document-level keys, so it nests cleanly
// inside this capability's input schema (and therefore inside the OpenAPI doc).
const nlohmann::json& connector_property_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json nested = zetta::connect::connector_config_schema();
        nested.erase("$schema");
        nested.erase("$id");
        return nested;
    }();
    return schema;
}

// The shape every connection read surface returns — note the absence of a
// `token` anywhere in it. `SavedConnection::to_redacted_json()` produces it from
// `ConnectorConfig::to_json()`, which never emits the credential, so the
// redaction is structural rather than a field someone must remember to strip.
const nlohmann::json& saved_connection_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "connector"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"connector",
           {{"type", "object"},
            {"description",
             "the saved connector settings WITHOUT the credential — the token "
             "is never returned by any surface"}}},
          {"created",
           {{"type", "string"},
            {"description", "when the connection was last saved (ISO-8601 UTC)"}}}}}};
    return schema;
}

const nlohmann::json& create_connection_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "connector"}},
        {"additionalProperties", false},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 64},
            {"description",
             "the name to save this connection under (letters, digits, '_' and "
             "'-'); saving an existing name replaces it"}}},
          {"connector", connector_property_schema()}}}};
    return schema;
}

const nlohmann::json& create_connection_output_schema() {
    return saved_connection_schema();
}

const nlohmann::json& list_connections_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_connections_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"connections"}},
        {"additionalProperties", false},
        {"properties",
         {{"connections",
           {{"type", "array"}, {"items", saved_connection_schema()}}}}}};
    return schema;
}

const nlohmann::json& delete_connection_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& delete_connection_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "removed"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}}, {"removed", {{"type", "boolean"}}}}}};
    return schema;
}

ApiResult handle_create_connection(const nlohmann::json& input,
                                   ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::optional<zetta::connect::ConnectorConfig> config =
        zetta::connect::ConnectorConfig::from_json(input.at("connector"));
    if (!config) {
        return ApiResult::failure(
            "invalid_input",
            "connector: expected {type, account, token} with a known type "
            "(and an optional auth of pat|oauth, database, schema, warehouse, "
            "role)");
    }
    const std::string rejection =
        zetta::connect::save_connection(context.node, name, *config);
    if (!rejection.empty()) {
        return ApiResult::failure("invalid_input", rejection);
    }
    const std::optional<zetta::connect::SavedConnection> saved =
        zetta::connect::find_connection(context.node, name);
    if (!saved) {
        return ApiResult::failure("engine_error",
                                  "connection '" + name + "' did not persist");
    }
    // Redacted on the way out — a create call never echoes the credential back.
    return ApiResult::success(saved->to_redacted_json());
}

ApiResult handle_list_connections(const nlohmann::json& input,
                                  ApiContext& context) {
    (void)input;
    nlohmann::json connections = nlohmann::json::array();
    for (const zetta::connect::SavedConnection& saved :
         zetta::connect::list_connections(context.node)) {
        connections.push_back(saved.to_redacted_json());
    }
    nlohmann::json output;
    output["connections"] = std::move(connections);
    return ApiResult::success(std::move(output));
}

ApiResult handle_delete_connection(const nlohmann::json& input,
                                   ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    nlohmann::json output;
    output["name"] = name;
    output["removed"] = zetta::connect::remove_connection(context.node, name);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& create_dataset_from_query_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        // Exactly one of `connection` / `connector` is required; the schema
        // cannot say "exactly one" without oneOf (which the query-string REST
        // coercion does not model), so the handler enforces it and says which.
        {"required", {"query"}},
        {"additionalProperties", false},
        {"properties",
         {{"connection",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the name of a saved connection (create_connection / "
             "list_connections) — use this instead of an inline connector so "
             "the credential never travels with the request"}}},
          {"connector", connector_property_schema()},
          {"query",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a single read-only SELECT (or WITH) to run against the source"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "target database, created if absent (default datasets)"}}},
          {"table_name",
           {{"type", "string"},
            {"description", "name for the new table (default 'dataset')"}}},
          {"max_rows",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 10000000},
            {"description", "cap on rows pulled from the source (default "
                            "100000)"}}},
          {"partitioned",
           {{"type", "boolean"},
            {"description",
             "shard + place the rows across the cluster (default true)"}}},
          {"partition_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 4096},
            {"description",
            "partitions to write; omit to let the partition scheduler "
            "size them (a target body size, with a floor of partitions per "
            "node) instead of fixing a count"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "owners per partition (default 1 — a connector result set is a "
             "cache, re-pullable from its source; promote_dataset raises it)"}}}}}};
    return schema;
}

const nlohmann::json& create_dataset_from_query_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"database", "table", "rows", "columns", "partitions"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"rows", {{"type", "integer"}}},
          {"columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"partitions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"partition_id", "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"owner_nodes",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}}}}}}}}}},
          {"coordinated",
           {{"type", "boolean"},
            {"description",
             "true when the fetch was co-launched across the cluster (every "
             "participant pulled its own tile straight from the source)"}}},
          {"participants",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "the nodes that fetched a tile"}}}}}};
    return schema;
}

// Resolve the connector this call should use: either a saved connection by name
// (the credential stays on the cluster) or an inline connector object. Exactly
// one; `reason` explains the rejection.
std::optional<zetta::connect::ConnectorConfig> resolve_query_connector(
    const nlohmann::json& input, ApiContext& context, std::string& reason) {
    const bool has_connection = input.contains("connection");
    const bool has_connector = input.contains("connector");
    if (has_connection == has_connector) {
        reason = has_connection
                     ? "give either a saved 'connection' name or an inline "
                       "'connector' object, not both"
                     : "give either a saved 'connection' name or an inline "
                       "'connector' object";
        return std::nullopt;
    }
    if (has_connection) {
        const std::string name = input.at("connection").get<std::string>();
        const std::optional<zetta::connect::SavedConnection> saved =
            zetta::connect::find_connection(context.node, name);
        if (!saved) {
            reason = "connection: no saved connection named '" + name +
                     "' (list them with list_connections)";
            return std::nullopt;
        }
        return saved->config;
    }
    std::optional<zetta::connect::ConnectorConfig> config =
        zetta::connect::ConnectorConfig::from_json(input.at("connector"));
    if (!config) {
        reason =
            "connector: expected {type, account, token} with a known type "
            "(and an optional auth of pat|oauth, database, schema, warehouse, "
            "role)";
    }
    return config;
}

ApiResult handle_create_dataset_from_query(const nlohmann::json& input,
                                           ApiContext& context) {
    std::string reason;
    const std::optional<zetta::connect::ConnectorConfig> config =
        resolve_query_connector(input, context, reason);
    if (!config) {
        return ApiResult::failure("invalid_input", reason);
    }

    zetta::connect::DistributedIngestOptions options;
    // The SAVED CONNECTION name, when that is how the caller identified the
    // source. It is what makes the cross-node co-launch possible: every node
    // resolves the credential from its own replicated copy, so nothing secret
    // rides the launch. An inline `connector` block leaves it empty, which
    // degrades the ingest to this node alone.
    options.connection = input.value("connection", std::string{});
    options.query = input.at("query").get<std::string>();
    options.max_rows = input.value("max_rows", static_cast<std::size_t>(100000));
    options.database = input.value("database", options.database);
    options.table_name = input.value("table_name", options.table_name);
    // Absent means AUTO — the scheduler targets a partition size instead of a
    // constant (#833). A value present is a manual override, honoured exactly.
    if (const auto it = input.find("partition_count"); it != input.end()) {
        options.partition_count = it->get<std::uint32_t>();
    }
    // A connector result set is a CACHE — re-pullable from its source and cheap
    // to lose — so it is RF=1 by default, explicitly, against the engine's
    // durable default of 3 (#833 section 8). promote_dataset upgrades one that
    // turns out to be worth keeping.
    options.replication_factor = input.value("replication_factor", std::size_t{1});

    // Reaches the external source over the network (consent-gated above), from
    // every participating node at once (#833 section 7).
    const zetta::connect::DistributedIngestResult ingested =
        zetta::connect::run_distributed_connector_ingest(context.node, *config,
                                                         options);
    if (!ingested.ok()) {
        return ApiResult::failure("engine_error", ingested.error);
    }
    if (ingested.rows == 0) {
        return ApiResult::failure(
            "engine_error",
            "the query returned no rows — nothing to load as a dataset");
    }

    nlohmann::json partitions = nlohmann::json::array();
    for (const auto& partition : ingested.partitions) {
        if (!partition) continue;
        partitions.push_back({{"partition_id", partition->id()},
                              {"owner_nodes", partition->owner_nodes()}});
    }
    nlohmann::json output;
    output["database"] =
        ingested.database ? ingested.database->name() : options.database;
    output["table"] = ingested.table ? ingested.table->name() : "";
    output["rows"] = ingested.rows;
    output["columns"] = ingested.columns;
    output["partitions"] = std::move(partitions);
    output["coordinated"] = ingested.coordinated;
    output["participants"] = ingested.participants;
    return ApiResult::success(std::move(output));
}

// --- promote_dataset ----------------------------------------------------------------

const nlohmann::json& promote_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"dataset"}},
        {"additionalProperties", false},
        {"properties",
         {{"dataset",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the dataset name (= the table name)"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the owning database (default datasets) — required when the same "
             "dataset name exists in several databases"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "target owners per partition (default 3), clamped to the node "
             "count; a partition that already has at least this many owners is "
             "left alone — promotion never removes one"}}}}}};
    return schema;
}

const nlohmann::json& promote_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"database", "table", "replication_factor", "partitions", "promoted"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"table_id", {{"type", "string"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"description", "the factor actually applied (clamped to the roster)"}}},
          {"partitions", {{"type", "integer"}}},
          {"promoted",
           {{"type", "integer"},
            {"description", "partitions whose owner set grew"}}},
          {"promotions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"partition_id", "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"partition", {{"type", "integer"}}},
                {"previous_owner_nodes",
                 {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"owner_nodes",
                 {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}}}}}};
    return schema;
}

ApiResult handle_promote_dataset(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("dataset").get<std::string>();
    const std::string requested_database = input.value("database", std::string{});
    // Resolve the dataset the same way get_dataset does: by name, with the
    // database required only when the name is ambiguous.
    std::vector<std::string> databases;
    for (const zetta::data::DatasetSummary& dataset : context.node.datasets()) {
        if (dataset.name != name) continue;
        if (!requested_database.empty() && dataset.database != requested_database) {
            continue;
        }
        databases.push_back(dataset.database);
    }
    if (databases.empty()) {
        return ApiResult::failure(
            "not_found",
            "no dataset named '" + name + "'" +
                (requested_database.empty()
                     ? ""
                     : " in database '" + requested_database + "'"));
    }
    if (databases.size() > 1) {
        std::string names;
        for (const std::string& database : databases) {
            if (!names.empty()) names += ", ";
            names += database;
        }
        return ApiResult::failure(
            "engine_error", "dataset '" + name + "' exists in several databases (" +
                                names + ") — pass \"database\" to pick one");
    }

    const std::size_t replication_factor = input.value(
        "replication_factor",
        static_cast<std::size_t>(zetta::data::kDefaultReplicationFactor));
    const zetta::data::PromoteDatasetResult promoted = zetta::data::promote_dataset(
        context.node, databases.front(), name, replication_factor,
        context.node.cluster_members());
    if (!promoted.ok()) {
        return ApiResult::failure("engine_error", promoted.error);
    }

    nlohmann::json promotions = nlohmann::json::array();
    for (const zetta::data::PartitionPromotion& promotion : promoted.promotions) {
        promotions.push_back({{"partition_id", promotion.partition_uuid},
                              {"partition", promotion.partition},
                              {"previous_owner_nodes", promotion.from},
                              {"owner_nodes", promotion.to}});
    }
    nlohmann::json output;
    output["database"] = promoted.database;
    output["table"] = promoted.table;
    output["table_id"] = promoted.table_id;
    output["replication_factor"] = promoted.replication_factor;
    output["partitions"] = promoted.partitions;
    output["promoted"] = promoted.promoted;
    output["promotions"] = std::move(promotions);
    return ApiResult::success(std::move(output));
}

// --- testsuite_run (kind: job) ------------------------------------------------------

const nlohmann::json& testsuite_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"case"}},
        {"additionalProperties", false},
        {"properties",
         {{"case",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "a test-suite case id (testsuite_list / "
                            "system.test_cases / the Test suite panel list "
                            "them)"}}},
          {"options",
           {{"type", "object"},
            {"description", "per-case option overrides keyed by option key; "
                            "values typed per the case's declaration"}}},
          {"custom_data_path",
           {{"type", "string"},
            {"description", "a churn dataset directory for cases that accept "
                            "one (default: the built-in synthetic dataset)"}}}}}};
    return schema;
}

const nlohmann::json& testsuite_run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "case", "status", "poll"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"description",
             "the durable engine handle — poll get_run / system.runs"}}},
          {"case", {{"type", "string"}}},
          {"status", {{"type", "string"}}},
          {"poll", {{"type", "string"}}}}}};
    return schema;
}

// Apply the caller's option overrides onto a case's declared defaults,
// validating each key + value type against the declaration — the same
// path-qualified rejection discipline as the schema boundary.
std::optional<ApiResult> apply_case_options(
    const zetta::testkit::TestCase& test_case, const nlohmann::json& overrides,
    zetta::testkit::TestOptions& options) {
    for (const auto& [key, value] : overrides.items()) {
        const zetta::testkit::TestOption* declared = nullptr;
        for (const zetta::testkit::TestOption& option : test_case.options) {
            if (option.key == key) {
                declared = &option;
                break;
            }
        }
        if (!declared) {
            return ApiResult::failure(
                "invalid_input",
                "'" + key + "' is not an option of case '" + test_case.id + "'",
                "/options/" + key);
        }
        const std::string pointer = "/options/" + key;
        switch (declared->type) {
            case zetta::testkit::OptionType::Bool:
                if (!value.is_boolean()) {
                    return ApiResult::failure("invalid_input",
                                              "expected a boolean", pointer);
                }
                options.set_bool(key, value.get<bool>());
                break;
            case zetta::testkit::OptionType::Int:
                if (!value.is_number_integer()) {
                    return ApiResult::failure("invalid_input",
                                              "expected an integer", pointer);
                }
                options.set_int(key, value.get<long>());
                break;
            case zetta::testkit::OptionType::Double:
                if (!value.is_number()) {
                    return ApiResult::failure("invalid_input",
                                              "expected a number", pointer);
                }
                options.set_double(key, value.get<double>());
                break;
            case zetta::testkit::OptionType::Choice: {
                int index = -1;
                if (value.is_number_integer()) {
                    index = value.get<int>();
                } else if (value.is_string()) {
                    const std::string label = value.get<std::string>();
                    for (std::size_t i = 0; i < declared->choices.size(); ++i) {
                        if (declared->choices[i] == label) {
                            index = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (index < 0 ||
                    index >= static_cast<int>(declared->choices.size())) {
                    return ApiResult::failure(
                        "invalid_input",
                        "expected one of the declared choices (by index or "
                        "label)",
                        pointer);
                }
                options.set_choice(key, index);
                break;
            }
            case zetta::testkit::OptionType::Text:
                if (!value.is_string()) {
                    return ApiResult::failure("invalid_input",
                                              "expected a string", pointer);
                }
                options.set_text(key, value.get<std::string>());
                break;
        }
    }
    return std::nullopt;
}

ApiResult handle_testsuite_run(const nlohmann::json& input, ApiContext& context) {
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "testsuite_run is a job capability and this embedding wired no "
            "background action owner");
    }
    const std::string case_id = input.at("case").get<std::string>();
    const zetta::testkit::TestCase* test_case =
        zetta::testkit::TestSuite::builtin().find(case_id);
    if (!test_case) {
        return ApiResult::failure(
            "not_found", "no test-suite case with id '" + case_id + "'");
    }
    zetta::testkit::TestOptions options = test_case->defaults();
    if (const auto overrides = input.find("options");
        overrides != input.end()) {
        if (auto rejected = apply_case_options(*test_case, *overrides, options)) {
            return *rejected;
        }
    }
    auto handle = zetta::testkit::AsyncCaseRun::start(
        context.node, *test_case, std::move(options),
        input.value("custom_data_path", ""), context.recorder, context.trace);
    if (!handle) {
        return ApiResult::failure("engine_error",
                                  "the run record could not be created");
    }
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });
    nlohmann::json output;
    output["run_id"] = handle->run_id();
    output["case"] = test_case->id;
    output["status"] = "running";
    output["poll"] = "get_run for status; the case's log lines land with the "
                     "terminal status";
    return ApiResult::success(std::move(output));
}

// --- testsuite_list (read) ----------------------------------------------------
//
// The whole in-app test-suite catalog (zetta::testkit::TestSuite::builtin()) as
// a structured listing — the same cases the headed Test suite panel renders and
// system.test_cases serves — so an agent can discover every case id + its
// tunable options before feeding one to testsuite_run.

const nlohmann::json& testsuite_list_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

std::string_view option_type_name(zetta::testkit::OptionType type) {
    switch (type) {
        case zetta::testkit::OptionType::Bool:
            return "bool";
        case zetta::testkit::OptionType::Int:
            return "int";
        case zetta::testkit::OptionType::Double:
            return "double";
        case zetta::testkit::OptionType::Choice:
            return "choice";
        case zetta::testkit::OptionType::Text:
            return "text";
    }
    return "text";
}

const nlohmann::json& testsuite_list_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"tests"}},
        {"additionalProperties", false},
        {"properties",
         {{"tests",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "category", "name", "description", "options"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"category", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"description", {{"type", "string"}}},
                {"options",
                 {{"type", "array"},
                  {"items",
                   {{"type", "object"},
                    {"required", {"key", "label", "type"}},
                    {"additionalProperties", false},
                    {"properties",
                     {{"key", {{"type", "string"}}},
                      {"label", {{"type", "string"}}},
                      {"type", {{"type", "string"}}}}}}}}}}}}}}}}}};
    return schema;
}

ApiResult handle_testsuite_list(const nlohmann::json&, ApiContext&) {
    nlohmann::json tests = nlohmann::json::array();
    for (const zetta::testkit::TestCase& test_case :
         zetta::testkit::TestSuite::builtin().cases()) {
        nlohmann::json options = nlohmann::json::array();
        for (const zetta::testkit::TestOption& option : test_case.options) {
            options.push_back(
                {{"key", option.key},
                 {"label", option.label},
                 {"type", std::string(option_type_name(option.type))}});
        }
        tests.push_back(
            {{"id", test_case.id},
             {"category",
              std::string(zetta::testkit::to_string(test_case.category))},
             {"name", test_case.name},
             {"description", test_case.description},
             {"options", std::move(options)}});
    }
    nlohmann::json output;
    output["tests"] = std::move(tests);
    return ApiResult::success(std::move(output));
}

// --- list_specs / get_spec (#646) --------------------------------------------

// The spec index: the OpenAPI document + every capability input/output schema
// + every boundary schema, rendered by specs.cpp from the registry and
// zetta::schemas::boundary_schemas() — served from the C++ accessors, never
// from the generated schemas/*.json artifacts.

const nlohmann::json& list_specs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_specs_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"count", "specs"}},
        {"additionalProperties", false},
        {"properties",
         {{"count", {{"type", "integer"}}},
          {"specs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"name", "kind", "href"}},
              {"additionalProperties", false},
              {"properties",
               {{"name",
                 {{"type", "string"},
                  {"description", "pass to get_spec for the document"}}},
                {"kind",
                 {{"type", "string"},
                  {"enum", {"openapi", "capability_input", "capability_output",
                            "boundary"}}}},
                {"href",
                 {{"type", "string"},
                  {"description", "GET this route for the raw document"}}},
                {"capability", {{"type", "string"}}},
                {"file",
                 {{"type", "string"},
                  {"description",
                   "the generated schemas/<file> artifact this accessor "
                   "keeps honest"}}},
                {"description", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

const nlohmann::json& get_spec_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a spec name from list_specs: 'openapi', '<capability>.input', "
             "'<capability>.output', or a boundary schema name like 'run'"}}}}}};
    return schema;
}

const nlohmann::json& get_spec_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "kind", "schema"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"kind",
           {{"type", "string"},
            {"enum", {"openapi", "capability_input", "capability_output",
                      "boundary"}}}},
          {"capability", {{"type", "string"}}},
          {"file", {{"type", "string"}}},
          {"description", {{"type", "string"}}},
          {"schema",
           {{"type", "object"},
            {"description",
             "the document itself — draft-07 JSON Schema, or the OpenAPI 3.1 "
             "document for 'openapi'"}}}}}};
    return schema;
}

// --- agents: CRUD for the served, selectable agent entity (#849) --------------

const nlohmann::json& agent_fields_schema() {
    // The agent record shape — reused as the create/get/update output schema.
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"description", {{"type", "string"}}},
          {"model", {{"type", "string"}}},
          {"system_prompt", {{"type", "string"}}},
          {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"created_at", {{"type", "string"}}},
          {"updated_at", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& create_agent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "model"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1}}},
          {"description", {{"type", "string"}}},
          {"model",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "checkpoint dir, model:<uuid>, or a HuggingFace repo id"}}},
          {"system_prompt", {{"type", "string"}}},
          {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          // Goals are editable after creation: what an agent is rewarded for is the thing most
          // likely to change as a deployment learns what it wants. Editing them moves the
          // environment fingerprint and marks every existing Saved Version stale — the intended
          // consequence, reported by agent_training_status.
          {"goals", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
    return schema;
}

const nlohmann::json& update_agent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}, {"minLength", 1}}},
          {"name", {{"type", "string"}, {"minLength", 1}}},
          {"description", {{"type", "string"}}},
          {"model", {{"type", "string"}, {"minLength", 1}}},
          {"system_prompt", {{"type", "string"}}},
          {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          // Goals are editable after creation: what an agent is rewarded for is the thing most
          // likely to change as a deployment learns what it wants. Editing them moves the
          // environment fingerprint and marks every existing Saved Version stale — the intended
          // consequence, reported by agent_training_status.
          {"goals", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
    return schema;
}

const nlohmann::json& agent_id_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id"}},
        {"additionalProperties", false},
        {"properties", {{"id", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& list_agents_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_agents_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"agents"}},
        {"properties",
         {{"agents", {{"type", "array"}, {"items", agent_fields_schema()}}}}}};
    return schema;
}

const nlohmann::json& delete_agent_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"removed"}},
        {"properties",
         {{"removed", {{"type", "boolean"}}}, {"id", {{"type", "string"}}}}}};
    return schema;
}

// Apply the provided fields of a create/update input onto `base` (a fresh record on
// create, the existing one on update). Only present fields overwrite.
zetta::agent::AgentRecord agent_from_input(const nlohmann::json& input,
                                           zetta::agent::AgentRecord base) {
    if (input.contains("name")) base.name = input.at("name").get<std::string>();
    if (input.contains("description")) {
        base.description = input.at("description").get<std::string>();
    }
    if (input.contains("model")) base.model = input.at("model").get<std::string>();
    if (input.contains("system_prompt")) {
        base.system_prompt = input.at("system_prompt").get<std::string>();
    }
    if (input.contains("tools") && input.at("tools").is_array()) {
        base.tools.clear();
        for (const nlohmann::json& tool : input.at("tools")) {
            if (tool.is_string()) base.tools.push_back(tool.get<std::string>());
        }
    }
    if (input.contains("goals") && input.at("goals").is_array()) {
        base.goals.clear();
        for (const nlohmann::json& entry : input.at("goals")) {
            if (std::optional<zetta::agent::Goal> goal = zetta::agent::goal_from_json(entry)) {
                base.goals.push_back(*std::move(goal));
            }
        }
        // Resolve to checks here, so the stored record says what it actually trains on rather
        // than leaving it to be inferred at reward time.
        zetta::agent::interpret_goals(base.goals);
    }
    return base;
}

ApiResult handle_create_agent(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    zetta::agent::AgentRecord agent;
    agent.id = zetta::util::new_uuid();
    agent.created_at = zetta::util::now_iso8601();
    agent.updated_at = agent.created_at;
    agent = agent_from_input(input, std::move(agent));

    zetta::agent::AgentStore store(*context.node.replication());
    std::string error;
    const std::optional<zetta::agent::AgentRecord> stored = store.put(agent, &error);
    if (!stored) return ApiResult::failure("invalid_argument", error);
    return ApiResult::success(zetta::agent::to_json(*stored));
}

ApiResult handle_list_agents(const nlohmann::json&, ApiContext& context) {
    nlohmann::json agents = nlohmann::json::array();
    if (context.node.replication() != nullptr) {
        zetta::agent::AgentStore store(*context.node.replication());
        for (const zetta::agent::AgentRecord& agent : store.list()) {
            agents.push_back(zetta::agent::to_json(agent));
        }
    }
    nlohmann::json output;
    output["agents"] = std::move(agents);
    return ApiResult::success(std::move(output));
}

ApiResult handle_get_agent(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no agents on a standalone node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + id + "'");
    return ApiResult::success(zetta::agent::to_json(*agent));
}

const nlohmann::json& training_schedule_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["required"] = nlohmann::json::array({"id", "training"});
        document["additionalProperties"] = false;
        document["properties"]["id"] = {{"type", "string"}, {"minLength", 1},
                                        {"description", "the agent's id"}};
        document["properties"]["training"] = zetta::agent::training_spec_schema();
        return document;
    }();
    return schema;
}

const nlohmann::json& training_status_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["properties"]["stale"] = {{"type", "boolean"}};
        document["properties"]["due"] = {{"type", "boolean"}};
        document["properties"]["reason"] = {{"type", "string"}};
        document["properties"]["versions"] = {{"type", "array"}};
        return document;
    }();
    return schema;
}

ApiResult handle_update_agent(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> existing = store.get(id);
    if (!existing) return ApiResult::failure("not_found", "no agent '" + id + "'");
    zetta::agent::AgentRecord updated = agent_from_input(input, *existing);
    updated.updated_at = zetta::util::now_iso8601();
    std::string error;
    const std::optional<zetta::agent::AgentRecord> stored = store.put(updated, &error);
    if (!stored) return ApiResult::failure("invalid_argument", error);
    return ApiResult::success(zetta::agent::to_json(*stored));
}

// Whether an agent should be retrained, and why (#941). Read-only: the answer a console shows
// next to a Retrain button, and the same answer a scheduler acts on — one source, so the two
// cannot disagree about the same agent.
ApiResult handle_agent_training_status(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + id + "'");

    const zetta::agent::RetrainStatus status = zetta::agent::retrain_status(
        agent->training, agent->versions, agent->tools, agent->goals, zetta::util::now_iso8601());

    nlohmann::json versions = nlohmann::json::array();
    for (const zetta::agent::AgentVersion& version : agent->versions) {
        versions.push_back(zetta::agent::to_json(version));
    }
    // Goals nobody can verify are reported here too — a goal that resolves to no check is one
    // training silently drops, and the person who wrote it should be told rather than assume.
    return ApiResult::success(
        nlohmann::json{{"agent", agent->id},
                       {"trained", status.trained},
                       {"stale", status.stale},
                       {"due", status.due},
                       {"reason", status.reason},
                       {"fingerprint", status.fingerprint},
                       {"training", zetta::agent::to_json(agent->training)},
                       {"versions", std::move(versions)},
                       {"unverifiable_goals", zetta::agent::unmapped_goals(agent->goals)}});
}

// Set how and when an agent retrains (#941). Separate from update_agent because changing a
// SCHEDULE is not the same act as changing what the agent can do — one is operational, the other
// changes the environment and makes every existing version stale.
ApiResult handle_set_training_schedule(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> existing = store.get(id);
    if (!existing) return ApiResult::failure("not_found", "no agent '" + id + "'");

    std::string reason;
    const std::optional<zetta::agent::TrainingSpec> spec =
        zetta::agent::training_spec_from_json(input.at("training"), &reason);
    if (!spec) return ApiResult::failure("invalid_argument", reason);

    zetta::agent::AgentRecord updated = *existing;
    updated.training = *spec;
    updated.updated_at = zetta::util::now_iso8601();
    std::string error;
    const std::optional<zetta::agent::AgentRecord> stored = store.put(updated, &error);
    if (!stored) return ApiResult::failure("invalid_argument", error);
    return ApiResult::success(zetta::agent::to_json(*stored));
}

ApiResult handle_delete_agent(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("id").get<std::string>();
    bool removed = false;
    if (context.node.replication() != nullptr) {
        zetta::agent::AgentStore store(*context.node.replication());
        removed = store.remove(id);
    }
    nlohmann::json output;
    output["removed"] = removed;
    output["id"] = id;
    return ApiResult::success(std::move(output));
}

// --- workspace profiles (#886, the create-a-data-profile pathway) ------------

const nlohmann::json& get_workspace_profile_schema() {
    static const nlohmann::json schema = id_input_schema(
        "workspace_profile_id",
        "the profile's uuid (system.workspace_profiles.workspace_profile_id)");
    return schema;
}

const nlohmann::json& workspace_profile_output_schema() {
    static const nlohmann::json schema = entity_value_with_id_schema(
        zetta::workspace::workspace_profile_value_schema());
    return schema;
}

const nlohmann::json& create_workspace_profile_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"role",
           {{"type", "string"}, {"description", "the user's workspace role"}}},
          {"daily_work",
           {{"type", "string"},
            {"description", "plain-language daily-work description"}}},
          {"sources",
           {{"type", "array"}, {"items", {{"type", "object"}}},
            {"description", "approved source refs, each {id,name,kind}"}}}}}};
    return schema;
}

const nlohmann::json& update_workspace_profile_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"workspace_profile_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"workspace_profile_id",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the profile's uuid to update"}}},
          {"role", {{"type", "string"}}},
          {"daily_work", {{"type", "string"}}},
          {"sources", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
    return schema;
}

ApiResult handle_get_workspace_profile(const nlohmann::json& input,
                                       ApiContext& context) {
    const std::string id = input.at("workspace_profile_id").get<std::string>();
    const auto profile = context.node.find_workspace_profile(id);
    if (!profile) {
        return ApiResult::failure("not_found",
                                  "no workspace profile with id '" + id + "'");
    }
    nlohmann::json output = profile->metadata();
    output["id"] = profile->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_create_workspace_profile(const nlohmann::json& input,
                                          ApiContext& context) {
    // The profile belongs to the calling principal (server-set, not client-set).
    zetta::workspace::WorkspaceProfileValue value;
    value.owner = actor_of(context);
    value.role = input.value("role", std::string{});
    value.daily_work = input.value("daily_work", std::string{});
    if (const auto it = input.find("sources"); it != input.end()) {
        value.sources = *it;
    }
    const auto profile = context.node.create_workspace_profile(std::move(value));
    if (!profile) {
        return ApiResult::failure("engine_error",
                                  "could not create the workspace profile");
    }
    nlohmann::json output = profile->metadata();
    output["id"] = profile->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_update_workspace_profile(const nlohmann::json& input,
                                          ApiContext& context) {
    const std::string id = input.at("workspace_profile_id").get<std::string>();
    const auto profile = context.node.find_workspace_profile(id);
    if (!profile) {
        return ApiResult::failure("not_found",
                                  "no workspace profile with id '" + id + "'");
    }
    // Apply present fields, defaulting to the profile's current values; the owner
    // and created stamp stay fixed.
    const std::string role = input.value("role", profile->role());
    const std::string daily_work = input.value("daily_work", profile->daily_work());
    nlohmann::json sources = profile->sources();
    if (const auto it = input.find("sources"); it != input.end()) sources = *it;
    profile->set_content(role, daily_work, std::move(sources),
                         zetta::util::now_iso8601());
    nlohmann::json output = profile->metadata();
    output["id"] = profile->id();
    return ApiResult::success(std::move(output));
}

// --- plans (Conflict D: metered usage + plan) --------------------------------

const nlohmann::json& get_plan_schema() {
    static const nlohmann::json schema =
        id_input_schema("plan_id", "the plan's uuid (system.plans.plan_id)");
    return schema;
}

const nlohmann::json& plan_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::billing::plan_value_schema());
    return schema;
}

const nlohmann::json& create_plan_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1},
                    {"description", "the plan's display name"}}},
          {"tier", {{"type", "string"},
                    {"description", "display tier (free / pro / enterprise)"}}},
          {"quotas", {{"type", "object"},
                      {"additionalProperties", {{"type", "number"}}},
                      {"description", "numeric limits, e.g. {max_runs, max_storage_bytes}"}}},
          {"period_start", {{"type", "string"},
                            {"description", "ISO-8601 start of the current period"}}}}}};
    return schema;
}

const nlohmann::json& update_plan_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"plan_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"plan_id", {{"type", "string"}, {"minLength", 1},
                       {"description", "the plan's uuid to update"}}},
          {"name", {{"type", "string"}, {"minLength", 1}}},
          {"tier", {{"type", "string"}}},
          {"quotas", {{"type", "object"}, {"additionalProperties", {{"type", "number"}}}}},
          {"period_start", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_get_plan(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("plan_id").get<std::string>();
    const auto plan = context.node.find_plan(id);
    if (!plan) {
        return ApiResult::failure("not_found", "no plan with id '" + id + "'");
    }
    nlohmann::json output = plan->metadata();
    output["id"] = plan->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_create_plan(const nlohmann::json& input, ApiContext& context) {
    zetta::billing::PlanValue value;
    value.name = input.at("name").get<std::string>();
    value.tier = input.value("tier", std::string{});
    if (const auto it = input.find("quotas"); it != input.end()) value.quotas = *it;
    value.period_start = input.value("period_start", std::string{});
    const auto plan = context.node.create_plan(std::move(value));
    if (!plan) {
        return ApiResult::failure("engine_error", "could not create the plan");
    }
    nlohmann::json output = plan->metadata();
    output["id"] = plan->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_update_plan(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("plan_id").get<std::string>();
    const auto plan = context.node.find_plan(id);
    if (!plan) {
        return ApiResult::failure("not_found", "no plan with id '" + id + "'");
    }
    const std::string name = input.value("name", plan->name());
    const std::string tier = input.value("tier", plan->tier());
    nlohmann::json quotas = plan->quotas();
    if (const auto it = input.find("quotas"); it != input.end()) quotas = *it;
    const std::string period_start = input.value("period_start", plan->period_start());
    plan->set_content(name, tier, std::move(quotas), period_start,
                      zetta::util::now_iso8601());
    nlohmann::json output = plan->metadata();
    output["id"] = plan->id();
    return ApiResult::success(std::move(output));
}

// --- chat: talk to an agent, with tool-calling (#849) -------------------------

const nlohmann::json& chat_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"agent", "message"}},
        {"additionalProperties", false},
        {"properties",
         {{"agent", {{"type", "string"}, {"minLength", 1}, {"description", "the agent's id"}}},
          {"history",
             {{"type", "array"},
              {"description",
               "the conversation so far, oldest first: [{ask, reply, tools:[{tool,status,detail}]}]. "
               "The tool outcomes matter — a follow-up like \"why did that fail?\" refers to what "
               "ran, not to the prose."},
              {"items", {{"type", "object"}}}}},
            {"message", {{"type", "string"}, {"minLength", 1}}},
          {"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "scope the answer to this project (name or uuid): its trained model's "
             "measure and ranked drivers are given to the agent, so it answers from "
             "what the pipeline actually produced instead of from priors (#914)"}}},
          {"max_new_tokens", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}}},
          {"max_rounds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}}}}};
    return schema;
}

const nlohmann::json& chat_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"reply", "tool_calls", "rounds", "grounded", "kind"}},
        {"properties",
         {{"reply", {{"type", "string"}}},
          {"rounds", {{"type", "integer"}}},
          {"grounded_project",
           {{"type", "string"},
            {"description",
             "the Project this reply was grounded in. Present whenever grounding "
             "happened — including when the engine chose it, because the caller "
             "named none and the most recent successful run's Project was used"}}},
          {"grounded",
           {{"type", "boolean"},
            {"description",
             "true only when a project was named AND it has a trained model whose real "
             "measure + ranked drivers were given to the agent. False means the reply "
             "rests on the model's priors — present it as such, never as a measured "
             "result (#914)"}}},
          {"grounding_warning",
           {{"type", "string"},
            {"description", "why the reply is ungrounded, when it is"}}},
          {"kind",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"action", "answer", "clarify", "abstain",
                                            "unparsed"})},
            {"description",
             "the mode the agent declared for this turn: answer renders as prose, clarify is a "
             "question awaiting a reply, abstain is a refusal to present as such, action carries "
             "a proposed Pipeline. `unparsed` means the generation was not a valid "
             "envelope - `reply` then carries an honest failure sentence and the raw "
             "generation moves to `raw`, so it never reaches a chat window (#979)"}}},
          {"raw",
           {{"type", "string"},
            {"description",
             "the raw generation, present only when `kind` is `unparsed`. For "
             "developers and traces - never render it to a user"}}},
          {"pipeline",
           {{"type", "object"},
            {"description", "the proposed Pipeline, when the turn built one"}}},
          {"tool_calls",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"tool", {{"type", "string"}}},
                {"status", {{"type", "string"}}},
                {"detail", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

// Render a trained model's real numbers as the plain lines the agent reads before
// answering (#914): the measure it scored and its drivers, ranked, strongest first.
// Drivers with zero importance are dropped — the model did not use them, and listing
// them invites the agent to talk about features that contributed nothing.
std::string model_grounding_text(const nlohmann::json& metadata, const std::string& model_name,
                                 const std::string& project_name) {
    std::string text = "Project: " + project_name + "\nTrained model: " + model_name;
    if (const auto kind = metadata.find("kind");
        kind != metadata.end() && kind->is_string()) {
        text += " (" + kind->get<std::string>() + ")";
    }
    text += "\n";

    if (const auto metrics = metadata.find("metrics");
        metrics != metadata.end() && metrics->is_object()) {
        const auto number = [&](const char* key) -> std::string {
            const auto found = metrics->find(key);
            if (found == metrics->end() || !found->is_number()) return {};
            std::ostringstream out;
            out << std::fixed << std::setprecision(3) << found->get<double>();
            return out.str();
        };
        const std::string auc = number("auc");
        const std::string accuracy = number("accuracy");
        if (!auc.empty()) text += "Measure: AUC " + auc;
        if (!accuracy.empty()) text += (auc.empty() ? "Measure: " : ", ") + std::string("accuracy ") + accuracy;
        if (!auc.empty() || !accuracy.empty()) text += "\n";
    }

    const auto schema = metadata.find("feature_schema");
    if (schema != metadata.end() && schema->is_object()) {
        const auto drivers = schema->find("drivers");
        if (drivers != schema->end() && drivers->is_array()) {
            std::vector<std::pair<std::string, double>> ranked;
            for (const nlohmann::json& driver : *drivers) {
                if (!driver.is_object()) continue;
                const auto feature = driver.find("feature");
                const auto importance = driver.find("importance");
                if (feature == driver.end() || !feature->is_string()) continue;
                const double weight =
                    (importance != driver.end() && importance->is_number())
                        ? importance->get<double>() : 0.0;
                if (weight <= 0.0) continue;  // the model did not use it
                ranked.emplace_back(feature->get<std::string>(), weight);
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& left, const auto& right) { return left.second > right.second; });
            if (ranked.empty()) {
                text += "Drivers: the model reported no feature with non-zero importance.\n";
            } else {
                // Present importances as a SHARE OF TOTAL importance, which is both a
                // standard way to report feature importance and — crucially — a number
                // the agent's instinct to write "%" is correct about. Telling it "these
                // are not percentages" did not work: it wrote "failed_payments (26.4%)"
                // anyway, turning a raw weight into a false claim. Giving it real
                // percentages makes the sentence it wants to write a true one.
                double total = 0.0;
                for (const auto& [feature, weight] : ranked) total += weight;
                text +=
                    "Drivers, strongest first — each shown as its share of the model's "
                    "total feature importance (they sum to 100%):\n";
                for (const auto& [feature, weight] : ranked) {
                    std::ostringstream line;
                    line << "- " << feature << ": " << std::fixed << std::setprecision(1)
                         << (total > 0.0 ? (weight / total) * 100.0 : 0.0)
                         << "% of the model's importance\n";
                    text += line.str();
                }
            }
        }
    }
    return text;
}

// The ranked at-risk list this Project's pipeline published (#915), as lines the
// agent reads before answering "which accounts are most at risk". Matched to the
// Project's OWN pipelines by table name — never a "newest table on the node" guess,
// which would hand one Project another's accounts. Capped: the question is about the
// top of the list, and a long tail only crowds the context out.
std::string at_risk_grounding_text(Node& node,
                                   const std::vector<std::shared_ptr<zetta::pipeline::ProjectGraph>>& graphs) {
    constexpr std::size_t kMaxRows = 10;
    const std::shared_ptr<zetta::data::Database> database = node.find_database("churn_results");
    if (!database) return {};

    std::shared_ptr<zetta::data::Table> table;
    for (const auto& graph : graphs) {
        if (!graph || graph->id().size() < 8) continue;
        if (auto found = database->find_table("at_risk_" + graph->id().substr(0, 8))) {
            table = std::move(found);  // last writer wins: the newest run of this Project
        }
    }
    if (!table) return {};

    std::vector<nlohmann::json> rows;
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const auto row = store.at(index);
        if (!row) continue;  // row.error() says why; a hole is not an answer
        rows.push_back(*row);
    }
    if (rows.empty()) return {};
    std::sort(rows.begin(), rows.end(), [](const nlohmann::json& left, const nlohmann::json& right) {
        return left.value("rank", 0.0) < right.value("rank", 0.0);
    });

    std::string text =
        "\nThe accounts this pipeline ranked most at risk. These are the ONLY accounts "
        "you may name — do not invent others — and risk is the model's calibrated "
        "probability of churn:\n";
    std::size_t shown = 0;
    for (const nlohmann::json& row : rows) {
        if (shown++ >= kMaxRows) break;
        std::ostringstream line;
        line << "- account " << row.value("account", 0) << ": risk " << std::fixed
             << std::setprecision(3) << row.value("risk", 0.0);
        if (const std::string driver = row.value("dominant_driver", std::string{});
            !driver.empty()) {
            line << ", driven by " << driver;
        }
        line << "\n";
        text += line.str();
    }
    if (rows.size() > kMaxRows) {
        text += "(and " + std::to_string(rows.size() - kMaxRows) + " more below the top " +
                std::to_string(kMaxRows) + ")\n";
    }
    return text;
}

ApiResult handle_chat(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no agents on a standalone node");
    }
    const std::string agent_id = input.at("agent").get<std::string>();
    const std::string message = input.at("message").get<std::string>();
    const int max_new_tokens = input.value("max_new_tokens", 256);
    const int max_rounds = input.value("max_rounds", 3);

    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(agent_id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + agent_id + "'");

    // The tools the agent may call while answering (#904): READ-ONLY introspection,
    // an explicit allowlist — never a mutating capability, so a chat can describe
    // the node but never change it. Each call goes back through
    // `registry.call(...)` with the SAME ApiContext, so the caller's own
    // permissions and the audit trail apply exactly as if they had made the call.
    static const std::vector<zetta::agent::AgentToolSpec> kAgentTools = {
        {"list_projects", "list the Projects on this node"},
        {"get_project", "read one Project: {\"name\": \"<project>\"}"},
        {"list_datasets", "list the datasets (tables) available, with row and column counts"},
        {"get_dataset", "read one dataset's schema: {\"name\": \"<dataset>\"}"},
        {"list_models", "list the trained models and their measures"},
        {"get_model",
         "read one model: its measures, its feature columns, and its ranked churn "
         "drivers: {\"model\": \"<id or name>\"}"},
        {"score",
         "run a trained model and get accounts ranked by risk plus what drives it — "
         "over a whole dataset: {\"model\": \"<id>\", \"dataset_path\": "
         "\"<database.table>\", \"top_k\": 10}, or over an account that is not in "
         "the data: {\"model\": \"<id>\", \"rows\": [{...}]}"},
        {"propose_improvement",
         "propose a change to a model and state its predicted effect, or say why the effect "
         "cannot be predicted: {\"model_id\": \"<uuid>\", \"change\": \"add_holdout\"}"},
        {"model_health",
         "check whether a deployed model can still be trusted - the held-out figure, the "
         "overfitting gap, whether scores are calibrated: {\"model_id\": \"<uuid>\"}"},
        {"get_run_report",
         "read what a finished run found — model quality (held-out first), the "
         "drivers, and the ranked at-risk list with the evidence behind each row: "
         "{\"run_id\": \"<uuid>\", \"limit\": 20}"},
        {"explain",
         "explain WHY one account scored as it did — every feature's signed "
         "contribution, strongest first: {\"model\": \"<id>\", \"dataset_path\": "
         "\"<database.table>\", \"entity\": 8336}, or for an account not in the "
         "data: {\"model\": \"<id>\", \"row\": {...}}"},
        {"deployment_profile",
         "ask this node how to deploy it — ports, storage, peering, security, and what "
         "it does NOT need: {\"target\": \"helm\"} returns chart values"},
        {"list_indexes", "list the search indexes and the fields they cover"},
        {"list_connections", "list the configured source connections"},
        {"server_info", "read this node's version and cluster membership"},
        // "which version are you" is asked of the model as often as of the binary,
        // and the agent is the only surface a person asks it on.
        {"versions",
         "report the software version and WHICH agent version is answering, "
         "including its checkpoint"},
    };
    // OPT-IN per agent: an agent only gets live tools it explicitly declares in its
    // record. This matters — naming tools in the prompt changes what the model
    // writes, and the churn-trained base agent (#555) answers WORSE when told about
    // tools it was never trained to use. So the default agent is untouched, and
    // enabling introspection is a data change (`update_agent` tools), not a code
    // change. Teaching a model to actually use them is #904's training half.
    std::vector<zetta::agent::AgentToolSpec> enabled_tools;
    for (const zetta::agent::AgentToolSpec& spec : kAgentTools) {
        if (std::find(agent->tools.begin(), agent->tools.end(), spec.name) !=
            agent->tools.end()) {
            enabled_tools.push_back(spec);
        }
    }
    const zetta::agent::AgentToolRunner agent_tools =
        [&context](const std::string& tool,
                   const nlohmann::json& arguments) -> zetta::agent::ToolResult {
        const bool allowed = std::any_of(
            kAgentTools.begin(), kAgentTools.end(),
            [&tool](const zetta::agent::AgentToolSpec& spec) { return spec.name == tool; });
        if (!allowed) {
            return zetta::agent::ToolResult::failure("'" + tool +
                                                     "' is not a tool this agent may call");
        }
        if (context.registry == nullptr) {
            return zetta::agent::ToolResult::failure("no capability registry for this call");
        }
        const ApiResult called = context.registry->call(
            tool, arguments.is_object() ? arguments : nlohmann::json::object(), context);
        if (!called.ok) return zetta::agent::ToolResult::failure(called.error.message);
        return zetta::agent::ToolResult::success(called.value);
    };

    // Run the agent (loads its model, runs the tool-calling loop) — the shared path the GUI
    // chat panel also uses, so both behave identically. agent.model must be a local
    // checkpoint dir; pull it first with `zetta pull` if it isn't present.
    // Ground the answer in what this Project's pipeline actually produced (#914).
    // The two questions a churn buyer asks — "what's driving churn" and "which are
    // most at risk" — are answerable from the trained model's ranked drivers and its
    // measure, which the run already wrote. Without this the agent answers from
    // training priors and cites nothing real.
    std::string grounding;
    bool grounded = false;
    std::string grounding_warning;
    std::string grounded_project;
    // The caller may name a Project. When it does not, fall back to the one whose
    // run finished most recently (#969): after `agent_run`, "which accounts are
    // most at risk" and "why that one" are about the run that just happened, and
    // making the user restate its name every turn is how a conversation stops
    // being one.
    //
    // This is grounding, not memory. It carries ONE referent — the active
    // Project — onto the retrieval path the model was actually trained on
    // (#914), rather than threading a conversation history the corpus has almost
    // no examples of.
    //
    // Whichever way it resolves, the reply reports `grounded_project`. A silent
    // guess about which Project an answer came from is the provenance failure in
    // #942 wearing a different hat.
    std::string project_hint;
    if (input.contains("project")) {
        project_hint = input.at("project").get<std::string>();
    } else {
        std::string newest_at;
        for (const std::shared_ptr<zetta::pipeline::Run>& run : context.node.runs()) {
            if (!run || run->project().empty()) continue;
            if (run->status() != zetta::pipeline::RunStatus::Succeeded) continue;
            if (newest_at.empty() || run->updated() > newest_at) {
                newest_at = run->updated();
                // find_project_by_id is private to Node; resolve by scanning the
                // public name list, which is small and already how other handlers
                // do it.
                for (const std::string& name : context.node.project_names()) {
                    const auto owner = context.node.find_project(name);
                    if (owner && owner->id() == run->project()) {
                        project_hint = owner->name();
                        break;
                    }
                }
            }
        }
    }
    if (!project_hint.empty()) {
        const std::string project_name = project_hint;
        if (auto project = resolve_project(context, project_name)) {
            std::shared_ptr<zetta::ml::Model> newest;
            for (const auto& candidate : context.node.models()) {
                if (candidate->project() != project->id()) continue;
                if (!newest || candidate->created() > newest->created()) newest = candidate;
            }
            if (newest) {
                grounding = model_grounding_text(newest->metadata(), newest->name(),
                                                 project->name());
                // …and the accounts it actually ranked, so "which are most at risk"
                // is answered with real accounts rather than invented ones (#915).
                grounding += at_risk_grounding_text(context.node, project->graphs());
                grounded = true;
                grounded_project = project->name();
            } else {
                // Deliberately NOT a prompt instruction. Measured behavior: told plainly
                // that a Project has no model and not to estimate, the base agent ignored
                // it and invented a churn rate ("dropped from 42% to 38%"). It pattern-
                // matches a churn answer rather than following an instruction it was never
                // trained on. So the refusal cannot live in the prompt — the caller is told
                // the reply is ungrounded and must present it as such (#914 / #910).
                grounding_warning =
                    "Project \"" + project->name() +
                    "\" has no trained model — its pipeline has not been run, so this reply "
                    "is not grounded in any measured result. Any figure in it is invented.";
            }
        }
    }

    // The conversation so far, supplied by the caller. Without it every turn is a FIRST turn:
    // "list all of those options" has no referent, and three different follow-ups produce the same
    // reply because the model sees three unrelated opening messages (#942).
    //
    // Caller-managed on purpose, and TEMPORARY: once a Conversation is persisted (#945) the thread
    // is read from the node and this parameter goes away. Two sources of history is how they drift.
    std::vector<zetta::agent::PriorTurn> history;
    if (const auto turns = input.find("history"); turns != input.end() && turns->is_array()) {
        for (const nlohmann::json& turn : *turns) {
            if (!turn.is_object()) continue;
            zetta::agent::PriorTurn prior;
            prior.ask = turn.value("ask", std::string());
            prior.reply = turn.value("reply", std::string());
            // Tool outcomes matter: a follow-up like "why did that fail?" refers to what RAN, not
            // to the prose of the previous answer.
            if (const auto tools = turn.find("tools"); tools != turn.end() && tools->is_array()) {
                for (const nlohmann::json& outcome : *tools) {
                    if (!outcome.is_object()) continue;
                    prior.tools.push_back({outcome.value("tool", std::string()),
                                           outcome.value("status", std::string()),
                                           outcome.value("detail", std::string())});
                }
            }
            if (!prior.ask.empty() || !prior.reply.empty()) history.push_back(std::move(prior));
        }
    }

    const zetta::agent::ChatResponse chat =
        enabled_tools.empty()
            ? zetta::agent::respond(*agent, message, max_new_tokens, max_rounds, {}, {},
                                    grounding, history)
            : zetta::agent::respond(*agent, message, max_new_tokens, max_rounds, agent_tools,
                                    enabled_tools, grounding, history);
    if (!chat.ok) return ApiResult::failure("engine_error", chat.error);

    nlohmann::json tool_calls = nlohmann::json::array();
    for (const zetta::agent::ChatToolInvocation& invocation : chat.result.invocations) {
        nlohmann::json entry;
        entry["tool"] = invocation.call.tool;
        entry["status"] = invocation.status == zetta::agent::ApplyStatus::Ok ? "ok"
                          : invocation.status == zetta::agent::ApplyStatus::UnknownTool
                              ? "unknown_tool"
                              : "failed";
        if (!invocation.detail.empty()) entry["detail"] = invocation.detail;
        tool_calls.push_back(std::move(entry));
    }
    nlohmann::json output;
    output["reply"] = chat.result.reply;
    output["tool_calls"] = std::move(tool_calls);
    output["rounds"] = chat.result.rounds;
    // Whether the reply stands on measured results, so a caller never presents a
    // generated number as a fact (#914). `grounded` is true only when a Project was
    // named AND it has a trained model whose real measure + drivers were supplied.
    output["grounded"] = grounded;
    // Always say WHICH Project the answer was grounded in. When the caller did not
    // name one, this is the only way to see that the engine chose the most
    // recently-run Project rather than the one you had in mind (#969).
    if (!grounded_project.empty()) output["grounded_project"] = grounded_project;
    if (!grounding_warning.empty()) output["grounding_warning"] = grounding_warning;
    // The mode the agent DECLARED for this turn (#918), so a caller switches on an explicit
    // value instead of inferring intent from the payload's shape. `answer` renders as prose,
    // `clarify` is a question awaiting a reply, `abstain` is a refusal the surface must present
    // as such rather than dressing up, and `action` carries a proposed Pipeline to review.
    if (chat.action) {
        output["kind"] = std::string(zetta::agent::to_string(chat.action->kind));
        // A proposed Pipeline is returned only when one was actually built — never an empty
        // graph, which a caller could mistake for "the agent built nothing on purpose".
        if (!chat.action->execution_graph.nodes().empty()) {
            output["pipeline"] = zetta::ir::to_json(chat.action->execution_graph);
            // How many steps the harness actually built by running the model's tool calls, so a
            // caller can tell a real Pipeline from an empty gesture without walking the graph.
            output["pipeline_steps"] =
                static_cast<int>(chat.action->execution_graph.nodes().size());
        }
    } else {
        // The generation did not parse as a response envelope. Say so explicitly: an unparsed
        // turn is an error state, not an `action` that happened to carry no pipeline.
        output["kind"] = "unparsed";

        // And do NOT hand the raw generation back as the reply (#979). It was reaching
        // the Studio chat window verbatim: asked "hello", a user saw 636 characters of
        // `{"kind":"answer","message":…,"tool":"serve"}`.
        //
        // A model that emits garbage is a training problem. A harness that SHOWS the
        // garbage to a customer is a serving problem, and it is the one that has to be
        // right, because it bounds the blast radius of every future parse failure —
        // including ones nobody has seen yet.
        //
        // The payload is not discarded: it moves to `raw`, which a developer reads and a
        // chat window does not render.
        output["raw"] = output.value("reply", std::string{});
        output["reply"] =
            "I could not produce a well-formed answer to that. Nothing was built and "
            "nothing was changed. Try rephrasing, or ask something more specific.";
    }
    return ApiResult::success(std::move(output));
}

// --- agent_run: the base agent builds AND runs an execution graph ------------------
//
// The point of #872: the base (#555) agent is usable RIGHT AWAY — it does not need the
// RLVR tool-calling fine-tune (#859) to drive real work. This capability closes the loop
// end to end from plain language:
//   1. the agent reads the message and recognizes the use case (chat/respond),
//   2. the engine compiles that message into a real ExecutionGraph (pipeline::compile_intent
//      — the same churn-shape heuristic behind compile_intent), stored on a project,
//   3. the graph runs through the gate + the real churn engine (delegated to handle_run).
// So "the agent builds and runs a churn graph for Charles" is one call. #859 later lets the
// agent emit the graph step-by-step itself; today the engine builds it from the same intent.
const nlohmann::json& agent_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"agent", "message"}},
        {"additionalProperties", false},
        {"properties",
         {{"agent", {{"type", "string"}, {"minLength", 1}, {"description", "the agent's id"}}},
          {"message",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the plain-language ask, e.g. 'predict which customers will churn'"}}},
          {"project",
           {{"type", "string"},
            {"description",
             "land the graph + run on this project (name or uuid); a new project is "
             "created when omitted"}}},
          {"graph_name",
           {{"type", "string"}, {"description", "name for the stored graph (default derived)"}}},
          {"max_new_tokens", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}}},
          {"max_rounds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}},
          {"top_k",
           {{"type", "integer"}, {"minimum", 0}, {"description", "at-risk list cap (0 = all)"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "`database.table` (a connection/query landed it) or a churn dataset dir "
             "on the node (default: the built-in demo)"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"model_name", {{"type", "string"}, {"description", "registered model name"}}}}}};
    return schema;
}

const nlohmann::json& agent_run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"reply", "shape", "project", "graph", "steps", "run"}},
        {"properties",
         {{"reply", {{"type", "string"}, {"description", "the agent's recognition of the ask"}}},
          {"shape",
           {{"type", "string"}, {"description", "the compiled template: 'churn' | 'generic'"}}},
          {"project", {{"type", "string"}}},
          {"graph", {{"type", "string"}}},
          {"graph_id", {{"type", "string"}}},
          {"steps", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"run",
           {{"type", "object"},
            {"description", "the run result (run_id + engine + status), poll get_run"}}}}}};
    return schema;
}

ApiResult handle_agent_run(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no agents on a standalone node");
    }
    const std::string agent_id = input.at("agent").get<std::string>();
    const std::string message = input.at("message").get<std::string>();

    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(agent_id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + agent_id + "'");

    // 1. The agent reads the ask and recognizes the use case (the shared respond() path).
    const zetta::agent::ChatResponse chat = zetta::agent::respond(
        *agent, message, input.value("max_new_tokens", 256), input.value("max_rounds", 3));
    if (!chat.ok) return ApiResult::failure("engine_error", chat.error);

    // 2. The engine compiles the same ask into a real, editable ExecutionGraph.
    std::string reason;
    const std::optional<zetta::pipeline::CompiledIntent> compiled =
        zetta::pipeline::compile_intent(message, &reason);
    if (!compiled) {
        return ApiResult::failure("engine_error",
                                  "the agent could not compile a graph from the ask: " + reason);
    }
    const std::string shape = std::string(zetta::pipeline::to_string(compiled->shape));
    std::vector<std::string> steps;
    for (const std::shared_ptr<zetta::ir::Node>& node : compiled->graph.nodes()) {
        steps.push_back(node->name);
    }

    // Land the graph on a project (the caller's, or a fresh one so a chat needs no setup).
    std::shared_ptr<zetta::pipeline::Project> project;
    if (const auto named = input.find("project"); named != input.end()) {
        project = resolve_project(context, named->get<std::string>());
        if (!project) {
            return ApiResult::failure(
                "not_found", "no project '" + named->get<std::string>() + "' (or with that id)");
        }
    } else {
        const std::string owner =
            context.acting_key.empty() ? std::string("local") : context.acting_key;
        project = context.node.create_project(
            "agent " + shape + " · " + zetta::util::new_uuid().substr(0, 8), owner);
        if (!project) {
            return ApiResult::failure("engine_error", "could not create a project for the run");
        }
    }

    const std::string graph_name =
        input.value("graph_name", agent->name.empty() ? shape + " graph"
                                                       : agent->name + " · " + shape + " graph");
    const std::shared_ptr<zetta::pipeline::ProjectGraph> stored =
        project->create_graph(graph_name, std::move(compiled->graph));
    if (!stored) {
        return ApiResult::failure(
            "engine_error", "could not store the graph '" + graph_name + "' on project '" +
                                project->name() + "' (name clash or empty)");
    }

    // `run` refuses a plan nobody approved (#969), and a freshly stored graph is
    // `proposed`. Calling agent_run IS the approval: the caller asked, in one
    // act, to build this and run it. So record that approval against the caller
    // rather than bypass the gate — the audit trail then says who said yes,
    // which is the whole point of the state existing.
    //
    // The conversational path is different and deliberately so: an agent that
    // PROPOSES a run stops here, and a human approves separately. That is the
    // `propose` half of #969 and it does not come through this handler.
    if (!project->approve_graph(stored->name(), actor_of(context),
                                zetta::util::now_iso8601())) {
        return ApiResult::failure(
            "engine_error",
            "could not record approval for the plan agent_run just built");
    }

    // 3. Run it through the gate + the real engine — the exact path `run` uses (DRY).
    nlohmann::json run_input;
    run_input["project"] = project->name();
    run_input["graph"] = graph_name;
    for (const char* passthrough :
         {"top_k", "dataset_path", "dataset", "model_name", "coordinate"}) {
        if (const auto found = input.find(passthrough); found != input.end()) {
            run_input[passthrough] = *found;
        }
    }
    const ApiResult run_result = handle_run(run_input, context);
    if (!run_result.ok) return run_result;  // reason already carries the failing step

    nlohmann::json output;
    output["reply"] = chat.result.reply;
    output["shape"] = shape;
    output["project"] = project->name();
    output["graph"] = graph_name;
    output["graph_id"] = stored->id();
    output["steps"] = steps;
    output["run"] = run_result.value;
    return ApiResult::success(std::move(output));
}

// --- dataset generation (#873): native, in-Zetta training-corpus synthesis ------------------

const nlohmann::json& list_dataset_generators_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_dataset_generators_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"generators",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"kind", {{"type", "string"}}},
                {"title", {{"type", "string"}}},
                {"description", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_dataset_generators(const nlohmann::json&, ApiContext&) {
    nlohmann::json generators = nlohmann::json::array();
    for (const std::shared_ptr<const zetta::dataset::DatasetGenerator>& generator :
         zetta::dataset::DatasetGeneratorFactory::builtin().generators()) {
        generators.push_back({{"kind", generator->kind()},
                              {"title", generator->title()},
                              {"description", generator->description()}});
    }
    return ApiResult::success(nlohmann::json{{"generators", std::move(generators)}});
}

const nlohmann::json& generate_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"kind"}},
        {"additionalProperties", false},
        {"properties",
         {{"kind",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "a generator kind from list_dataset_generators (e.g. 'base-agent')"}}},
          {"variants_per_subject",
           {{"type", "integer"}, {"minimum", 1}, {"description", "task variants per plan subject"}}},
          {"held_out_fraction",
           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"description", "canary fraction"}}},
          {"path",
           {{"type", "string"},
            {"description", "write the gated JSONL corpus to this file on the node (optional)"}}}}}};
    return schema;
}

const nlohmann::json& generate_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"kind", {{"type", "string"}}},
          {"cases", {{"type", "integer"}}},
          {"subjects", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"held_out", {{"type", "integer"}}},
          {"admitted", {{"type", "boolean"}}},
          {"path", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_generate_dataset(const nlohmann::json& input, ApiContext&) {
    const std::string kind = input.at("kind").get<std::string>();
    const std::shared_ptr<const zetta::dataset::DatasetGenerator> generator =
        zetta::dataset::DatasetGeneratorFactory::builtin().create(kind);
    if (!generator) {
        return ApiResult::failure("not_found", "no dataset generator '" + kind + "'");
    }
    zetta::dataset::DatasetGenSpec spec;
    spec.variants_per_subject = input.value("variants_per_subject", 1);
    spec.held_out_fraction = input.value("held_out_fraction", 0.0);
    const zetta::dataset::GeneratedDataset dataset = generator->generate(spec);

    // Gate it (blender-lr discipline) — the generator builds gated cases, but the corpus is only
    // trusted once admit_corpus confirms it (reference passes, controls fail).
    const zetta::agent::AdmissionResult gate = zetta::agent::admit_corpus(dataset.cases);

    nlohmann::json output;
    output["kind"] = kind;
    output["cases"] = dataset.cases.size();
    output["subjects"] = dataset.subjects;
    output["held_out"] = dataset.held_out;
    output["admitted"] = gate.admitted;
    if (!gate.admitted && !gate.failures.empty()) {
        output["gate_failure"] = std::string(zetta::agent::to_string(gate.failures.front().kind)) +
                                 " (task '" + gate.failures.front().task_id + "')";
    }
    if (const auto path = input.find("path"); path != input.end()) {
        std::ofstream file(path->get<std::string>());
        if (!file) {
            return ApiResult::failure(
                "engine_error", "could not open '" + path->get<std::string>() + "' for writing");
        }
        file << zetta::agent::dump_corpus_jsonl(dataset.cases);
        output["path"] = *path;
    }
    return ApiResult::success(std::move(output));
}

}  // namespace

void register_core_capabilities(Registry& registry) {
    registry.add(Capability{
        "versions",
        "Show version",
        "Read the software version and the version of every agent this node "
        "serves, including which one answers in Studio. No side effects.",
        versions_schema(),
        versions_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_versions});
    registry.add(Capability{
        "server_info",
        "Server info",
        "Read this node's identity, version, entity counts, and cluster "
        "membership. No side effects.",
        server_info_schema(),
        server_info_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_server_info});

    registry.add(Capability{
        "sql",
        "Run SQL",
        "Run one governed SQL statement (Postgres subset). Every system.* "
        "observability view is readable here. DDL/INSERT mutate the database.",
        sql_schema(),
        sql_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_sql});

    registry.add(Capability{
        "search",
        "Search",
        "Search a registered index by free text — distributed scatter-gather "
        "across the cluster when the table is partitioned — returning ranked "
        "hits with per-node provenance and a trace id. No side effects.",
        search_schema(),
        search_response_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_search});

    registry.add(Capability{
        "list_projects",
        "List projects",
        "Read the names of every AI project on this node. No side effects.",
        list_projects_schema(),
        list_projects_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_projects});

    registry.add(Capability{
        "list_traces",
        "List recent traces",
        "Read the most recent observability spans on this node (intent, "
        "timings, result shapes). No side effects.",
        list_traces_schema(),
        list_traces_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_traces});

    registry.add(Capability{
        "get_trace",
        "Get one trace",
        "Read every recorded span of one trace by its 32-hex trace id. No "
        "side effects.",
        get_trace_schema(),
        get_trace_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_trace});

    registry.add(Capability{
        "get_project",
        "Get one project",
        "Read one AI project's full detail — metadata plus its inputs and "
        "graphs — by name or uuid. No side effects.",
        get_project_schema(),
        get_project_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_project});

    registry.add(Capability{
        "get_graph",
        "Get one graph",
        "Read one execution graph's full structure — its typed step nodes "
        "{id, kind, name} and directed edges {from, to, kind} — by project + "
        "graph name or uuid. No side effects.",
        graph_ref_schema(),
        graph_structure_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_graph});

    registry.add(Capability{
        "save_graph",
        "Save a graph",
        "Persist an edited execution graph — its nodes + edges — back to a "
        "project graph, creating it when absent or replacing the existing one "
        "(replicated + audited, the visual editor's save path).",
        save_graph_schema(),
        project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "save_graph"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_save_graph});

    registry.add(Capability{
        "approve_plan",
        "Approve a proposed plan",
        "Approve a stored, proposed Pipeline for build (#885, the design's "
        "review-before-build flow): records the approving principal + time, "
        "replicates, and audits it (graph_approved). gate and deploy require an "
        "approved plan; editing a plan re-proposes it.",
        approve_plan_schema(),
        approve_plan_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "approve_plan"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_approve_plan});

    registry.add(Capability{
        "get_run",
        "Get one run",
        "Read one run's full validated record — status, logs, outputs, "
        "lineage — by its uuid. No side effects.",
        get_run_schema(),
        get_run_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_run});

    registry.add(Capability{
        "get_run_report",
        "Get a run's report facts",
        "Read the facts a finished run produced — how good the model is (held-out "
        "first, in-sample beside it), what drives the outcome, and the ranked "
        "at-risk list with its evidence. Assembled at query time from the run's "
        "model and its at-risk table, so it cannot drift from them. Anything a "
        "report would want and this run cannot supply is listed in `missing` with "
        "the reason, rather than left blank. No side effects. (#958)",
        get_run_report_schema(),
        get_run_report_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_run_report});

    registry.add(Capability{
        "propose_improvement",
        "Propose an improvement",
        "Propose a change to a deployed model and state its PREDICTED EFFECT, or refuse and say "
        "why the effect cannot be predicted. No side effects - it proposes, it does not act.",
        propose_improvement_schema(),
        propose_improvement_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_propose_improvement});
    registry.add(Capability{
        "model_health",
        "Model health",
        "Read whether a deployed model can still be trusted: the held-out figure, the "
        "overfitting gap against in-sample, whether scores are calibrated, and what each "
        "means. No side effects.",
        model_health_schema(),
        model_health_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_model_health});
    registry.add(Capability{
        "get_model",
        "Get one model",
        "Read one model's full validated record — kind, lineage, feature "
        "schema, metrics, weights fingerprint — by its uuid. No side effects.",
        get_model_schema(),
        get_model_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_model});

    registry.add(Capability{
        "get_workspace_profile",
        "Get one workspace profile",
        "Read one Workspace Profile's full validated record (owner, role, daily "
        "work, approved source refs) by its uuid. No side effects. (#886)",
        get_workspace_profile_schema(),
        workspace_profile_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_workspace_profile});

    registry.add(Capability{
        "create_workspace_profile",
        "Create a workspace profile",
        "Create the calling user's Workspace Profile — the input to the Discover "
        "Opportunities pathway (#886). Owner is the caller; replicates to every "
        "node. Role, daily work, and approved source refs are the editable body.",
        create_workspace_profile_schema(),
        workspace_profile_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_workspace_profile"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_workspace_profile});

    registry.add(Capability{
        "update_workspace_profile",
        "Update a workspace profile",
        "Update a Workspace Profile's role, daily work, or approved source refs "
        "by its uuid (#886); the owner and created stamp stay fixed. Replicates.",
        update_workspace_profile_schema(),
        workspace_profile_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_workspace_profile"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_update_workspace_profile});

    registry.add(Capability{
        "get_plan",
        "Get one plan",
        "Read one Plan's full validated record (name, tier, quota limits) by its "
        "uuid. Metered usage is read live from system.usage. (Conflict D)",
        get_plan_schema(),
        plan_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_plan});

    registry.add(Capability{
        "create_plan",
        "Create a plan",
        "Create a Plan — the quota/tier config the Billing/Usage surface measures "
        "live consumption against (Conflict D). Replicates to every node.",
        create_plan_schema(),
        plan_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_plan"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_plan});

    registry.add(Capability{
        "update_plan",
        "Update a plan",
        "Update a Plan's name, tier, quotas, or period by its uuid (Conflict D); "
        "the created stamp stays fixed. Replicates.",
        update_plan_schema(),
        plan_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_plan"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_update_plan});

    registry.add(Capability{
        "get_job",
        "Get one job",
        "Read one compute job's full validated record — kind, status, "
        "lineage refs, detail — by its uuid. No side effects.",
        get_job_schema(),
        get_job_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_job});

    registry.add(Capability{
        "get_index",
        "Get one index",
        "Read one registered index's full validated record — table, kind, "
        "backend, fields — by its uuid. No side effects.",
        get_index_schema(),
        get_index_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_index});

    registry.add(Capability{
        "get_dataset",
        "Get one dataset",
        "Read one dataset's computed summary — schema, row count, size, "
        "format, replication, partition placement — by name (plus database "
        "when ambiguous). No side effects.",
        get_dataset_schema(),
        dataset_summary_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_dataset});

    registry.add(Capability{
        "list_runs",
        "List runs",
        "Read a compact listing of every run — id, lineage refs, status, "
        "stamps, log/output counts. No side effects.",
        list_runs_schema(),
        list_runs_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_runs});

    registry.add(Capability{
        "list_models",
        "List models",
        "Read a compact listing of every registered model — id, name, kind, "
        "lineage refs, weights presence. No side effects.",
        list_models_schema(),
        list_models_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_models});

    registry.add(Capability{
        "list_jobs",
        "List jobs",
        "Read a compact listing of every compute job — id, kind, status, "
        "lineage refs, stamps, detail. No side effects.",
        list_jobs_schema(),
        list_jobs_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_jobs});

    registry.add(Capability{
        "list_indexes",
        "List indexes",
        "Read a compact listing of every registered index — id, table, kind, "
        "backend, fields — to discover what `search` can target. No side "
        "effects.",
        list_indexes_schema(),
        list_indexes_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_indexes});

    registry.add(Capability{
        "list_datasets",
        "List datasets",
        "Read the computed summary of every dataset — schema, row count, "
        "size, format, replication, partition placement. No side effects.",
        list_datasets_schema(),
        list_datasets_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_datasets});

    registry.add(Capability{
        "submit_feedback",
        "Submit feedback",
        "Snapshot a feedback bundle — your description plus the relevant "
        "traces, recent errors, crash reports, and versions — stored as a "
        "large object you explicitly export; nothing leaves the cluster on "
        "its own.",
        submit_feedback_schema(),
        submit_feedback_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_submit_feedback});

    registry.add(Capability{
        "create_project",
        "Create project",
        "Create a new AI project (the central user-managed artifact); the "
        "project replicates to every node.",
        create_project_schema(),
        create_project_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_project"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_project});

    // --- agents (#849): CRUD for the served, selectable agent entity ----------
    registry.add(Capability{
        "create_agent",
        "Create an agent",
        "Create a served, selectable agent — a model (checkpoint dir, model:<uuid>, or a "
        "HuggingFace repo id) plus an optional system prompt and the tools it may call. "
        "Returns the stored record with its generated id; replicates to every node.",
        create_agent_schema(),
        agent_fields_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_agent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_agent});
    registry.add(Capability{
        "list_agents",
        "List agents",
        "Read every agent on this node (id, name, model, tools). No side effects.",
        list_agents_schema(),
        list_agents_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_agents});
    registry.add(Capability{
        "get_agent",
        "Get one agent",
        "Read one agent by id. No side effects.",
        agent_id_schema(),
        agent_fields_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_agent});
    registry.add(Capability{
        "update_agent",
        "Update an agent",
        "Update the provided fields of an agent (name, description, model, system prompt, "
        "tools); replicates. Returns the updated record.",
        update_agent_schema(),
        agent_fields_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_agent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_update_agent});
    registry.add(Capability{
        "agent_training_status",
        "Should this agent be retrained?",
        "Whether an agent is stale (its tools or goals changed since its newest Saved Version was "
        "trained), whether its schedule says it is due, every version it has produced, and any "
        "goals that resolve to no verifiable check. Read-only.",
        agent_id_schema(),
        training_status_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Orchestration, "agent_training_status"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_agent_training_status});
    registry.add(Capability{
        "set_training_schedule",
        "Set when an agent retrains",
        "Set an agent's training spec: the corpus, the base checkpoint, the step count, and the "
        "cadence (manual, on_change, daily, weekly, monthly). Manual is the default - an agent "
        "never retrains behind your back.",
        training_schedule_schema(),
        agent_fields_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "set_training_schedule"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_set_training_schedule});
    registry.add(Capability{
        "score",
        "Score a dataset or rows through a trained model",
        "Run a registered model over a `database.table` dataset (compute-near-data, this "
        "node's own partitions) or over rows you supply, and get the entities ranked by "
        "risk plus what drives it. This is how the at-risk list is answerable after the "
        "run that produced it has ended, and how an account that is not in the data at "
        "all gets a score. Read-only.",
        score_schema(),
        score_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "predict"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_score});
    registry.add(Capability{
        "deployment_profile",
        "How to deploy this node",
        "Ask a node how to deploy itself: every port and what it carries, what it needs "
        "from the cluster and — as important — the dependencies people assume it needs "
        "and it does not, the storage and peering model, and the security posture. "
        "`target=helm` returns values a chart can consume directly. Computed from the "
        "live node, so it cannot drift from what the binary does. Never emits a "
        "credential; it says how to seed one. Read-only. (#974)",
        deployment_profile_schema(),
        deployment_profile_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_deployment_profile});
    registry.add(Capability{
        "explain",
        "Explain one row's score",
        "Answer WHY one account scored as it did: the exact signed contribution of "
        "every feature, strongest first, for a single entity of a dataset or for a "
        "row you supply. This is the per-account answer — `score`'s drivers are the "
        "population ranking, identical for every row, so two accounts with the same "
        "score look the same there even when they are at risk for opposite reasons. "
        "Read-only.",
        explain_schema(),
        explain_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "predict"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_explain});
    registry.add(Capability{
        "delete_agent",
        "Delete an agent",
        "Remove an agent by id (a CRDT tombstone; replicates). Idempotent.",
        agent_id_schema(),
        delete_agent_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_agent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_agent});
    Capability chat_capability{
        "chat",
        "Chat with an agent",
        "Send a message to an agent and get its reply, with tool-calling: the agent's model "
        "generates, any tool calls it emits run in its sandbox and feed back, until it "
        "answers. Loads the agent's model (a local checkpoint dir). No persistent side "
        "effects.",
        chat_schema(),
        chat_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_chat};
    // Model decode is CPU/GPU-bound and touches only the thread-safe replication
    // store, so in principle it can run off the owner/tick thread. But the current
    // runtime::offload path does not resume the REST request coroutine back on the
    // connection's executor, so route.send() after it never completes and the HTTP
    // response hangs (MCP, which runs on the owner loop, is unaffected). Until the
    // offload/executor resumption is fixed, serve chat on the owner loop — correct,
    // at the cost of briefly occupying the tick during a decode. See #731.
    // NOT offloadable, and the reason changed in 2026-08 (#975). The flag is only
    // safe for a handler touching no main-thread-only Node state — and grounding a
    // reply on the active Project (#969) reads `node.runs()` and
    // `node.project_names()`, which are exactly that. So offloading chat today would
    // race the Node tree.
    //
    // That also means the REST hang in #975 cannot be fixed by flipping this flag:
    // whoever fixes it has to either move the grounding read behind the owner gate
    // or fix the offload/executor resumption the comment below describes.
    chat_capability.offloadable = false;
    registry.add(std::move(chat_capability));
    registry.add(Capability{
        "agent_run",
        "Agent builds + runs a graph",
        "Give an agent a plain-language ask; it recognizes the use case, compiles a real "
        "execution graph from it, and runs that graph through the gate + the real engine "
        "(churn today) — end to end in one call. The base agent needs no tool-calling "
        "fine-tune for this. Returns the agent's reply, the built graph's steps, and the "
        "run id (poll get_run / system.runs).",
        agent_run_schema(),
        agent_run_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_agent_run});

    // --- dataset generation (#873) -------------------------------------------
    registry.add(Capability{
        "list_dataset_generators",
        "List dataset generators",
        "Read the native training-dataset generators available on this node (the base agent's "
        "corpus, Auto-Suggest, …) — each grows + gates a corpus grounded in the solutions plan. "
        "No side effects.",
        list_dataset_generators_schema(),
        list_dataset_generators_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_dataset_generators});
    registry.add(Capability{
        "generate_dataset",
        "Generate a dataset",
        "Synthesize an admission-gated training corpus natively (ADR-17), grounded in the "
        "solutions plan: pick a generator kind, get the gated cases (reference passes, negative "
        "controls fail) with reference actions, and optionally write the JSONL to a path. This is "
        "the native replacement for the offline Python generators + what a per-customer fork "
        "(#853) uses to grow its own corpus.",
        generate_dataset_schema(),
        generate_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_generate_dataset});

    // --- governed serving (#648, docs/api/serving.md) -------------------------

    registry.add(Capability{
        "expose_model",
        "Expose model",
        "Publish a model's HTTP predict endpoint (/v1/models/{name}/predict) "
        "on every node of the cluster; endpoint access then requires the "
        "data:predict capability. Idempotent; undo with unexpose_model.",
        expose_model_schema(),
        expose_model_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "expose_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_expose_model});

    registry.add(Capability{
        "unexpose_model",
        "Un-expose model",
        "Withdraw a model's HTTP predict endpoint everywhere in one action — "
        "the replicated exposure flag flips and every node refuses the route. "
        "Idempotent.",
        expose_model_schema(),
        expose_model_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "unexpose_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_unexpose_model});

    registry.add(Capability{
        "expose_index",
        "Expose index",
        "Publish an index's HTTP search endpoint (/v1/indexes/{name}/search — "
        "the distributed scatter-gather path underneath) on every node; "
        "endpoint access then requires the data:search capability. "
        "Idempotent; undo with unexpose_index.",
        expose_index_schema(),
        expose_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "expose_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_expose_index});

    registry.add(Capability{
        "unexpose_index",
        "Un-expose index",
        "Withdraw an index's HTTP search endpoint everywhere in one action — "
        "the replicated exposure flag flips and every node refuses the route. "
        "Idempotent.",
        expose_index_schema(),
        expose_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "unexpose_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_unexpose_index});

    registry.add(Capability{
        "create_index",
        "Create index",
        "Build a full-text index over a loaded table: records the Search index "
        "entity and builds the per-partition BM25F sub-indexes so `search` "
        "returns ranked hits (load the table first with add_dataset).",
        create_index_schema(),
        create_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_index});

    // --- governed action parity (#617): the rest of the product loop ---------

    registry.add(Capability{
        "capture_intent",
        "Capture intent",
        "Capture a natural-language intent as a replicated project input with "
        "provenance; compile it into a graph with compile_intent.",
        capture_intent_schema(),
        project_input_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "capture_intent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_capture_intent});

    registry.add(Capability{
        "import_n8n",
        "Import n8n workflow",
        "Validate an n8n workflow export against its schema and store it "
        "byte-for-byte as a replicated project artifact input; compile it "
        "with compile_n8n.",
        import_n8n_schema(),
        project_input_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "import_n8n"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_import_n8n});

    registry.add(Capability{
        "compile_intent",
        "Compile intent to graph",
        "Compile a captured intent input into a named, editable execution "
        "graph stored on the project (replicated; audited as a graph change).",
        compile_intent_schema(),
        project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_intent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_intent});

    registry.add(Capability{
        "compile_n8n",
        "Compile n8n workflow to graph",
        "Compile a stored n8n workflow input into a named, editable execution "
        "graph stored on the project (replicated; audited as a graph change).",
        compile_n8n_schema(),
        project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_n8n"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_n8n});

    registry.add(Capability{
        "compile_python",
        "Compile simulated Python to graph",
        "Lower a simulated-Python pipeline source to a named, editable "
        "execution graph stored on the project (replicated; audited as a "
        "graph change); zetta re-implements the pipeline — it never runs "
        "Python.",
        compile_python_schema(),
        project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_python"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_python});

    registry.add(Capability{
        "gate",
        "Run the correctness gate",
        "Run the correctness gate over a stored project graph and return the "
        "full validation report (per-check status + reason, verdict, "
        "block_deploy). Evaluates only — no state change.",
        gate_schema(),
        gate_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "gate"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_gate});

    registry.add(Capability{
        "deploy",
        "Deploy a graph",
        "Place a stored project graph onto a target through the correctness "
        "gate (RULE-1: a failing gate blocks with the failing check's "
        "reason); the recorded deployment replicates and is audited.",
        deploy_schema(),
        deploy_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "deploy"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_deploy});

    registry.add(Capability{
        "run",
        "Run a graph",
        "Execute a stored, gate-passing project graph: a churn-shaped graph "
        "runs the real engine over the live cluster and returns its run id "
        "immediately (poll get_run / system.runs); other graphs run "
        "structurally within the call.",
        run_schema(),
        run_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_run});

    registry.add(Capability{
        "add_dataset",
        "Add a dataset",
        "Load a local parquet/jsonl/json/csv file into a new partitioned "
        "table placed across the cluster (audited as dataset_added; visible "
        "in system.datasets).",
        add_dataset_schema(),
        add_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "add_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_add_dataset});

    registry.add(Capability{
        "create_connection",
        "Save a source connection",
        "Save a named connector configuration (credential included) so a "
        "connector query can reference it by name instead of carrying the "
        "token in every request. The connection is stored replicate-to-all — "
        "every node holds it, which is what lets each node fetch its own "
        "stripe of a distributed query straight from the source. Saving an "
        "existing name replaces it. The token is never returned by any "
        "surface (list_connections and system.connections omit it).",
        create_connection_schema(),
        create_connection_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_connection"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_connection});

    registry.add(Capability{
        "list_connections",
        "List saved connections",
        "The saved source connections, credentials REDACTED — name, connector "
        "settings, and when each was saved. Same source as "
        "system.connections.",
        list_connections_schema(),
        list_connections_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Orchestration, "list_connections"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_connections});

    registry.add(Capability{
        "delete_connection",
        "Delete a saved connection",
        "Remove a saved source connection (and its stored credential) from "
        "every node. Returns removed=false when no connection had that name.",
        delete_connection_schema(),
        delete_connection_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_connection"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_connection});

    registry.add(Capability{
        "create_dataset_from_query",
        "Create a dataset from a connector query",
        "Run a read-only query against an external source (Snowflake) and load "
        "its rows into a new partitioned table placed across the cluster — the "
        "in-product path from a warehouse to a featurizable dataset, with no "
        "manual export. Only a single SELECT/WITH may run. Reaches the source "
        "over the network with your credentials, so it is consent-gated "
        "(audited as dataset_added; visible in system.datasets).",
        create_dataset_from_query_schema(),
        create_dataset_from_query_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_dataset_from_query"},
        /*mutates=*/true,
        /*requires_consent=*/true,
        handle_create_dataset_from_query});

    registry.add(Capability{
        "promote_dataset",
        "Promote a cached dataset to durable",
        "Raise a dataset's replication factor (default 3, clamped to the node "
        "count) so a connector result set that landed as a cache at RF=1 stops "
        "being one copy with no backup. Adds owners to each partition in the "
        "catalog; the bodies then move to them over the existing large-object "
        "transport. Never removes an owner, so it can only make a dataset "
        "safer. Visible in system.dataset_partitions.",
        promote_dataset_schema(),
        promote_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "promote_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_promote_dataset});

    registry.add(Capability{
        "testsuite_run",
        "Run a test-suite case",
        "Start one in-app test-suite case in the background and return its "
        "run id immediately (poll get_run — the case's log lines land with "
        "the terminal status). List the cases with testsuite_list / "
        "system.test_cases.",
        testsuite_run_schema(),
        testsuite_run_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "testsuite_run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_testsuite_run});

    registry.add(Capability{
        "testsuite_list",
        "List test-suite cases",
        "Read the whole in-app test-suite catalog — one entry per runnable "
        "case (id, category, name, description, tunable options). Feed a case "
        "id to testsuite_run. Also queryable as system.test_cases. No side "
        "effects.",
        testsuite_list_schema(),
        testsuite_list_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_testsuite_list});

    // The spec index (#646, docs/api/specs-index.md): discovery of every
    // schema the cluster speaks, served from the C++ accessors. The handlers
    // capture the registry they live in — safe: the registry is populated at
    // startup, then only read, and outlives its own entries.
    registry.add(Capability{
        "list_specs",
        "List specs",
        "Read the spec index: the OpenAPI document plus every JSON Schema "
        "this node speaks — capability input/output schemas and all "
        "schema-as-code boundary schemas — as names, kinds, and links. No "
        "side effects.",
        list_specs_schema(),
        list_specs_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        [&registry](const nlohmann::json&, ApiContext&) {
            return ApiResult::success(spec_index(registry));
        }});

    registry.add(Capability{
        "get_spec",
        "Get one spec",
        "Read one spec document by its list_specs name — 'openapi', "
        "'<capability>.input' / '<capability>.output', or a boundary schema "
        "name like 'run'. No side effects.",
        get_spec_schema(),
        get_spec_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        [&registry](const nlohmann::json& input, ApiContext&) {
            const std::string name = input.at("name").get<std::string>();
            auto spec = find_spec(registry, name);
            if (!spec) {
                return ApiResult::failure(
                    "not_found",
                    "unknown spec '" + name +
                        "' — call list_specs for the index",
                    "/name");
            }
            return ApiResult::success(std::move(*spec));
        }});

    // The cloud-touching tools + the consent protocol tools (#621) — the
    // consent-gated slice, in its own TU (cloud_capabilities.cpp).
    register_cloud_capabilities(registry);
}

}  // namespace zetta::api
