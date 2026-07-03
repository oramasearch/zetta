#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/labels.hpp"
#include "gui/tones.hpp"

#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/pipeline/validation.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

}  // namespace

void App::draw_gate() {
    ImGui::SeparatorText("Correctness gate");
    ImGui::TextDisabled(
        "Validate an execution graph before you deploy. No deploy without a "
        "passing gate.");

    // Graph source. Fill a preset, load a stored graph from the selected
    // project, or edit the IR JSON directly below. ir::from_json validates the
    // document at the boundary, so a malformed graph is rejected with a reason.
    if (ImGui::SmallButton("Passing example")) {
        gate_graph_input_ = kExampleGraphJson;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Empty (fails)")) {
        gate_graph_input_ = kEmptyGraphJson;
    }

    // Load a graph from the selected project's stored graphs (the real
    // ProjectGraph data path), when the project has any.
    if (const auto project = node_.find_project(selected_project_)) {
        const std::vector<std::string> graph_names = project->graph_names();
        if (!graph_names.empty()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo("##loadgraph", "Load from project…")) {
                for (const std::string& name : graph_names) {
                    if (ImGui::Selectable(name.c_str())) {
                        if (const auto graph = project->find_graph(name)) {
                            gate_graph_input_ =
                                ir::to_json(graph->graph()).dump(2);
                        }
                    }
                }
                ImGui::EndCombo();
            }
        }
    }

    ImGui::InputTextMultiline("##gategraph", &gate_graph_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8));

    if (primary_button("Run gate")) {
        gate_error_.clear();
        gate_report_.reset();
        // Parse + validate at the boundary without exceptions: a malformed
        // document yields a reason we show inline, never a thrown stack trace.
        auto document = nlohmann::json::parse(gate_graph_input_, nullptr,
                                              /*allow_exceptions=*/false);
        if (document.is_discarded()) {
            gate_error_ = "invalid JSON, could not parse the graph";
        } else {
            std::string parse_error;
            std::optional<ir::ExecutionGraph> graph =
                ir::from_json(document, &parse_error);
            if (!graph) {
                gate_error_ = "invalid graph: " + parse_error;
            } else {
                gate_report_ = std::make_unique<zetta::pipeline::ValidationReport>(
                    zetta::pipeline::default_gate().run(*graph));
            }
        }
    }

    banner(Tone::Error, gate_error_);

    if (!gate_report_) return;
    const zetta::pipeline::ValidationReport& report = *gate_report_;

    // The block-on-fail verdict is the headline state this panel exists to
    // show: the status pill (the header form, #636) plus the reason.
    ImGui::Spacing();
    if (report.block_deploy) {
        std::size_t failures = 0;
        for (const zetta::pipeline::CheckResult& check : report.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) ++failures;
        }
        status_pill(Tone::Error, "blocked");
        ImGui::SameLine();
        ImGui::TextColored(tone_color(Tone::Error),
                           "Deploy blocked: %zu check(s) failed (verdict: %s)",
                           failures, to_string(report.verdict()).c_str());
    } else {
        status_pill(Tone::Ok, "allowed");
        ImGui::SameLine();
        ImGui::TextColored(tone_color(Tone::Ok),
                           "Deploy allowed: gate passed (verdict: %s)",
                           to_string(report.verdict()).c_str());
    }

    // Per-check results: the check, its pass/warn/fail status, and the
    // human-readable reason (never a stack trace).
    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("gate_checks", 3, table_flags)) {
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch,
                                0.15f);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch,
                                0.55f);
        ImGui::TableHeadersRow();
        for (const zetta::pipeline::CheckResult& check : report.checks) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(check.name.c_str());
            ImGui::TableSetColumnIndex(1);
            status_label(tone_of(check.status),
                         std::string{label_of(check.status)}.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", check.reason.c_str());
        }
        ImGui::EndTable();
    }
    if (report.checks.empty()) {
        ImGui::TextDisabled("(the gate ran no checks)");
    }
}

