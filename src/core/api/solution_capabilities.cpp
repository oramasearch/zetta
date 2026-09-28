#include "zetta/api/capabilities.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetta/api/background.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/solution.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

struct PreviewStage {
    const zetta::ml::Methodology* methodology = nullptr;
    zetta::ir::ExecutionGraph graph;
};

struct PreviewState {
    zetta::Node* node = nullptr;
    const zetta::cluster::Cluster* cluster = nullptr;
    BackgroundActions* background = nullptr;
    zetta::observability::SpanRecorder* recorder = nullptr;
    std::string actor;
    bool actor_is_user = false;
    std::string project_name;
    std::string graph_id;
    std::string master_run_id;
    nlohmann::json input;
    std::vector<PreviewStage> stages;
    std::atomic<bool> done = false;
};

std::shared_ptr<zetta::pipeline::Project>
resolve_project(ApiContext& context, const std::string& name_or_id) {
    return context.node.find_project(name_or_id);
}

std::shared_ptr<zetta::pipeline::Project>
resolve_project(zetta::Node& node, const std::string& name_or_id) {
    return node.find_project(name_or_id);
}

std::shared_ptr<zetta::pipeline::ProjectGraph>
resolve_graph(zetta::pipeline::Project& project,
              const std::string& name_or_id) {
    if (auto graph = project.find_graph(name_or_id)) {
        return graph;
    }
    return project.find_graph_by_id(name_or_id);
}

void update_master(const std::shared_ptr<PreviewState>& state,
                   const std::function<void(zetta::pipeline::Run&)>& update) {
    state->node->run_on_owner([&] {
        if (auto run = state->node->find_run(state->master_run_id)) {
            update(*run);
        }
    });
}

ApiResult call_stage(const std::shared_ptr<PreviewState>& state,
                     const PreviewStage& stage, int stage_number) {
    ApiResult result;
    state->node->run_on_owner([&] {
        const std::shared_ptr<zetta::pipeline::Project> project =
            resolve_project(*state->node, state->project_name);
        if (!project) {
            result = ApiResult::failure(
                "not_found", "the solution's project no longer exists");
            return;
        }
        zetta::pipeline::ProjectGraph stage_graph(
            state->graph_id, project->id(),
            "solution stage " + std::to_string(stage_number), stage.graph, {});
        ApiContext context{*state->node,
                           state->cluster,
                           state->actor,
                           {},
                           state->recorder,
                           state->background};
        context.acting_is_user = state->actor_is_user;
        zetta::ml::RunRequest request;
        request.context = &context;
        request.project = project.get();
        request.graph = &stage_graph;
        request.input = state->input;
        result = stage.methodology->run(request);
    });
    return result;
}

struct RunSnapshot {
    bool found = false;
    zetta::pipeline::RunStatus status = zetta::pipeline::RunStatus::Pending;
    std::vector<zetta::pipeline::RunOutput> outputs;
};

RunSnapshot read_run(const std::shared_ptr<PreviewState>& state,
                     const std::string& run_id) {
    RunSnapshot snapshot;
    state->node->run_on_owner([&] {
        if (const auto run = state->node->find_run(run_id)) {
            snapshot.found = true;
            snapshot.status = run->status();
            snapshot.outputs = run->outputs();
        }
    });
    return snapshot;
}

