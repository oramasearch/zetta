#include "gui/app.hpp"
#include "gui/app_internal.hpp"

#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/graph_plan.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/data/table.hpp"
#include "zetta/pipeline/validation.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

// The gate status as an uppercase badge ("PASS" / "WARN" / "FAIL").
std::string status_badge(zetta::pipeline::CheckStatus status) {
    std::string text = to_string(status);
    for (char& character : text) {
        character =
            static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

// The text colour for a check status.
ImVec4 status_color(zetta::pipeline::CheckStatus status) {
    switch (status) {
        case zetta::pipeline::CheckStatus::Pass: return kPassColor;
        case zetta::pipeline::CheckStatus::Warn: return kWarnColor;
        case zetta::pipeline::CheckStatus::Fail: return kFailColor;
    }
    return kFailColor;
}

// Graph-workspace canvas palette (ImU32, for the window draw list) — light
// cards on the Orama canvas. Nodes are white boxes with hairline borders laid
// out in columns by StepKind; the selected node gets a coral border (the "you
// are here" accent). Edges are gray lines (data) / amber (control).
constexpr ImU32 kCanvasHeaderColor = IM_COL32(95, 95, 92, 255);      // ink-muted
constexpr ImU32 kNodeFill = IM_COL32(255, 255, 255, 255);           // white
constexpr ImU32 kNodeFillHover = IM_COL32(242, 242, 240, 255);      // wash
constexpr ImU32 kNodeFillSelected = IM_COL32(247, 238, 239, 255);   // coral wash
constexpr ImU32 kNodeBorder = IM_COL32(224, 224, 224, 255);         // border
constexpr ImU32 kNodeBorderSelected = IM_COL32(226, 84, 90, 255);   // coral
constexpr ImU32 kNodeTextColor = IM_COL32(28, 28, 26, 255);         // ink-primary
constexpr ImU32 kDataEdgeColor = IM_COL32(150, 150, 146, 220);      // gray line
constexpr ImU32 kControlEdgeColor = IM_COL32(184, 134, 44, 220);    // amber

// The colour a step's projected cluster placement is drawn in (#591) — semantic,
// within the Orama palette (coral stays the "you are here" accent, never status):
//   near-data  → green (compute runs where the data lives — the efficient case)
//   fan-out    → ink   (SPMD across every node — neutral, distributed compute)
//   all nodes  → quiet (a load whose result is replicated everywhere)
//   coordinator→ amber (a single-node funnel point — attention, not distributed)
ImVec4 placement_color(zetta::pipeline::StepPlacement placement) {
    switch (placement) {
        case zetta::pipeline::StepPlacement::NearData:    return kDotActive;
        case zetta::pipeline::StepPlacement::FanOut:      return kInkSecondary;
        case zetta::pipeline::StepPlacement::AllNodes:    return kInkQuiet;
        case zetta::pipeline::StepPlacement::Coordinator: return kWarnColor;
    }
    return kInkQuiet;
}

}  // namespace

void App::draw_gate() {
    ImGui::SeparatorText("Correctness gate");
    ImGui::TextDisabled(
        "Validate an execution graph before deploy. RULE-1: no deploy without a "
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

    if (!gate_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", gate_error_.c_str());
    }

    if (!gate_report_) return;
    const zetta::pipeline::ValidationReport& report = *gate_report_;

    // The block-on-fail banner is the headline state this panel exists to show.
    ImGui::Spacing();
    if (report.block_deploy) {
        std::size_t failures = 0;
        for (const zetta::pipeline::CheckResult& check : report.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) ++failures;
        }
        ImGui::TextColored(kFailColor,
                           "Deploy blocked: %zu check(s) failed (verdict: %s)",
                           failures, to_string(report.verdict()).c_str());
    } else {
        ImGui::TextColored(kPassColor,
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
            ImGui::TextColored(status_color(check.status), "%s",
                               status_badge(check.status).c_str());
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
        std::string label = to_string(kind);
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(label[0]));
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
                                 to_string(target.kind) + " (deployment " +
                                 deployment->id().substr(0, 8) + ")";
            } else {
                deploy_failed_ = true;
                deploy_status_ =
                    target.is_local()
                        ? "Deploy failed: the gate passed but placement was "
                          "rejected"
                        : "Deploy failed: only the local target is available "
                          "in R1";
            }
        }
    }
    ImGui::EndDisabled();
    if (!deploy_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(deploy_failed_ ? kErrorColor : kSuccessColor, "%s",
                           deploy_status_.c_str());
    }

    // 3b) Run over the cluster — the churn solution actually trains a Model, ranks
    //     the at-risk accounts, and explains the drivers (FEAT-4 / UC-1, #398) on the
    //     node's LIVE JobCoordinator (the #579 compute spine over the real cluster, not
    //     an isolated loopback — #580). Only the churn blueprint maps to the real churn
    //     pipeline today, so the action is offered only for it; it runs over a built-in
    //     demo dataset.
    if (selected != nullptr && selected->id == "churn-prediction") {
        ImGui::Spacing();
        if (primary_button("Run over the cluster")) {
            run_churn_demo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "trains a real model on a built-in demo dataset, over the live cluster");
        if (!churn_run_status_.empty()) {
            ImGui::TextColored(churn_run_failed_ ? kErrorColor : kSuccessColor,
                               "%s", churn_run_status_.c_str());
        }
    }

    // 4) Deployment status — the recorded placements (acceptance: "see its
    //    status"). Sourced from the replicated deployments() list.
    ImGui::SeparatorText("Deployments");
    const std::vector<std::shared_ptr<zetta::pipeline::Deployment>> deployments =
        node_.deployments();
    if (deployments.empty()) {
        ImGui::TextDisabled("(none yet)");
        return;
    }
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("deployments", 5, table_flags)) {
        ImGui::TableSetupColumn("Graph");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Steps");
        ImGui::TableSetupColumn("Edges");
        ImGui::TableSetupColumn("Created");
        ImGui::TableHeadersRow();
        if (mono_font_) ImGui::PushFont(mono_font_);
        for (const std::shared_ptr<zetta::pipeline::Deployment>& deployment : deployments) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(deployment->graph_name().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(to_string(deployment->target().kind).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", deployment->node_count());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", deployment->edge_count());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(deployment->created().c_str());
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

    compute::CpuDevice device;
    zetta::usecases::churn::ChurnSolutionOptions options;
    options.project = "deploy-panel";
    options.graph = "churn-prediction";

    // Run over the node's **live** JobCoordinator — the long-lived compute spine the
    // running node owns over its real Cluster (#579/#580). The featurize / train /
    // explain / rank jobs combine over the real cluster membership (single-node here
    // in the common headed deployment), and the produced Model + jobs + Run register
    // on the App's node_ (so the Runs / Models / Jobs / Admin panels reflect them).
    // Synchronous on the UI thread that owns the node — the same deliberate per-click
    // pattern as run_cloud_inspect.
    zetta::usecases::churn::ChurnSolutionResult result = run_churn_solution_on_node(
        node_, device, selected->graph, zetta::usecases::churn::demo_churn_dataset(), options);

    // Defensive fallback: a node with no compute plane attached (no cluster) can't run
    // over the live spine. The headed app always attaches a cluster, so this is just a
    // safety net — bring up a throwaway single-node loopback so the demo still works.
    if (node_.job_coordinator() == nullptr) {
        zetta::cluster::ClusterConfig compute_cfg;
        compute_cfg.listen_host = "127.0.0.1";
        compute_cfg.listen_port = 47000;
        compute_cfg.port_range = 64;  // bind the first free port (repeat clicks reuse)
        zetta::cluster::Cluster compute_cluster("churn-run", compute_cfg);
        compute_cluster.start();
        compute::JobCoordinator coordinator(compute_cluster, /*worker_count=*/2);
        coordinator.start();
        result = run_churn_solution(node_, coordinator, device, selected->graph,
                                    zetta::usecases::churn::demo_churn_dataset(), options);
        coordinator.stop();
    }

    if (!result.ok()) {
        churn_run_failed_ = true;
        churn_run_status_ = "Run failed: " +
                            (result.execution.error.empty()
                                 ? std::string("the churn pipeline did not complete")
                                 : result.execution.error);
        return;
    }
    churn_run_failed_ = false;
    std::string line = "Trained model " + result.model_id.substr(0, 8) + " · " +
                       std::to_string(result.ranked.size()) + " accounts ranked";
    if (!result.top_driver.empty()) line += " · top driver: " + result.top_driver;
    churn_run_status_ = line;
}

void App::open_graph(const zetta::pipeline::ProjectGraph& stored) {
    // A fresh working copy: edits apply here and only land in the IR on Save, so
    // an edit is a deliberate, atomic act (and Revert is just "drop this copy").
    editing_graph_ = std::make_unique<ir::ExecutionGraph>(stored.graph());
    selected_graph_ = stored.name();
    graph_loaded_id_ = stored.id();
    graph_dirty_ = false;
    selected_node_id_.clear();
    graph_error_.clear();
    rename_graph_input_ = stored.name();
    rename_node_input_.clear();
    add_node_name_input_.clear();
    add_edge_from_.clear();
    add_edge_to_.clear();
}

void App::draw_graph_workspace() {
    ImGui::SeparatorText("Graph workspace");

    // The workspace edits the *currently selected* project's graphs (the same
    // selection the explorer's Projects column drives). Re-resolved by name each
    // frame so a dropped/renamed project never leaves a dangling handle.
    const auto project = node_.find_project(selected_project_);
    if (!project) {
        editing_graph_.reset();
        graph_panel_project_.clear();
        selected_graph_.clear();
        ImGui::TextDisabled("Select a project to view its execution graphs.");
        return;
    }

    // When the selected project changes, drop any open working copy so we never
    // show one project's graph under another.
    if (graph_panel_project_ != project->id()) {
        graph_panel_project_ = project->id();
        editing_graph_.reset();
        selected_graph_.clear();
        graph_loaded_id_.clear();
        selected_node_id_.clear();
        graph_dirty_ = false;
        graph_error_.clear();
    }

    // --- graph list + create ----------------------------------------------
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool submit = ImGui::InputTextWithHint(
        "##newgraph", "new graph name", &new_graph_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("New graph") || submit) {
        if (new_graph_input_.empty()) {
            graph_error_ = "graph name required";
        } else if (auto created =
                       project->create_graph(new_graph_input_, ir::ExecutionGraph{})) {
            open_graph(*created);
            new_graph_input_.clear();
        } else {
            graph_error_ = "a graph by that name already exists";
        }
    }

    const std::vector<std::string> names = project->graph_names();
    if (names.empty()) {
        ImGui::TextDisabled("(this project has no graphs yet)");
    }
    for (const std::string& name : names) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            project->drop_graph(name);
            if (selected_graph_ == name) {
                editing_graph_.reset();
                selected_graph_.clear();
                graph_loaded_id_.clear();
                selected_node_id_.clear();
                graph_dirty_ = false;
            }
            ImGui::PopID();
            continue;  // this name is gone; don't draw a selectable for it
        }
        ImGui::SameLine();
        const bool is_open = editing_graph_ && selected_graph_ == name;
        if (ImGui::Selectable(name.c_str(), is_open)) {
            const auto stored = project->find_graph(name);
            // Re-clicking the open graph keeps unsaved edits; opening a
            // different one loads a fresh working copy (discarding edits).
            if (stored && stored->id() != graph_loaded_id_) open_graph(*stored);
        }
        ImGui::PopID();
    }

    if (!editing_graph_) {
        if (!graph_error_.empty()) {
            ImGui::TextColored(kErrorColor, "%s", graph_error_.c_str());
        }
        ImGui::TextDisabled("Select or create a graph to view and edit it.");
        return;
    }

    ImGui::Separator();

    // Cluster-plan overlay (#591): project how this graph would run over the live
    // cluster — which steps run near their data, which fan out, and the owner
    // nodes — and overlay it on the canvas + a plan section below. On by default so
    // opening a graph shows the projection; the checkbox hides it for a plain edit
    // view. The projection is rebuilt each frame from the IR + the partition
    // catalog + the live membership, so it tracks edits and joins/leaves live.
    ImGui::Checkbox("Cluster plan", &show_cluster_plan_);
    ImGui::SameLine();
    ImGui::TextDisabled("project this graph over the current cluster");
    const zetta::pipeline::ClusterPlan plan = show_cluster_plan_ ? build_graph_plan() : zetta::pipeline::ClusterPlan{};

    draw_graph_canvas(show_cluster_plan_ ? &plan : nullptr);
    if (show_cluster_plan_) draw_graph_plan(plan);
    draw_graph_editor(*project);
}