void App::draw_deploy() {
    // Consume a navigate_to(entity) deep link aimed at this view (#633): a
    // deployment id (a run's "deployment:" link) or a deployed graph name (an
    // audit/activity target) — the deployments table highlights the match.
    if (auto pending_entity = take_pending_selection(View::Deploy)) {
        selected_deployment_ = *pending_entity;
    }
    ImGui::SeparatorText("Deploy");
    ImGui::TextDisabled(
        "Pick an AI solution, choose where to run it, and deploy. The "
        "correctness gate must pass first; no deploy without it.");

    const zetta::pipeline::SolutionCatalog& catalog = zetta::pipeline::SolutionCatalog::builtin();

    // 1) Solution picker — the AI Solution Library blueprints (the GUI's source
    //    of deployable graphs until a graph editor lands). Selection is by id.
    const zetta::pipeline::Solution* selected = catalog.find(selected_solution_);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##solution",
                          selected ? selected->name.c_str()
                                   : "Select an AI solution...")) {
        for (const zetta::pipeline::Solution& solution : catalog.solutions()) {
            const bool is_selected = solution.id == selected_solution_;
            if (ImGui::Selectable(solution.name.c_str(), is_selected)) {
                selected_solution_ = solution.id;
                deploy_status_.clear();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s · %s", solution.category.c_str(),
                                  solution.summary.c_str());
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    selected = catalog.find(selected_solution_);
    if (selected) {
        ImGui::TextDisabled("%s · %zu steps · %zu edges",
                            selected->category.c_str(),
                            selected->graph.node_count(),
                            selected->graph.edge_count());
    }

    // 2) Placement dial — the local→kubernetes→vpc→cloud progression. R1 ships
    //    Local only; the rest are shown disabled and labelled "(planned)" so the
    //    dial communicates the roadmap without being choosable. The combo index
    //    mirrors DeploymentKind's declaration order; deploy still guards on Local.
    static constexpr std::array<zetta::pipeline::DeploymentKind, 4> kDial{
        zetta::pipeline::DeploymentKind::Local, zetta::pipeline::DeploymentKind::Kubernetes, zetta::pipeline::DeploymentKind::Vpc,
        zetta::pipeline::DeploymentKind::Cloud};
    const auto dial_label = [](zetta::pipeline::DeploymentKind kind) -> std::string {
        const std::string label{label_of(kind)};
        return kind == zetta::pipeline::DeploymentKind::Local ? label : label + " (planned)";
    };
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##placement",
                          dial_label(kDial[deploy_placement_]).c_str())) {
        for (int index = 0; index < static_cast<int>(kDial.size()); ++index) {
            const bool functional = kDial[index] == zetta::pipeline::DeploymentKind::Local;
            ImGui::BeginDisabled(!functional);
            const bool is_selected = index == deploy_placement_;
            if (ImGui::Selectable(dial_label(kDial[index]).c_str(),
                                  is_selected)) {
                deploy_placement_ = index;
                deploy_status_.clear();
            }
            ImGui::EndDisabled();
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // 3) One-button deploy: run the correctness gate (RULE-1 — no deploy without
    //    a passing gate), then place the blueprint onto the chosen target.
    ImGui::BeginDisabled(selected == nullptr);
    if (primary_button("Deploy")) {
        const ir::ExecutionGraph& graph = selected->graph;
        const zetta::pipeline::ValidationReport report = zetta::pipeline::default_gate().run(graph);
        if (report.block_deploy) {
            std::string reason;
            for (const zetta::pipeline::CheckResult& check : report.checks) {
                if (check.status == zetta::pipeline::CheckStatus::Fail) {
                    reason = check.name + ": " + check.reason;
                    break;
                }
            }
            deploy_failed_ = true;
            deploy_status_ = "Gate blocked deploy: " +
                             (reason.empty() ? "graph failed validation" : reason);
        } else {
            const zetta::pipeline::DeploymentTarget target{kDial[deploy_placement_], {}, {}};
            const std::shared_ptr<zetta::pipeline::Deployment> deployment =
                node_.deploy_graph(graph, target, selected->name);
            if (deployment) {
                deploy_failed_ = false;
                deploy_status_ = "Deployed \"" + selected->name + "\" to " +
                                 std::string{label_of(target.kind)} +
                                 " (deployment " +
                                 deployment->id().substr(0, 8) + ")";
            } else {
                deploy_failed_ = true;
                deploy_status_ =
                    target.is_local()
                        ? "Deploy failed: the gate passed but placement was "
                          "rejected"
                        : "Deploy failed: only the local target is available "
                          "for now";
            }
        }
    }
    ImGui::EndDisabled();
    banner(deploy_failed_ ? Tone::Error : Tone::Ok, deploy_status_);

    // 3b) Run over the cluster — the churn solution actually trains a Model, ranks
    //     the at-risk accounts, and explains the drivers (FEAT-4 / UC-1, #398) on the
    //     node's LIVE JobCoordinator (the #579 compute spine over the real cluster, not
    //     an isolated loopback — #580). Only the churn blueprint maps to the real churn
    //     pipeline today, so the action is offered only for it; it runs over a built-in
    //     demo dataset. The run executes OFF the UI thread (#624): while in
    //     flight the panel renders live progress from the run's own records
    //     (the Run + its jobs — the same rows system.runs / system.jobs serve)
    //     and offers a cooperative cancel.
    if (selected != nullptr && selected->id == "churn-prediction") {
        ImGui::Spacing();
        const bool churn_running = churn_run_ != nullptr;
        ImGui::BeginDisabled(churn_running);
        if (primary_button("Run over the cluster")) {
            run_churn_demo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(
            "trains a real model on a built-in demo dataset, over the live cluster");
        if (churn_running) {
            draw_churn_run_progress();
        } else {
            // The finished run's outcome, toned by how it ended (#632).
            banner(churn_run_failed_ ? Tone::Error : Tone::Ok,
                   churn_run_status_);
        }
    }

    // 4) Deployment status — the recorded placements (acceptance: "see its
    //    status"). Sourced from the replicated deployments() list. The graph
    //    and latest-run cells are entity links (#633): the graph opens in the
    //    workspace, the run in the Runs view; a deep-linked deployment (a
    //    run's "deployment:" link, an audit target) highlights its row.
    ImGui::SeparatorText("Deployments");
    const std::vector<std::shared_ptr<zetta::pipeline::Deployment>> deployments =
        node_.deployments();
    if (deployments.empty()) {
        // The next action is the picker directly above, so the empty state
        // carries no button of its own (#631).
        empty_state("No deployments yet",
                    "Pick an AI solution above, choose where to run it, and "
                    "deploy: the recorded placement lands here.",
                    "");
        return;
    }
    const std::vector<std::shared_ptr<zetta::pipeline::Run>> runs = node_.runs();
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("deployments", 7, table_flags)) {
        ImGui::TableSetupColumn("Deployment");
        ImGui::TableSetupColumn("Graph");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Steps");
        ImGui::TableSetupColumn("Edges");
        ImGui::TableSetupColumn("Latest run");
        ImGui::TableSetupColumn("Created");
        ImGui::TableHeadersRow();
        if (mono_font_) ImGui::PushFont(mono_font_);
        for (const std::shared_ptr<zetta::pipeline::Deployment>& deployment : deployments) {
            ImGui::PushID(deployment->id().c_str());
            ImGui::TableNextRow();
            // The deep-linked deployment gets the coral "you are here" wash —
            // matched by id or by the deployed graph's name (audit targets).
            if (deployment->id() == selected_deployment_ ||
                deployment->graph_name() == selected_deployment_) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(kCoralWash));
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(
                deployment->id().substr(0, deployment->id().find('-')).c_str());
            ImGui::TableSetColumnIndex(1);
            entity_link(deployment->graph_name().c_str(), View::Graph,
                        deployment->graph_name());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                std::string{label_of(deployment->target().kind)}.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", deployment->node_count());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%zu", deployment->edge_count());
            ImGui::TableSetColumnIndex(5);
            // The newest run this deployment's controller started (a run ties
            // itself back through run.deployment) — the deployment → run hop.
            std::shared_ptr<zetta::pipeline::Run> latest;
            for (const std::shared_ptr<zetta::pipeline::Run>& run : runs) {
                if (!run || run->deployment() != deployment->id()) continue;
                if (!latest || run->created() > latest->created()) latest = run;
            }
            if (latest) {
                entity_link(latest->id().substr(0, latest->id().find('-')).c_str(),
                            View::Runs, latest->id());
            } else {
                ImGui::TextDisabled("·");
            }
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(deployment->created().c_str());
            ImGui::PopID();
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::EndTable();
    }
}