zetta::runtime::task<> run_preview(std::shared_ptr<PreviewState> state) {
    std::vector<zetta::pipeline::RunOutput> outputs;
    std::vector<std::string> lineage_steps;
    for (std::size_t index = 0; index < state->stages.size(); ++index) {
        const PreviewStage& stage = state->stages[index];
        const std::string stage_name = stage.methodology->id + " " +
                                       stage.methodology->name;
        update_master(state, [&](zetta::pipeline::Run& run) {
            run.add_log({zetta::util::now_iso8601(), "info",
                         "preview stage started: " + stage_name});
        });
        const ApiResult launched =
            call_stage(state, stage, static_cast<int>(index + 1));
        if (!launched.ok || !launched.value.contains("run_id")) {
            const std::string reason =
                launched.ok ? "methodology returned no run_id"
                            : launched.error.message;
            update_master(state, [&](zetta::pipeline::Run& run) {
                run.add_log({zetta::util::now_iso8601(), "error",
                             stage_name + " failed: " + reason});
                run.set_status(zetta::pipeline::RunStatus::Failed);
            });
            co_return;
        }
        const std::string stage_run_id =
            launched.value.at("run_id").get<std::string>();
        while (true) {
            const RunSnapshot snapshot = read_run(state, stage_run_id);
            if (snapshot.found &&
                snapshot.status == zetta::pipeline::RunStatus::Succeeded) {
                outputs.insert(outputs.end(), snapshot.outputs.begin(),
                               snapshot.outputs.end());
                break;
            }
            if (snapshot.found &&
                snapshot.status == zetta::pipeline::RunStatus::Failed) {
                update_master(state, [&](zetta::pipeline::Run& run) {
                    run.add_log({zetta::util::now_iso8601(), "error",
                                 stage_name + " failed; inspect Run " +
                                     stage_run_id});
                    run.set_status(zetta::pipeline::RunStatus::Failed);
                });
                co_return;
            }
            [[maybe_unused]] auto delayed =
                co_await zetta::runtime::delay(std::chrono::milliseconds(10));
        }
        lineage_steps.push_back(stage_name + " run " + stage_run_id);
        update_master(state, [&](zetta::pipeline::Run& run) {
            run.add_log({zetta::util::now_iso8601(), "info",
                         "preview stage succeeded: " + stage_name});
        });
    }
    update_master(state, [&](zetta::pipeline::Run& run) {
        run.set_outputs(std::move(outputs));
        zetta::pipeline::RunLineage lineage;
        lineage.sources = {state->input.value("sources", nlohmann::json::object()).dump()};
        lineage.steps = std::move(lineage_steps);
        lineage.freshness = zetta::util::now_iso8601();
        lineage.confidence = 0.0;
        run.set_lineage(std::move(lineage));
        run.add_log({zetta::util::now_iso8601(), "warn",
                     "methodology preview complete; production foundation "
                     "components were not executed"});
        run.set_status(zetta::pipeline::RunStatus::Succeeded);
    });
}

class AsyncSolutionPreview {
public:
    explicit AsyncSolutionPreview(std::shared_ptr<PreviewState> state)
        : state_(std::move(state)) {}

    static std::shared_ptr<AsyncSolutionPreview>
    start(std::shared_ptr<PreviewState> state) {
        auto handle =
            std::shared_ptr<AsyncSolutionPreview>(new AsyncSolutionPreview(state));
        zetta::runtime::spawn(run_preview(state), [state] {
            state->done.store(true, std::memory_order_release);
        });
        return handle;
    }

    bool done() const {
        return state_->done.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<PreviewState> state_;
};

const nlohmann::json& empty_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}};
    return schema;
}

const nlohmann::json& string_array_schema() {
    static const nlohmann::json schema = {
        {"type", "array"}, {"items", {{"type", "string"}}}};
    return schema;
}

const nlohmann::json& composition_entry_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"methodology", "role", "minimum_level"}},
        {"additionalProperties", false},
        {"properties",
         {{"methodology", {{"type", "string"}}},
          {"role", {{"type", "string"}}},
          {"minimum_level", {{"type", "integer"}}}}}};
    return schema;
}

const nlohmann::json& autonomy_default_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"action_class", "autonomy"}},
        {"additionalProperties", false},
        {"properties",
         {{"action_class", {{"type", "string"}}},
          {"autonomy", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& data_role_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"role", "required", "description"}},
        {"additionalProperties", false},
        {"properties",
         {{"role", {{"type", "string"}}},
          {"required", {{"type", "boolean"}}},
          {"description", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& capability_level_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"level", "name", "description"}},
        {"additionalProperties", false},
        {"properties",
         {{"level", {{"type", "integer"}}},
          {"name", {{"type", "string"}}},
          {"description", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& missing_component_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id", "name", "issue"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"issue", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& solution_item_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id", "name", "promise", "use_case", "composition",
                       "pipeline_order",
                       "action_patterns", "latency_classes", "autonomy_defaults",
                       "required_data_roles", "decision_policy_template",
                       "measurement_design", "guardrails", "capability_levels",
                       "missing_components", "status", "runnable", "issue",
                       "promotion_condition"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"promise", {{"type", "string"}}},
          {"use_case", {{"type", "string"}}},
          {"composition",
           {{"type", "array"}, {"items", composition_entry_schema()}}},
          {"pipeline_order", string_array_schema()},
          {"action_patterns", string_array_schema()},
          {"latency_classes", string_array_schema()},
          {"autonomy_defaults",
           {{"type", "array"}, {"items", autonomy_default_schema()}}},
          {"required_data_roles",
           {{"type", "array"}, {"items", data_role_schema()}}},
          {"decision_policy_template", {{"type", "string"}}},
          {"measurement_design", {{"type", "string"}}},
          {"guardrails", string_array_schema()},
          {"capability_levels",
           {{"type", "array"}, {"items", capability_level_schema()}}},
          {"missing_components",
           {{"type", "array"}, {"items", missing_component_schema()}}},
          {"status", {{"type", "string"}}},
          {"runnable", {{"type", "boolean"}}},
          {"issue", {{"type", "string"}}},
          {"promotion_condition", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& list_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"solutions"}},
        {"additionalProperties", false},
        {"properties",
         {{"solutions", {{"type", "array"}, {"items", solution_item_schema()}}}}}};
    return schema;
}

const nlohmann::json& get_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"solution"}},
        {"additionalProperties", false},
        {"properties", {{"solution", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& get_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"solution"}},
        {"additionalProperties", false},
        {"properties", {{"solution", solution_item_schema()}}}};
    return schema;
}