zetta::pipeline::ClusterPlan App::build_graph_plan() const {
    if (!editing_graph_) return zetta::pipeline::ClusterPlan{};

    // Members projected over = the live cluster (system.zetta_nodes / the spatial
    // Cluster view's source), so the plan tracks joins/leaves with no separate copy.
    std::vector<std::string> members;
    for (const zetta::cluster::NodeStatus& node : cluster_.nodes()) members.push_back(node.node_id);

    // Partition catalog (system.dataset_partitions): table id → the sorted union of
    // its partitions' owner nodes. A table absent here is not partitioned, so a step
    // reading it has no near-data locality (it falls to its kind's default).
    std::unordered_map<std::string, std::vector<std::string>> table_owners;
    for (const auto& partition : node_.dataset_partitions()) {
        std::vector<std::string>& owners = table_owners[partition->table()];
        for (const std::string& owner : partition->owner_nodes()) {
            const auto pos = std::lower_bound(owners.begin(), owners.end(), owner);
            if (pos == owners.end() || *pos != owner) owners.insert(pos, owner);
        }
    }

    // Resolve a step's "table" attribute (a name OR a tbl id) → a known table id.
    // Index both name→id and id→id over every table in every database, so either
    // form an authored graph uses resolves.
    std::unordered_map<std::string, std::string> table_ref_to_id;
    for (const std::string& db_name : node_.database_names()) {
        const auto database = node_.find_database(db_name);
        if (!database) continue;
        for (const std::string& tbl_name : database->table_names()) {
            const auto table = database->find_table(tbl_name);
            if (!table) continue;
            table_ref_to_id[table->name()] = table->id();
            table_ref_to_id[table->id()] = table->id();
        }
    }

    // Per step: if it directly reads a table that is partitioned, record that
    // table's owner nodes — the seed project_cluster_plan propagates downstream.
    std::unordered_map<std::string, std::vector<std::string>> direct_owners;
    for (const auto& node : editing_graph_->nodes()) {
        const auto attr = node->attributes.find("table");
        if (attr == node->attributes.end() || !attr->is_string()) continue;
        const auto resolved = table_ref_to_id.find(attr->get<std::string>());
        if (resolved == table_ref_to_id.end()) continue;
        const auto owners = table_owners.find(resolved->second);
        if (owners == table_owners.end() || owners->second.empty()) continue;
        direct_owners[node->id] = owners->second;
    }

    return zetta::pipeline::project_cluster_plan(*editing_graph_, members, direct_owners);
}

