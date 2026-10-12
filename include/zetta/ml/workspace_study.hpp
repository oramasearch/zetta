#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ml/study.hpp"

namespace zetta {
class Node;
}

// Experiments (studies) over a workspace methodology's parameters (lake#689).
//
// A workspace methodology (`W-<8 hex>`, workspace_methodology.hpp) declares its
// parameters in its manifest: kind, default, and bounds or choices. A study over
// one samples those parameters with the study's sampler, but the engine never
// runs the Python (ADR-17): each admitted trial is a Run that waits for Lake's
// runner. Lake reads the waiting trials from the study's `runner` work items,
// runs each as a training run in its sandbox, and reports back with
// `record_methodology_run` (`study`, `trial`), which completes the trial and
// admits the next ones; a run that failed or was stopped ends its trial through
// `stop_trial` with the reason. The native StudyDriver never drives these
// studies. See docs/ml/workspace-methodologies.md#experiments.
namespace zetta::ml {

// A study whose methodology is a workspace methodology: Lake's runner runs it.
bool is_workspace_study(const StudySpec& spec);

// What a workspace methodology publishes for studies: its searchable
// parameters (a numeric one needs both bounds; bool is the choices
// false/true), the defaults of every declared parameter, the objective
// metrics (`held_out_<m>`) and the default objective.
struct WorkspaceStudyDefinition {
    std::vector<StudyParameter> parameters;
    nlohmann::json defaults = nlohmann::json::object();
    std::vector<std::string> objective_metrics;
    StudyObjective default_objective;
    // Why the methodology cannot be studied; empty when it can.
    std::string reason;
};
WorkspaceStudyDefinition workspace_study_definition(const nlohmann::json& entry);

// The listing fields `list_methodologies` and `get_methodology` carry for a
// workspace entry: `studies`, `study_reason` and `study_search_space` (null
// when it cannot be studied: not yet validated for its code, or no parameter
// with bounds or choices).
nlohmann::json workspace_study_listing_fields(const nlohmann::json& entry);

// Whether a metric name is better smaller (a loss or an error).
bool metric_is_minimized(std::string_view metric);

// Check and normalize a study spec against its workspace methodology's entry:
// a runnable entry; the objective a `held_out_` metric it reports, on the
// `validation` split (its held-out rows); every searched parameter declared,
// of its declared kind (bool as categorical), within its declared bounds and
// choices; no pruner (a runner trial reports no intermediate values); no
// differential privacy. Fills an empty search space from the definition.
// Returns "/path: reason" on a problem.
std::optional<std::string> normalize_workspace_study_spec(const nlohmann::json& entry,
                                                          StudySpec& spec);

// Admit waiting trials up to the budgets (max_trials, max_parallel_trials,
// cost_cap) and end the study when its wall time is reached or no trial is
// left to run. Each trial is a Run with status Running and the sampled
// parameters (with `study.id`, `study.seed`); it waits for Lake's runner.
// Two trials in a row failing with the same reason fail the study. Runs on
// the node's owner thread. Returns how many trials it admitted.
std::size_t advance_workspace_study(Node& node, Study& study);

// End a trial with the result Lake's runner reported: complete with its
// objective, or failed with the reason; sets its Run's status and publishes
// the trial's event on `study-events.<id>`. Runs on the owner thread.
bool finish_workspace_trial(Node& node, Study& study, const std::string& run,
                            std::optional<double> objective, double cost, std::string reason,
                            std::string model);

// The trials waiting for Lake's runner, one work item each: study, study name,
// trial (its Run id), number, methodology, params (the declared defaults
// overridden by the sampled values), seed, dataset_path, target, features,
// holdout_fraction, and project and graph for a pipeline study.
nlohmann::json workspace_study_runner_work(Node& node, const Study& study);

// The JSON Schema of one runner work item.
const nlohmann::json& workspace_study_work_schema();

}  // namespace zetta::ml
