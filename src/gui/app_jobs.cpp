#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"
#include "gui/labels.hpp"
#include "gui/tones.hpp"

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/scaling_policy.hpp"
#include "zetta/node.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {


}  // namespace

void App::reset_scaling_controls() {
    scaling_objective_ = 0;          // minimize_cost
    scaling_min_nodes_ = 1;
    scaling_max_nodes_ = 8;
    scaling_cost_cap_ = 0.0;
    scaling_target_seconds_ = 600.0;
    scaling_nodes_ = 2;
    scaling_progress_ = 0.4;
    scaling_elapsed_ = 120.0;
    scaling_cost_ = 50.0;
}

void App::draw_job_scaling_controls() {
    // Per-job objective/scaling controls — a LIVE PREVIEW over the built
    // ScalingPolicy::evaluate(). It is a "what-if" calculator, not a write path:
    // ScalingPolicy is not persisted and the autoscaler is unbuilt (both later
    // slices), so this scores an editable policy + sample with the real, pure
    // evaluate() and shows the resulting decision — set the reward objective +
    // bounds, add/remove nodes, watch the recommendation move. Nothing here
    // mutates the job.
    ImGui::SeparatorText("Objective & scaling (preview)");
    ImGui::TextDisabled(
        "Score a reward objective against a sample; no autoscaler is wired yet, "
        "so this previews the scaling decision; it doesn't change the job.");

    // 1) Objective — "set the reward". The three reward models the autoscaler can
    //    optimize, in declaration order (the combo index maps to ScalingObjective).
    static constexpr std::array<zetta::compute::ScalingObjective, 3> kObjectives{
        zetta::compute::ScalingObjective::MinimizeCost, zetta::compute::ScalingObjective::RuntimeTarget,
        zetta::compute::ScalingObjective::DataShape};
    if (scaling_objective_ < 0 ||
        scaling_objective_ >= static_cast<int>(kObjectives.size())) {
        scaling_objective_ = 0;
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo(
            "Reward objective",
            std::string(label_of(kObjectives[scaling_objective_])).c_str())) {
        for (int index = 0; index < static_cast<int>(kObjectives.size());
             ++index) {
            const bool is_selected = index == scaling_objective_;
            if (ImGui::Selectable(
                    std::string(label_of(kObjectives[index])).c_str(),
                    is_selected)) {
                scaling_objective_ = index;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const zetta::compute::ScalingObjective objective = kObjectives[scaling_objective_];

    // 2) Bounds — the hard envelope the autoscaler must stay inside (min/max node
    //    count + a cost cap; 0 = unlimited). A runtime target only matters for the
    //    RuntimeTarget objective, so it is shown only then.
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Min nodes", &scaling_min_nodes_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max nodes", &scaling_max_nodes_);
    if (scaling_min_nodes_ < 1) scaling_min_nodes_ = 1;
    if (scaling_max_nodes_ < scaling_min_nodes_) {
        scaling_max_nodes_ = scaling_min_nodes_;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Cost cap (0 = unlimited)", &scaling_cost_cap_, 0.0, 0.0,
                       "%.1f");
    if (scaling_cost_cap_ < 0.0) scaling_cost_cap_ = 0.0;
    if (objective == zetta::compute::ScalingObjective::RuntimeTarget) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputDouble("Target seconds", &scaling_target_seconds_, 0.0, 0.0,
                           "%.1f");
        if (scaling_target_seconds_ < 0.0) scaling_target_seconds_ = 0.0;
    }

    // 3) Sample — the live signals the autoscaler scores (progress, elapsed,
    //    cost) plus the current allocation. "Add/remove nodes" are the +/- buttons
    //    that move the sample's node count within the bounds.
    ImGui::Spacing();
    ImGui::TextDisabled("Allocated nodes");
    ImGui::SameLine();
    if (ImGui::SmallButton("- node") && scaling_nodes_ > 0) --scaling_nodes_;
    ImGui::SameLine();
    ImGui::Text("%d", scaling_nodes_);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ node")) ++scaling_nodes_;
    if (scaling_nodes_ < 0) scaling_nodes_ = 0;

    ImGui::SetNextItemWidth(220.0f);
    float progress = static_cast<float>(scaling_progress_);
    if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f, "%.2f")) {
        scaling_progress_ = progress;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Elapsed seconds", &scaling_elapsed_, 0.0, 0.0, "%.1f");
    if (scaling_elapsed_ < 0.0) scaling_elapsed_ = 0.0;
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Accrued cost", &scaling_cost_, 0.0, 0.0, "%.1f");
    if (scaling_cost_ < 0.0) scaling_cost_ = 0.0;

    // 4) Score it — the real, pure decision. reward (higher is better),
    //    recommended node count, the scale up/hold/down action relative to the
    //    sample, whether the cost cap holds, and the human-readable reason.
    zetta::compute::ScalingPolicy policy;
    policy.objective = objective;
    policy.bounds.min_nodes = static_cast<std::uint32_t>(scaling_min_nodes_);
    policy.bounds.max_nodes = static_cast<std::uint32_t>(scaling_max_nodes_);
    policy.bounds.cost_cap = scaling_cost_cap_;
    policy.target_seconds = scaling_target_seconds_;
    const zetta::compute::ScalingSample sample{scaling_progress_, scaling_elapsed_, scaling_cost_,
                               static_cast<std::uint32_t>(scaling_nodes_)};
    const zetta::compute::ScalingDecision decision = policy.evaluate(sample);

    ImGui::SeparatorText("Decision");
    ImGui::Text("recommended nodes: %u", decision.recommended_nodes);
    ImGui::SameLine();
    ImGui::TextColored(tone_color(tone_of(decision.action)), "(%s)",
                       std::string(label_of(decision.action)).c_str());
    ImGui::Text("reward: %.4g", decision.reward);
    if (!decision.within_cost_cap) {
        ImGui::SameLine();
        ImGui::TextColored(tone_color(Tone::Error), "  cost cap breached");
    }
    ImGui::TextWrapped("%s", decision.reason.c_str());
}