void App::draw_graph_canvas(const zetta::pipeline::ClusterPlan* plan) {
    ir::ExecutionGraph& graph = *editing_graph_;

    // Index the plan by step id so each node box can show its projected placement.
    std::unordered_map<std::string, const zetta::pipeline::StepPlan*> plan_of;
    if (plan != nullptr) {
        for (const zetta::pipeline::StepPlan& step : plan->steps) plan_of[step.node_id] = &step;
    }

    const float font = ImGui::GetFontSize();
    const float node_w = font * 9.0f;
    const float node_h = font * 2.4f;
    const float col_gap = font * 3.0f;
    const float row_gap = font * 0.9f;
    const float header_h = font * 1.8f;
    const float pad = font * 0.6f;
    const float rounding = font * 0.3f;

    if (ImGui::BeginChild("##graph_canvas", ImVec2(0, font * 18.0f),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (graph.empty()) {
            ImGui::TextDisabled("(empty graph; add a step below)");
        } else {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            // Pass 1: place each node in its StepKind column and remember its
            // rect + center (so edges can be drawn between centers underneath).
            struct Placed {
                ImVec2 top_left;
                ImVec2 bottom_right;
                std::shared_ptr<ir::Node> node;
            };
            std::vector<Placed> placed;
            std::unordered_map<std::string, ImVec2> center_of;
            float content_right = origin.x;
            float content_bottom = origin.y;

            const auto kinds = ir::all_step_kinds();
            for (std::size_t column = 0; column < kinds.size(); ++column) {
                const float x =
                    origin.x + static_cast<float>(column) * (node_w + col_gap);
                // Column header as a mono uppercase eyebrow (the StepKind), the
                // connect→…→act pipeline reading left to right.
                const std::string header =
                    to_upper(std::string{ir::to_string(kinds[column])});
                const ImVec2 header_pos(x, origin.y);
                if (mono_font_) {
                    draw->AddText(mono_font_, ImGui::GetFontSize() * 0.92f,
                                  header_pos, kCanvasHeaderColor, header.c_str());
                } else {
                    draw->AddText(header_pos, kCanvasHeaderColor, header.c_str());
                }
                float y = origin.y + header_h;
                for (const auto& node : graph.nodes_of_kind(kinds[column])) {
                    const ImVec2 top_left(x, y);
                    const ImVec2 bottom_right(x + node_w, y + node_h);
                    placed.push_back({top_left, bottom_right, node});
                    center_of[node->id] =
                        ImVec2(x + node_w * 0.5f, y + node_h * 0.5f);
                    y += node_h + row_gap;
                    content_bottom = std::max(content_bottom, y);
                }
                content_right = std::max(content_right, x + node_w);
            }

            // Pass 2: edges, clipped to each box's border with an arrowhead at
            // the `to` end so direction reads even when an edge points backward.
            const ImVec2 half(node_w * 0.5f, node_h * 0.5f);
            const auto border_point = [&](ImVec2 center, ImVec2 toward) {
                const ImVec2 d(toward.x - center.x, toward.y - center.y);
                if (d.x == 0.0f && d.y == 0.0f) return center;
                const float scale_x =
                    d.x != 0.0f ? half.x / std::abs(d.x) : FLT_MAX;
                const float scale_y =
                    d.y != 0.0f ? half.y / std::abs(d.y) : FLT_MAX;
                const float scale = std::min(scale_x, scale_y);
                return ImVec2(center.x + d.x * scale, center.y + d.y * scale);
            };
            for (const ir::Edge& edge : graph.edges()) {
                const auto from = center_of.find(edge.from);
                const auto to = center_of.find(edge.to);
                if (from == center_of.end() || to == center_of.end()) continue;
                const ImU32 color = edge.kind == ir::EdgeKind::Control
                                        ? kControlEdgeColor
                                        : kDataEdgeColor;
                const ImVec2 start = border_point(from->second, to->second);
                const ImVec2 end = border_point(to->second, from->second);
                draw->AddLine(start, end, color, font * 0.1f);
                ImVec2 dir(end.x - start.x, end.y - start.y);
                const float length =
                    std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (length > 0.001f) {
                    dir.x /= length;
                    dir.y /= length;
                    const ImVec2 normal(-dir.y, dir.x);
                    const float head = font * 0.55f;
                    const ImVec2 base(end.x - dir.x * head, end.y - dir.y * head);
                    draw->AddTriangleFilled(
                        end,
                        ImVec2(base.x + normal.x * head * 0.5f,
                               base.y + normal.y * head * 0.5f),
                        ImVec2(base.x - normal.x * head * 0.5f,
                               base.y - normal.y * head * 0.5f),
                        color);
                }
            }

            // Pass 3: node boxes + click-to-select (invisible buttons hit-test).
            for (const Placed& item : placed) {
                ImGui::SetCursorScreenPos(item.top_left);
                ImGui::InvisibleButton(item.node->id.c_str(),
                                       ImVec2(node_w, node_h));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    selected_node_id_ = item.node->id;
                    rename_node_input_ = item.node->name;
                }
                const bool selected = item.node->id == selected_node_id_;
                const ImU32 fill = selected ? kNodeFillSelected
                                            : (hovered ? kNodeFillHover : kNodeFill);
                draw->AddRectFilled(item.top_left, item.bottom_right, fill,
                                    rounding);
                draw->AddRect(item.top_left, item.bottom_right,
                              selected ? kNodeBorderSelected : kNodeBorder,
                              rounding, 0, selected ? 2.0f : 1.0f);
                draw->PushClipRect(item.top_left, item.bottom_right, true);

                // Cluster-plan overlay (#591): a coloured left stripe in the step's
                // projected placement colour + a mono badge under the name. The name
                // moves to the top so the badge fits below it; without a plan the
                // name stays vertically centred (plain edit view).
                const auto plan_it = plan_of.find(item.node->id);
                const zetta::pipeline::StepPlan* step_plan =
                    plan_it != plan_of.end() ? plan_it->second : nullptr;
                const float stripe_w = font * 0.22f;
                float name_x = item.top_left.x + pad;
                float name_y = item.top_left.y + (node_h - font) * 0.5f;
                if (step_plan != nullptr) {
                    const ImVec4 color = placement_color(step_plan->placement);
                    draw->AddRectFilled(
                        ImVec2(item.top_left.x + 1.0f, item.top_left.y + 1.0f),
                        ImVec2(item.top_left.x + stripe_w, item.bottom_right.y - 1.0f),
                        ImGui::GetColorU32(color));
                    name_x = item.top_left.x + stripe_w + pad * 0.5f;
                    name_y = item.top_left.y + font * 0.30f;
                    // Mono placement badge near the box bottom.
                    const float small = font * 0.78f;
                    const ImVec2 badge_pos(name_x,
                                           item.top_left.y + node_h - font * 0.30f -
                                               small);
                    const std::string badge{placement_label(step_plan->placement)};
                    if (mono_font_) {
                        draw->AddText(mono_font_, small, badge_pos,
                                      ImGui::GetColorU32(color), badge.c_str());
                    } else {
                        draw->AddText(badge_pos, ImGui::GetColorU32(color),
                                      badge.c_str());
                    }
                }
                draw->AddText(ImVec2(name_x, name_y), kNodeTextColor,
                              item.node->name.c_str());
                draw->PopClipRect();
            }

            // Reserve the laid-out area so the child scrolls to fit it.
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(content_right - origin.x + col_gap,
                                content_bottom - origin.y + row_gap));
        }
    }
    ImGui::EndChild();
}