const nlohmann::json& create_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "solution", "level", "sources"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}, {"minLength", 1}}},
          {"solution", {{"type", "string"}, {"minLength", 1}}},
          {"level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
          {"sources",
           {{"type", "object"},
            {"description", "common callback hints plus optional M-id overrides"}}},
          {"name", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& create_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "solution", "level", "graph", "preview",
                       "methodologies", "missing_components"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}}},
          {"solution", {{"type", "string"}}},
          {"level", {{"type", "integer"}}},
          {"graph", {{"type", "string"}}},
          {"preview", {{"type", "boolean"}}},
          {"methodologies", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"missing_components",
           {{"type", "array"}, {"items", missing_component_schema()}}}}}};
    return schema;
}

const nlohmann::json& run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}, {"minLength", 1}}},
          {"graph", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "engine", "status", "preview"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id", {{"type", "string"}}},
          {"engine", {{"type", "string"}}},
          {"status", {{"type", "string"}}},
          {"preview", {{"type", "boolean"}}}}}};
    return schema;
}

ApiResult handle_list(const nlohmann::json&, ApiContext&) {
    nlohmann::json entries = nlohmann::json::array();
    for (const zetta::ml::Solution& solution : zetta::ml::solutions()) {
        entries.push_back(zetta::ml::solution_json(solution));
    }
    return ApiResult::success({{"solutions", std::move(entries)}});
}

ApiResult handle_get(const nlohmann::json& input, ApiContext&) {
    const std::string id = input.at("solution").get<std::string>();
    const zetta::ml::Solution* solution = zetta::ml::find_solution(id);
    if (solution == nullptr) {
        return ApiResult::failure("not_found", "no solution named '" + id + "'");
    }
    return ApiResult::success({{"solution", zetta::ml::solution_json(*solution)}});
}

ApiResult handle_create(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Project> project =
        resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found", "no project named '" + project_name + "'");
    }
    const std::string id = input.at("solution").get<std::string>();
    const zetta::ml::Solution* solution = zetta::ml::find_solution(id);
    if (solution == nullptr) {
        return ApiResult::failure("not_found", "no solution named '" + id + "'");
    }
    const int level = input.at("level").get<int>();
    zetta::ml::ComposedSolution composed =
        zetta::ml::compose_solution(*solution, level, input.at("sources"));
    if (!composed.ok()) {
        return ApiResult::failure("not_supported", composed.error);
    }
    const std::string name =
        input.value("name", solution->id + " CL-" + std::to_string(level) + " preview");
    const std::shared_ptr<zetta::pipeline::ProjectGraph> graph =
        project->create_graph(name, std::move(composed.graph));
    if (!graph) {
        return ApiResult::failure(
            "invalid_input", "project already has a graph named '" + name + "'");
    }
    nlohmann::json missing = nlohmann::json::array();
    for (const zetta::ml::MissingSolutionComponent& component :
         composed.missing_components) {
        missing.push_back({{"id", component.id},
                           {"name", component.name},
                           {"issue", component.issue}});
    }
    return ApiResult::success({{"project", project->name()},
                               {"solution", solution->id},
                               {"level", level},
                               {"graph", graph->name()},
                               {"preview", true},
                               {"methodologies", composed.methodologies},
                               {"missing_components", std::move(missing)}});
}

