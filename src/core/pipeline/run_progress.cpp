#include "zetta/pipeline/run_progress.hpp"

#include "zetta/ir/step_kind.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace zetta::pipeline {
namespace {

// A step's log/lineage label — mirrors the executor's step_label (its name
// when it has one, else its id), because that is exactly what the log lines
// and output names carry.
std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

}  // namespace

std::string_view to_string(StepRunStatus status) {
    switch (status) {
        case StepRunStatus::Pending:   return "pending";
        case StepRunStatus::Running:   return "running";
        case StepRunStatus::Succeeded: return "succeeded";
        case StepRunStatus::Failed:    return "failed";
    }
    return "pending";
}

RunProgress project_run_progress(const ir::ExecutionGraph& graph,
                                 const RunValue& run) {
    RunProgress progress;
    progress.run_status = run.status;
    progress.steps.reserve(graph.node_count());

    // The exact markers the GraphExecutor writes for each step: its start line
    // ("<kind>: <label>") and its failure prefix ("<kind> step '<label>'
    // failed: "). Indexed once so the log walk below is O(logs).
    std::unordered_map<std::string, std::size_t> step_of_start_marker;
    std::unordered_map<std::string, std::size_t> step_of_label;
    std::vector<std::string> failure_prefixes;
    for (const std::shared_ptr<ir::Node>& step : graph.nodes()) {
        const std::size_t index = progress.steps.size();
        const std::string label = step_label(*step);
        const std::string kind{ir::to_string(step->kind)};
        step_of_start_marker.emplace(kind + ": " + label, index);
        step_of_label.emplace(label, index);
        failure_prefixes.push_back(kind + " step '" + label + "' failed: ");
        progress.steps.push_back(StepRunProgress{step->id});
    }

    // Walk the log once: record which steps started (and in what order) and
    // which failed with what reason. `last_error` is the fallback pinned
    // reason for a run that failed before any step ran (empty graph, cycle).
    std::vector<bool> started(progress.steps.size(), false);
    std::size_t last_started = progress.steps.size();  // size() = none yet
    std::string last_error;
    for (const RunLogEntry& entry : run.logs) {
        if (const auto found = step_of_start_marker.find(entry.message);
            found != step_of_start_marker.end()) {
            started[found->second] = true;
            last_started = found->second;
            continue;
        }
        if (entry.level == "error") {
            last_error = entry.message;
            for (std::size_t index = 0; index < failure_prefixes.size(); ++index) {
                const std::string& prefix = failure_prefixes[index];
                if (entry.message.rfind(prefix, 0) == 0) {
                    progress.steps[index].status = StepRunStatus::Failed;
                    progress.steps[index].reason = entry.message.substr(prefix.size());
                    break;
                }
            }
        }
    }

    // Resolve each started, not-failed step: the last step to start is Running
    // while the run is still in flight; every earlier start already completed
    // (the executor runs steps strictly in order, fail-fast).
    for (std::size_t index = 0; index < progress.steps.size(); ++index) {
        StepRunProgress& step = progress.steps[index];
        if (step.status == StepRunStatus::Failed || !started[index]) continue;
        const bool in_flight = run.status == RunStatus::Running ||
                               run.status == RunStatus::Pending;
        step.status = (in_flight && index == last_started)
                          ? StepRunStatus::Running
                          : StepRunStatus::Succeeded;
    }

    // Attribute the run's outputs to the steps that produced them. Both
    // executors stamp the producing step's label into RunOutput::name; the
    // default executor's logical ref also carries the step id ("<kind>:<id>"),
    // which wins when labels collide.
    for (const RunOutput& output : run.outputs) {
        std::size_t target = progress.steps.size();
        for (std::size_t index = 0; index < progress.steps.size(); ++index) {
            const std::string& step_id = progress.steps[index].step_id;
            if (output.ref.size() > step_id.size() &&
                output.ref.compare(output.ref.size() - step_id.size() - 1,
                                   std::string::npos, ":" + step_id) == 0) {
                target = index;
                break;
            }
        }
        if (target == progress.steps.size()) {
            if (const auto found = step_of_label.find(output.name);
                found != step_of_label.end()) {
                target = found->second;
            }
        }
        if (target < progress.steps.size()) {
            progress.steps[target].outputs.push_back(output);
        }
    }

    // The pinned failure reason: the failed step's own reason when one exists,
    // else the run's last error line (a run can fail before any step runs).
    if (run.status == RunStatus::Failed) {
        for (const StepRunProgress& step : progress.steps) {
            if (step.status == StepRunStatus::Failed) {
                progress.failure_reason = step.reason;
                break;
            }
        }
        if (progress.failure_reason.empty()) progress.failure_reason = last_error;
    }
    return progress;
}

}  // namespace zetta::pipeline