void App::draw_jobs() {
    page_header("Compute jobs",
                "Native distributed compute: lifecycle, references, and the "
                "scaling preview");

    // The GUI form of system.jobs: the live compute-job registry (one row
    // per unit of native distributed accelerated compute — featurize / train /
    // explain / score / a DDF op — with its lifecycle status). Read-only over the
    // replicated, already-validated ComputeJob handles, like the Runs panel —
    // creating/scheduling a job is a later slice. The detail pane adds the per-job
    // objective/scaling preview (draw_job_scaling_controls).
    const std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs = node_.jobs();
    std::optional<std::string> pending = take_pending_selection(View::Jobs);
    if (jobs.empty()) {
        if (empty_state("No compute jobs yet",
                        "Feature builds, training, explanation, and scoring "
                        "land here when a solution runs over the cluster.",
                        "Open deploy")) {
            navigate_to(View::Deploy);
        }
        return;
    }
    ImGui::TextDisabled("%zu job(s)", jobs.size());

    // Jobs have no human name, so each row's title is the UUID's first segment
    // with the lifecycle status as the dot and `kind · status` as meta (the
    // status word rides along — meaning never rides on colour alone).
    std::vector<MasterRow> rows;
    rows.reserve(jobs.size());
    for (const std::shared_ptr<zetta::compute::ComputeJob>& job : jobs) {
        rows.push_back({.id = job->id(),
                        .title = entity_short_id(job->id()),
                        .meta = std::string(label_of(job->kind())) + " · " +
                                std::string(label_of(job->status())),
                        .tone = tone_of(job->status()),
                        .id_title = true});
    }

    master_detail(
        "jobs", rows, selected_job_, std::move(pending), "(no jobs)",
        "Select a job to view its status, references, and scaling controls.",
        [&](const MasterRow& row) {
            const std::shared_ptr<zetta::compute::ComputeJob> job = node_.find_job(row.id);
            if (!job) return;
            // Reset the scaling controls whenever the selection changes, so
            // they start from sensible defaults per job rather than leaking
            // the previous job's edits.
            if (scaling_loaded_job_ != job->id()) {
                reset_scaling_controls();
                scaling_loaded_job_ = job->id();
            }

            detail_header("Job " + job->id(), tone_of(job->status()),
                          std::string(label_of(job->status())));
            // The human kind first; the raw wire name (what SQL and the API
            // return) stays one line down in the mono strip.
            ImGui::TextUnformatted(std::string(label_of(job->kind())).c_str());
            mono_line(kInkQuiet, "kind:     ", std::string(to_string(job->kind())));
            mono_line(kInkQuiet, "created:  ", job->created());
            if (job->updated() != job->created()) {
                mono_line(kInkQuiet, "updated:  ", job->updated());
            }
            if (!job->detail().empty()) {
                ImGui::TextWrapped("detail: %s", job->detail().c_str());
            }

            // Id references — the job's place in the pipeline (project / graph
            // / run / scaling-policy reward). Shown only when set; a ref that
            // resolves to a live entity is a one-click hop to its panel
            // (#633), an unresolvable one stays plain mono.
            if (!job->project().empty()) {
                const std::string project_name =
                    project_name_for(job->project());
                if (project_name.empty()) {
                    mono_line(kInkQuiet, "project:  ", job->project());
                } else {
                    link_line("project:  ", project_name, View::Explorer,
                              project_name);
                }
            }
            if (!job->graph().empty()) {
                if (graph_is_stored(job->graph())) {
                    link_line("graph:    ", job->graph(), View::Graph,
                              job->graph());
                } else {
                    mono_line(kInkQuiet, "graph:    ", job->graph());
                }
            }
            if (!job->run().empty()) {
                if (node_.find_run(job->run())) {
                    link_line("run:      ", job->run(), View::Runs, job->run());
                } else {
                    mono_line(kInkQuiet, "run:      ", job->run());
                }
            }
            if (!job->scaling_policy().empty()) {
                mono_line(kInkQuiet, "policy:   ", job->scaling_policy());
            }

            draw_job_scaling_controls();
        });
}

}  // namespace zetta::gui