void App::draw_graph_plan(const zetta::pipeline::ClusterPlan& plan) {
    const float font = ImGui::GetFontSize();

    ImGui::Spacing();
    eyebrow(mono_font_, "PLAN OVER CLUSTER");
    ImGui::TextWrapped(
        "Projected over the current %zu-node cluster: how each step would run "
        "(zetta's SPMD + compute-near-data model). A live projection of the IR + "
        "partition placement, not a committed schedule.",
        plan.members.size());

    // Legend — the four placement classes and what each means.
    const auto legend_item = [&](zetta::pipeline::StepPlacement placement, const char* meaning) {
        status_dot(placement_color(placement));
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(placement_color(placement), "%s",
                           std::string{placement_label(placement)}.c_str());
        if (mono_font_) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", meaning);
    };
    legend_item(zetta::pipeline::StepPlacement::NearData,
                "runs on the partition owners, near its data");
    legend_item(zetta::pipeline::StepPlacement::FanOut,
                "SPMD across every node; partials combine over collectives");
    legend_item(zetta::pipeline::StepPlacement::AllNodes, "a load replicated to every node");
    legend_item(zetta::pipeline::StepPlacement::Coordinator, "runs once on the coordinator");

    // The current cluster as a node strip. When a step is selected in the canvas
    // (or the table below), the nodes it is projected to run on get the coral
    // "runs here" treatment — the direct graph↔cluster link.
    const zetta::pipeline::StepPlan* selected_step = nullptr;
    for (const zetta::pipeline::StepPlan& step : plan.steps)
        if (step.node_id == selected_node_id_) selected_step = &step;
    const auto runs_here = [&](const std::string& node_id) {
        return selected_step != nullptr &&
               std::find(selected_step->target_nodes.begin(),
                         selected_step->target_nodes.end(),
                         node_id) != selected_step->target_nodes.end();
    };
    const auto short_id = [](const std::string& id) {
        return id.substr(0, id.find('-'));
    };
    const auto health_color = [](const zetta::cluster::NodeStatus& node) -> ImVec4 {
        if (node.is_self) return kDotActive;
        if (!node.replication.known) return kInkQuiet;
        return node.replication.in_sync ? kSuccessColor : kWarnColor;
    };

    ImGui::Spacing();
    const std::vector<zetta::cluster::NodeStatus> nodes = cluster_.nodes();
    const float box_w = font * 9.0f;
    const float box_h = font * 2.3f;
    const float gap = font * 0.6f;
    const float box_pad = font * 0.5f;
    const float rounding = font * 0.3f;
    if (ImGui::BeginChild("##plan_cluster_strip", ImVec2(0, font * 7.0f),
                          ImGuiChildFlags_Borders)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const int columns =
            std::max(1, static_cast<int>((avail + gap) / (box_w + gap)));
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const zetta::cluster::NodeStatus& node = nodes[index];
            const int row = static_cast<int>(index) / columns;
            const int col = static_cast<int>(index) % columns;
            const ImVec2 top_left(origin.x + col * (box_w + gap),
                                  origin.y + row * (box_h + gap));
            const ImVec2 bottom_right(top_left.x + box_w, top_left.y + box_h);
            const bool here = runs_here(node.node_id);
            draw->AddRectFilled(top_left, bottom_right,
                                here ? kNodeFillSelected : kNodeFill, rounding);
            draw->AddRect(top_left, bottom_right,
                          here ? kNodeBorderSelected : kNodeBorder, rounding, 0,
                          here ? 2.0f : 1.0f);
            draw->PushClipRect(top_left, bottom_right, true);
            const float dot_radius = font * 0.26f;
            const ImVec2 dot_center(top_left.x + box_pad + dot_radius,
                                    top_left.y + box_pad + font * 0.5f);
            draw->AddCircleFilled(dot_center, dot_radius,
                                  ImGui::GetColorU32(health_color(node)));
            const std::string id =
                short_id(node.node_id) + (node.is_self ? " (self)" : "");
            const ImVec2 id_pos(dot_center.x + dot_radius + font * 0.4f,
                                top_left.y + box_pad);
            if (mono_font_) {
                draw->AddText(mono_font_, font, id_pos, kNodeTextColor, id.c_str());
            } else {
                draw->AddText(id_pos, kNodeTextColor, id.c_str());
            }
            const ImVec2 addr_pos(top_left.x + box_pad,
                                  top_left.y + box_pad + font * 1.4f);
            if (mono_font_) {
                draw->AddText(mono_font_, font * 0.82f, addr_pos, kCanvasHeaderColor,
                              node.address.c_str());
            } else {
                draw->AddText(addr_pos, kCanvasHeaderColor, node.address.c_str());
            }
            draw->PopClipRect();
        }
        const int rows = (static_cast<int>(nodes.size()) + columns - 1) / columns;
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(avail, std::max(0, rows) * (box_h + gap)));
    }
    ImGui::EndChild();
    if (selected_step == nullptr) {
        ImGui::TextDisabled("Select a step to highlight the nodes it runs on.");
    }

    // Per-step placement table. Clicking a row selects that step (highlighting it
    // in the canvas + the strip above).
    ImGui::Spacing();
    if (plan.steps.empty()) {
        ImGui::TextDisabled("(add steps to project a plan)");
        return;
    }
    const auto targets_summary = [&](const zetta::pipeline::StepPlan& step) -> std::string {
        if (step.target_nodes.empty()) return "—";
        switch (step.placement) {
            case zetta::pipeline::StepPlacement::NearData: {
                std::string ids;
                for (std::size_t i = 0; i < step.target_nodes.size(); ++i) {
                    if (i) ids += ", ";
                    ids += short_id(step.target_nodes[i]);
                }
                return std::to_string(step.target_nodes.size()) + " owner(s) · " +
                       ids;
            }
            case zetta::pipeline::StepPlacement::FanOut:
            case zetta::pipeline::StepPlacement::AllNodes:
                return "all " + std::to_string(step.target_nodes.size()) +
                       " node(s)";
            case zetta::pipeline::StepPlacement::Coordinator:
                return short_id(step.target_nodes.front()) + " (coordinator)";
        }
        return "—";
    };

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("graph_plan_steps", 4, table_flags)) {
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 0.12f);
        ImGui::TableSetupColumn("Placement", ImGuiTableColumnFlags_WidthStretch,
                                0.16f);
        ImGui::TableSetupColumn("Runs on", ImGuiTableColumnFlags_WidthStretch,
                                0.42f);
        ImGui::TableHeadersRow();
        for (const zetta::pipeline::StepPlan& step : plan.steps) {
            const auto node = editing_graph_->find_node(step.node_id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(step.node_id.c_str());
            const bool is_selected = step.node_id == selected_node_id_;
            if (ImGui::Selectable(node ? node->name.c_str() : step.node_id.c_str(),
                                  is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_node_id_ = step.node_id;
                if (node) rename_node_input_ = node->name;
            }
            // Hovering the row explains the projection (the step's rationale).
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", step.rationale.c_str());
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s",
                                std::string{ir::to_string(step.kind)}.c_str());
            ImGui::TableSetColumnIndex(2);
            status_dot(placement_color(step.placement));
            ImGui::TextColored(placement_color(step.placement), "%s",
                               std::string{placement_label(step.placement)}.c_str());
            ImGui::TableSetColumnIndex(3);
            if (mono_font_) ImGui::PushFont(mono_font_);
            ImGui::TextUnformatted(targets_summary(step).c_str());
            if (mono_font_) ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

void App::draw_graph_editor(zetta::pipeline::Project& project) {
    ir::ExecutionGraph& graph = *editing_graph_;
    const float font = ImGui::GetFontSize();

    // --- summary, rename, save / revert -----------------------------------
    ImGui::Text("%zu step(s), %zu edge(s)", graph.node_count(),
                graph.edge_count());
    if (graph_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored(kUnsavedColor, "● unsaved edits");
    }

    ImGui::SetNextItemWidth(font * 12.0f);
    ImGui::InputTextWithHint("##rengraph", "graph name", &rename_graph_input_);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        // Persist the working copy back to the IR (disk + CRDT) via update_graph.
        // A name change in the field becomes a rename in the same call.
        std::string new_name = rename_graph_input_ == selected_graph_
                                   ? std::string{}
                                   : rename_graph_input_;
        if (auto updated =
                project.update_graph(selected_graph_, graph, std::move(new_name))) {
            selected_graph_ = updated->name();
            graph_loaded_id_ = updated->id();
            rename_graph_input_ = updated->name();
            graph_dirty_ = false;
            graph_error_.clear();
            io_status_ = "Saved graph '" + updated->name() + "' (" +
                         std::to_string(updated->graph().node_count()) +
                         " steps, " +
                         std::to_string(updated->graph().edge_count()) +
                         " edges)";
        } else {
            graph_error_ = "save failed: name empty or already taken";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        if (const auto stored = project.find_graph_by_id(graph_loaded_id_)) {
            open_graph(*stored);
        } else {
            editing_graph_.reset();
            selected_graph_.clear();
            graph_loaded_id_.clear();
        }
    }
    if (!graph_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", graph_error_.c_str());
    }
    if (!editing_graph_) return;  // Revert may have closed the graph

    // --- add a step --------------------------------------------------------
    ImGui::SeparatorText("Add a step");
    ImGui::SetNextItemWidth(font * 10.0f);
    ImGui::InputTextWithHint("##nodename", "step name", &add_node_name_input_);
    ImGui::SameLine();
    const auto kinds = ir::all_step_kinds();
    ImGui::SetNextItemWidth(font * 7.0f);
    if (ImGui::BeginCombo("##nodekind",
                          std::string{ir::to_string(kinds[add_node_kind_])}
                              .c_str())) {
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            const std::string label{ir::to_string(kinds[i])};
            if (ImGui::Selectable(label.c_str(),
                                  static_cast<int>(i) == add_node_kind_)) {
                add_node_kind_ = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add step")) {
        if (add_node_name_input_.empty()) {
            graph_error_ = "step name required";
        } else if (auto added = graph.add_node(kinds[add_node_kind_],
                                               add_node_name_input_)) {
            selected_node_id_ = added->id;
            rename_node_input_ = added->name;
            add_node_name_input_.clear();
            graph_dirty_ = true;
            graph_error_.clear();
        } else {
            graph_error_ = "could not add step";
        }
    }

    // A picker over the graph's nodes, labelled "<name> [<kind>]".
    const auto node_label = [](const ir::Node& node) {
        return node.name + "  [" + std::string{ir::to_string(node.kind)} + "]";
    };
    const auto node_picker = [&](const char* combo_id, const char* placeholder,
                                 std::string& selection) {
        const auto current = graph.find_node(selection);
        const std::string preview =
            current ? node_label(*current) : std::string{placeholder};
        ImGui::SetNextItemWidth(font * 11.0f);
        if (ImGui::BeginCombo(combo_id, preview.c_str())) {
            for (const auto& node : graph.nodes()) {
                if (ImGui::Selectable(node_label(*node).c_str(),
                                      node->id == selection)) {
                    selection = node->id;
                }
            }
            ImGui::EndCombo();
        }
    };

    // --- add an edge -------------------------------------------------------
    ImGui::SeparatorText("Add an edge");
    if (graph.node_count() < 1) {
        ImGui::TextDisabled("Add steps first.");
    } else {
        node_picker("##edgefrom", "(from)", add_edge_from_);
        ImGui::SameLine();
        ImGui::TextUnformatted("→");
        ImGui::SameLine();
        node_picker("##edgeto", "(to)", add_edge_to_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 7.0f);
        const char* edge_kind_names[] = {"data_flow", "control"};
        ImGui::Combo("##edgekind", &add_edge_kind_, edge_kind_names, 2);
        ImGui::SameLine();
        if (ImGui::Button("Add edge")) {
            const ir::EdgeKind kind = add_edge_kind_ == 1
                                          ? ir::EdgeKind::Control
                                          : ir::EdgeKind::DataFlow;
            if (graph.add_edge(add_edge_from_, add_edge_to_, kind)) {
                graph_dirty_ = true;
                graph_error_.clear();
            } else {
                graph_error_ =
                    "edge rejected: unknown step or duplicate edge";
            }
        }
    }

    // Existing edges, each with a delete button (remove_edge is the inverse).
    if (!graph.edges().empty()) {
        int remove_at = -1;
        for (std::size_t i = 0; i < graph.edges().size(); ++i) {
            const ir::Edge& edge = graph.edges()[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("x")) remove_at = static_cast<int>(i);
            ImGui::SameLine();
            const auto from = graph.find_node(edge.from);
            const auto to = graph.find_node(edge.to);
            ImGui::TextDisabled(
                "%s → %s  (%s)",
                from ? from->name.c_str() : edge.from.c_str(),
                to ? to->name.c_str() : edge.to.c_str(),
                std::string{ir::to_string(edge.kind)}.c_str());
            ImGui::PopID();
        }
        if (remove_at >= 0) {
            const ir::Edge edge = graph.edges()[remove_at];
            graph.remove_edge(edge.from, edge.to, edge.kind);
            graph_dirty_ = true;
        }
    }

    // --- selected step -----------------------------------------------------
    ImGui::SeparatorText("Selected step");
    const auto selected = graph.find_node(selected_node_id_);
    if (!selected) {
        ImGui::TextDisabled("Click a step in the canvas to select it.");
    } else {
        const std::string short_id =
            selected_node_id_.substr(0, selected_node_id_.find('-'));
        ImGui::TextDisabled("id: %s   kind: %s", short_id.c_str(),
                            std::string{ir::to_string(selected->kind)}.c_str());
        ImGui::SetNextItemWidth(font * 12.0f);
        const bool rename = ImGui::InputTextWithHint(
            "##rennode", "step name", &rename_node_input_,
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Rename step") || rename) {
            if (rename_node_input_.empty()) {
                graph_error_ = "step name required";
            } else {
                selected->name = rename_node_input_;  // mutate the working copy
                graph_dirty_ = true;
                graph_error_.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete step")) {
            graph.remove_node(selected_node_id_);
            selected_node_id_.clear();
            graph_dirty_ = true;
        }
    }
}

}  // namespace zetta::gui