void App::run_churn_demo() {
    const zetta::pipeline::Solution* selected = zetta::pipeline::SolutionCatalog::builtin().find(selected_solution_);
    if (selected == nullptr || selected->id != "churn-prediction") {
        churn_run_failed_ = true;
        churn_run_status_ = "Select the churn solution to run it.";
        return;
    }
    if (churn_run_) return;  // one churn run at a time (the button is disabled)

    zetta::usecases::churn::ChurnSolutionOptions options;
    options.project = "deploy-panel";
    options.graph = "churn-prediction";

    // Run over the node's **live** JobCoordinator — the long-lived compute spine the
    // running node owns over its real Cluster (#579/#580), the ONE churn run entry
    // point (#637). On a cluster with peers the runner co-launches first (#601): a
    // run.launch task fans out over the signal plane and every member co-runs its
    // own disjoint shard, so the featurize / train / explain / rank jobs combine
    // over the real cluster membership, and the produced Model + jobs + Run land on
    // the live cluster (so the Runs / Models / Jobs / Admin panels reflect them).
    // OFF the UI thread (#624): AsyncChurnRun drives the pipeline on a core
    // worker whose node writes gate through Node::run_on_owner, so this panel
    // keeps rendering — live progress comes from the Run + jobs records
    // themselves, and poll_background_ops() applies the outcome to the status
    // line below. The headed app always attaches a cluster; if none is attached
    // the runner fails with a clear reason, surfaced like any failed run.
    churn_run_status_.clear();
    churn_run_failed_ = false;
    churn_run_from_deploy_ = true;
    churn_run_from_onboarding_ = false;
    churn_run_ = zetta::usecases::churn::AsyncChurnRun::start(
        node_, selected->graph, zetta::usecases::churn::demo_churn_dataset(),
        options);
}

