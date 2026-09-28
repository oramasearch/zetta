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
#include "zetta/pipeline/gate.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/timestamp.hpp"

#include "zetta/solutions/solution_execution.hpp"

namespace zetta::solutions {
using api::ApiContext;
using api::ApiResult;
using api::BackgroundActions;
namespace {

struct SolutionStage {
    const zetta::ml::Methodology* methodology = nullptr;
    zetta::ir::ExecutionGraph graph;
};

struct SolutionState {
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
    nlohmann::json binding;
    std::vector<SolutionStage> stages;
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

void update_master(const std::shared_ptr<SolutionState>& state,
                   const std::function<void(zetta::pipeline::Run&)>& update) {
    state->node->run_on_owner([&] {
        if (auto run = state->node->find_run(state->master_run_id)) {
            update(*run);
        }
    });
}

ApiResult call_stage(const std::shared_ptr<SolutionState>& state,
                     const SolutionStage& stage, int stage_number) {
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
        if (stage.methodology->id == "M-4") {
            request.input.erase("dataset_path");
            request.input.erase("dataset");
        }
        result = stage.methodology->run(request);
    });
    return result;
}

struct RunSnapshot {
    bool found = false;
    zetta::pipeline::RunStatus status = zetta::pipeline::RunStatus::Pending;
    std::vector<zetta::pipeline::RunOutput> outputs;
};

RunSnapshot read_run(const std::shared_ptr<SolutionState>& state,
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

void fail_master(const std::shared_ptr<SolutionState>& state, const std::string& reason) {
    update_master(state, [&](zetta::pipeline::Run& run) {
        run.add_log({zetta::util::now_iso8601(), "error", reason});
        run.set_status(zetta::pipeline::RunStatus::Failed);
    });
}

zetta::runtime::task<> run_stages(std::shared_ptr<SolutionState> state) {
    std::vector<zetta::pipeline::RunOutput> outputs;
    std::vector<std::string> lineage_steps = {"snapshot"};
    std::vector<zetta::pipeline::RunOutput> scored_outputs;
    std::vector<zetta::pipeline::RunOutput> supporting_outputs;
    ApiResult prepared;
    state->node->run_on_owner([&] {
        prepared = prepare_solution_snapshot(*state->node, state->binding);
    });
    if (!prepared.ok) {
        fail_master(state, prepared.error.message);
        co_return;
    }
    state->input["dataset_path"] = prepared.value.at("dataset_path");
    state->input["dataset"] = state->binding.at("dataset");
    state->input["solution_snapshot_as_of"] = state->binding.at("snapshot").at("as_of");
    outputs.push_back({"dataset", "solution snapshot", prepared.value.at("dataset_path")});
    for (std::size_t index = 0; index < state->stages.size(); ++index) {
        const SolutionStage& stage = state->stages[index];
        const std::string stage_name = stage.methodology->id + " " +
                                       stage.methodology->name;
        update_master(state, [&](zetta::pipeline::Run& run) {
            run.add_log({zetta::util::now_iso8601(), "info",
                         "solution stage started: " + stage_name});
        });
        const ApiResult launched =
            call_stage(state, stage, static_cast<int>(index + 1));
        if (!launched.ok || !launched.value.contains("run_id")) {
            const std::string reason =
                launched.ok ? "methodology returned no run_id"
                            : launched.error.message;
            fail_master(state, stage_name + " failed: " + reason);
            co_return;
        }
        const std::string stage_run_id =
            launched.value.at("run_id").get<std::string>();
        while (true) {
            const RunSnapshot snapshot = read_run(state, stage_run_id);
            if (snapshot.found &&
                snapshot.status == zetta::pipeline::RunStatus::Succeeded) {
                if (stage.methodology->id == state->binding.at("decision_source").at("methodology").get<std::string>()) {
                    scored_outputs = snapshot.outputs;
                }
                if (state->binding.value("solution", std::string{}) ==
                        "S-12" &&
                    stage.methodology->id == "M-11") {
                    for (const zetta::pipeline::RunOutput& output :
                         snapshot.outputs) {
                        if (output.kind == "model") {
                            supporting_outputs.push_back(output);
                        }
                    }
                }
                const std::string solution_id =
                    state->binding.value("solution", std::string{});
                if ((solution_id == "S-1" || solution_id == "S-3" || solution_id == "S-4") &&
                    state->binding.value("level", 1) >= 3 &&
                    stage.methodology->id == "M-6") {
                    for (const zetta::pipeline::RunOutput& output :
                         snapshot.outputs) {
                        if (output.kind == "dataset" &&
                            output.name == "treatment effects") {
                            scored_outputs.push_back(output);
                        }
                    }
                }
                outputs.insert(outputs.end(), snapshot.outputs.begin(),
                               snapshot.outputs.end());
                break;
            }
            if (snapshot.found &&
                snapshot.status == zetta::pipeline::RunStatus::Failed) {
                fail_master(state, stage_name + " failed; inspect Run " + stage_run_id);
                co_return;
            }
            [[maybe_unused]] auto delayed =
                co_await zetta::runtime::delay(std::chrono::milliseconds(10));
        }
        lineage_steps.push_back(stage_name + " run " + stage_run_id);
        update_master(state, [&](zetta::pipeline::Run& run) {
            run.add_log({zetta::util::now_iso8601(), "info",
                         "solution stage succeeded: " + stage_name});
        });
    }
    scored_outputs.insert(scored_outputs.end(), supporting_outputs.begin(),
                          supporting_outputs.end());
    ApiResult finished;
    state->node->run_on_owner([&] {
        finished = finish_solution_run(*state->node, state->binding,
                                      state->master_run_id, scored_outputs,
                                      state->actor.empty() ? "local" : state->actor);
    });
    if (!finished.ok) {
        fail_master(state, finished.error.message);
        co_return;
    }
    outputs.push_back({"dataset", "solution decisions", finished.value.at("decisions")});
    for (const std::string step : {"policy", "holdout", "actions", "ledger", "monitor"}) {
        lineage_steps.push_back(step);
    }
    update_master(state, [&](zetta::pipeline::Run& run) {
        run.set_outputs(std::move(outputs));
        zetta::pipeline::RunLineage lineage;
        lineage.sources = {state->input.at("dataset_path").get<std::string>(),
                           state->binding.at("snapshot").dump()};
        lineage.steps = std::move(lineage_steps);
        lineage.freshness = zetta::util::now_iso8601();
        lineage.confidence = 0.0;
        run.set_lineage(std::move(lineage));
        run.add_log({zetta::util::now_iso8601(), "info",
                     "solution foundation and methodology stages completed"});
        run.set_status(zetta::pipeline::RunStatus::Succeeded);
    });
}

class AsyncComposedSolution {
public:
    explicit AsyncComposedSolution(std::shared_ptr<SolutionState> state)
        : state_(std::move(state)) {}

    static std::shared_ptr<AsyncComposedSolution>
    start(std::shared_ptr<SolutionState> state) {
        auto handle =
            std::shared_ptr<AsyncComposedSolution>(new AsyncComposedSolution(state));
        zetta::runtime::spawn(run_stages(state), [state] {
            state->done.store(true, std::memory_order_release);
        });
        return handle;
    }

    bool done() const {
        return state_->done.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<SolutionState> state_;
};

} // namespace

api::ApiResult run_composed_solution(const nlohmann::json& input, ApiContext& context) {
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
    if (!manifest || !manifest->attributes.contains("foundation")) {
        return ApiResult::failure(
            "invalid_input", "run_solution requires a graph created by create_solution");
    }
    const std::string binding_error = validate_solution_binding(manifest->attributes.at("foundation"));
    if (!binding_error.empty()) {
        return ApiResult::failure("invalid_input", "solution foundation " + binding_error);
    }
    const auto correctness = zetta::pipeline::default_gate().run(graph->graph());
    if (!correctness.passed()) {
        for (const auto& check : correctness.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) {
                return ApiResult::failure("invalid_input", "solution correctness gate: " + check.reason);
            }
        }
    }
    const nlohmann::json methodology_ids =
        manifest->attributes.value("methodologies", nlohmann::json::array());
    std::vector<SolutionStage> stages;
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
    for (const auto& existing : context.node.runs()) {
        if (existing->value().graph == graph->id() &&
            (existing->status() == zetta::pipeline::RunStatus::Pending ||
             existing->status() == zetta::pipeline::RunStatus::Running)) {
            return ApiResult::failure("invalid_input", "a Run for this solution graph is already active");
        }
    }
    const std::shared_ptr<zetta::pipeline::Run> master =
        context.node.create_run(project->id(), graph->id());
    if (!master) {
        return ApiResult::failure("engine_error", "could not create the solution Run");
    }
    master->set_status(zetta::pipeline::RunStatus::Running);
    master->add_log({zetta::util::now_iso8601(), "info",
                     "running snapshot, methodologies and governed solution foundation"});

    auto state = std::make_shared<SolutionState>();
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
    state->input.erase("delivery_token");
    state->binding = manifest->attributes.at("foundation");
    state->binding["delivery_token"] = input.value("delivery_token", "");
    state->input["sources"] = manifest->attributes.value(
        "sources", nlohmann::json::object());
    state->stages = std::move(stages);
    const std::shared_ptr<AsyncComposedSolution> handle =
        AsyncComposedSolution::start(std::move(state));
    context.background->adopt(handle, [worker = handle.get()] {
        return worker->done();
    });
    return ApiResult::success({{"run_id", master->id()},
                               {"engine", "solution"},
                               {"status", "running"},
                               {"preview", false}});
}


} // namespace zetta::solutions