ApiResult handle_run(const nlohmann::json& input, ApiContext& context) {
    if (context.background == nullptr) {
        return ApiResult::failure(
            "engine_error", "run_solution needs a background action owner");
    }
    const std::string project_name = input.at("project").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Project> project =
        resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found", "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const std::shared_ptr<zetta::pipeline::ProjectGraph> graph =
        resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name + "'");
    }
    const std::shared_ptr<zetta::ir::Node> manifest =
        graph->graph().find_node("solution_manifest");
    if (!manifest || !manifest->attributes.value("preview", false)) {
        return ApiResult::failure(
            "invalid_input", "run_solution requires a graph created by create_solution");
    }
    const nlohmann::json methodology_ids =
        manifest->attributes.value("methodologies", nlohmann::json::array());
    std::vector<PreviewStage> stages;
    for (std::size_t index = 0; index < methodology_ids.size(); ++index) {
        const std::string methodology_id = methodology_ids[index].get<std::string>();
        const zetta::ml::Methodology* methodology =
            zetta::ml::find_methodology(methodology_id);
        if (methodology == nullptr || !methodology->runnable || !methodology->run) {
            const std::string reason =
                methodology == nullptr
                    ? "no registered methodology for " + methodology_id
                    : zetta::ml::unsupported_methodology_message(*methodology);
            return ApiResult::failure("not_supported", reason);
        }
        zetta::ir::ExecutionGraph stage_graph =
            zetta::ml::solution_stage_graph(graph->graph(),
                                            static_cast<int>(index + 1));
        if (stage_graph.empty()) {
            return ApiResult::failure(
                "not_supported", "the composed graph is missing stage " +
                                     std::to_string(index + 1) + " (" +
                                     methodology_id + ") for issue " +
                                     std::string("https://github.com/oramasearch/zetta/issues/1102"));
        }
        stages.push_back({methodology, std::move(stage_graph)});
    }
    const std::shared_ptr<zetta::pipeline::Run> master =
        context.node.create_run(project->id(), graph->id());
    if (!master) {
        return ApiResult::failure("engine_error", "could not create the solution Run");
    }
    master->set_status(zetta::pipeline::RunStatus::Running);
    master->add_log({zetta::util::now_iso8601(), "warn",
                     "running methodology-only preview; missing production "
                     "foundation components remain deferred"});

    auto state = std::make_shared<PreviewState>();
    state->node = &context.node;
    state->cluster = context.cluster;
    state->background = context.background;
    state->recorder = context.recorder;
    state->actor = context.acting_key;
    state->actor_is_user = context.acting_is_user;
    state->project_name = project->name();
    state->graph_id = graph->id();
    state->master_run_id = master->id();
    state->input = input;
    state->input["sources"] = manifest->attributes.value(
        "sources", nlohmann::json::object());
    state->stages = std::move(stages);
    const std::shared_ptr<AsyncSolutionPreview> handle =
        AsyncSolutionPreview::start(std::move(state));
    context.background->adopt(handle, [worker = handle.get()] {
        return worker->done();
    });
    return ApiResult::success({{"run_id", master->id()},
                               {"engine", "solution_preview"},
                               {"status", "running"},
                               {"preview", true}});
}

} // namespace

void register_solution_capabilities(Registry& registry) {
    registry.add(Capability{
        "list_solutions", "List solutions",
        "Read all twelve catalogue solutions and eight Band-B sketches, including composition, data roles, governance, capability levels, and runnable status. No side effects.",
        empty_schema(), list_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false, handle_list});
    registry.add(Capability{
        "get_solution", "Get solution",
        "Read one catalogue solution or sketch by id, including every production dependency and tracking issue. No side effects.",
        get_schema(), get_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false, handle_get});
    registry.add(Capability{
        "create_solution", "Create solution preview",
        "Compose a selected solution level through methodology registry callbacks and store its replicated preview Pipeline on the project; missing production components are returned.",
        create_schema(), create_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_solution"}, true,
        false, handle_create});
    registry.add(Capability{
        "run_solution", "Run solution preview",
        "Run a composed preview's runnable methodology stages in order and return a durable Run with stage lineage; absent production foundation components are explicitly skipped.",
        run_schema(), run_output_schema(), CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run_solution"}, true,
        false, handle_run});
}

} // namespace zetta::api