void App::draw_churn_run_progress() {
    // Everything rendered here comes from the run's OWN records on the node —
    // the Run (status + logs) created by the in-flight pipeline — the same
    // rows system.runs serves, so this panel and an MCP caller polling SQL
    // watch one source (#624). The Run appears a beat after launch (its
    // creation gates through the owner thread), so tolerate its absence.
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      churn_run_->started())
            .count();
    char headline[96];
    std::snprintf(headline, sizeof(headline),
                  "Running the churn solution · %.0fs", elapsed);
    status_label(Tone::Active, headline);

    std::shared_ptr<zetta::pipeline::Run> run;
    for (const std::shared_ptr<zetta::pipeline::Run>& candidate : node_.runs()) {
        if (candidate && candidate->project() == churn_run_->project()) {
            run = candidate;  // runs are sorted by (created, id): newest wins
        }
    }
    if (run) {
        if (!run->logs().empty()) {
            if (mono_font_) ImGui::PushFont(mono_font_);
            ImGui::TextColored(kInkSecondary, "%s",
                               run->logs().back().message.c_str());
            if (mono_font_) ImGui::PopFont();
        }
        entity_link("view run", View::Runs, run->id());
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(churn_run_->cancel_requested());
    if (ImGui::Button("Cancel run")) {
        // Cooperative: the in-flight step completes, the remaining steps are
        // skipped, and the run ends Failed with a "run cancelled" log line.
        churn_run_->request_cancel();
    }
    ImGui::EndDisabled();
    if (churn_run_->cancel_requested()) {
        ImGui::SameLine();
        ImGui::TextColored(kInkQuiet, "cancelling after the current step…");
    }
}

}  // namespace zetta::gui
