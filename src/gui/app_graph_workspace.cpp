#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"

#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/graph_plan.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/run_progress.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

// The canvas colours (kCanvasBox*, kCanvasEdge*) come from the shared canvas
// palette in app_internal.hpp (#630): nodes are white boxes with hairline
// borders laid out in columns by StepKind; the selected node gets a coral
// border (the "you are here" accent); edges are grey (data) / amber (control).

// The colour a step's projected cluster placement is drawn in (#591): the
// shared placement tone (#636) — coral stays the "you are here" accent, never
// status.
ImVec4 placement_color(zetta::pipeline::StepPlacement placement) {
    return tone_color(tone_of(placement));
}

// The short display form of a run output's ref for the tight canvas box: the
// segment after any "<tag>:" prefix, cut at the first '-' — so the default
// executor's logical "model:<uuid>" and the churn engine's real model/index
// ids both read as one short mono token.
std::string short_output_ref(const std::string& ref) {
    const std::size_t colon = ref.find(':');
    const std::string tail = colon == std::string::npos ? ref : ref.substr(colon + 1);
    return tail.substr(0, tail.find('-'));
}

}  // namespace

void App::open_graph(const zetta::pipeline::ProjectGraph& stored) {
    // A fresh working copy: edits apply here and only land in the IR on Save, so
    // an edit is a deliberate, atomic act (and Revert is just "drop this copy").
    editing_graph_ = std::make_unique<ir::ExecutionGraph>(stored.graph());
    selected_graph_ = stored.name();
    graph_loaded_id_ = stored.id();
    graph_dirty_ = false;
    selected_node_id_.clear();
    graph_error_.clear();
    graph_status_.clear();
    rename_graph_input_ = stored.name();
    rename_node_input_.clear();
    add_node_name_input_.clear();
    add_edge_from_.clear();
    add_edge_to_.clear();
}

void App::draw_graph_workspace() {
    // Consume a navigate_to(entity) deep link aimed at this view (#633): the
    // entity is a stored graph, referenced by id (a run's `graph`) or by saved
    // name (a deployment's `graph_name`, an audit target). Resolve it across
    // every project — the link's sender doesn't know which project owns it —
    // then select that project and open the graph. An unresolvable ref (the
    // default executor's free-form labels) just lands on the workspace.
    if (auto pending_entity = take_pending_selection(View::Graph)) {
        for (const std::string& project_name : node_.project_names()) {
            const auto owner = node_.find_project(project_name);
            if (!owner) continue;
            auto stored = owner->find_graph_by_id(*pending_entity);
            if (!stored) stored = owner->find_graph(*pending_entity);
            if (!stored) continue;
            selected_project_ = project_name;
            // Pre-empt the project-changed reset below so the open survives.
            graph_panel_project_ = owner->id();
            open_graph(*stored);
            break;
        }
    }

    ImGui::SeparatorText("Graph workspace");

    // The workspace edits the *currently selected* project's graphs (the same
    // selection the explorer's Projects column drives). Re-resolved by name each
    // frame so a dropped/renamed project never leaves a dangling handle.
    const auto project = node_.find_project(selected_project_);
    if (!project) {
        editing_graph_.reset();
        graph_panel_project_.clear();
        selected_graph_.clear();
        // Two nothing-yet cases (#631): a fresh node with no projects at all
        // (create one, right here), and projects that merely need selecting
        // (Home lists them; clicking one lands back in this workspace).
        if (node_.project_names().empty()) {
            if (empty_state("No projects yet",
                            "A project holds your execution graphs and turns "
                            "an intent into a deployed pipeline.",
                            "New project…")) {
                new_project_modal_requested_ = true;
            }
        } else if (empty_state("No project selected",
                               "Pick a project and its execution graphs open "
                               "here.",
                               "Open home")) {
            navigate_to(View::Home);
        }
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

    // The owning project, linked back to its home (Explorer) — the graph →
    // project hop of the object graph (#633).
    link_line("project:  ", project->name(), View::Explorer, project->name());

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
        // The next action is the New graph field directly above (#631).
        empty_state("No graphs yet",
                    "Name a graph above and it opens here as an empty canvas "
                    "to build step by step.",
                    "");
    }
    for (const std::string& name : names) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            if (!project->drop_graph(name)) {
                graph_error_ = "could not drop graph '" + name + "'";
            } else if (selected_graph_ == name) {
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
        banner(Tone::Error, graph_error_);
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

    // Live-run overlay (#626): project the newest run referencing this graph
    // onto the canvas. Re-resolved + re-projected from the live run record
    // every frame (the #590 pattern — the record is the only source; a run
    // arriving over replication, or started from the console/MCP, animates
    // identically to one started here). "Run graph" executes the saved graph
    // on this node, labelled with the stored-graph id so the overlay (and any
    // reader of run.graph) can tie the run back to it.
    ImGui::Checkbox("Live run", &show_run_overlay_);
    ImGui::SameLine();
    ImGui::TextDisabled("overlay the latest run of this graph");
    ImGui::SameLine();
    ImGui::BeginDisabled(graph_dirty_ || editing_graph_->empty() ||
                         churn_run_ != nullptr);
    if (ImGui::SmallButton("Run graph")) run_open_graph(*project);
    ImGui::EndDisabled();
    if (graph_dirty_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Save the graph first · a run executes the saved graph");
    } else if (churn_run_ &&
               ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("A churn run is in flight · the overlay tracks it live");
    }

    const std::shared_ptr<zetta::pipeline::Run> live_run =
        show_run_overlay_ ? latest_run_for_open_graph() : nullptr;
    zetta::pipeline::RunProgress run_progress;
    if (live_run) {
        run_progress = zetta::pipeline::project_run_progress(*editing_graph_,
                                                             live_run->value());
    }

    draw_graph_canvas(show_cluster_plan_ ? &plan : nullptr,
                      live_run ? &run_progress : nullptr);
    if (show_run_overlay_) draw_graph_run_status(live_run.get(), run_progress);
    if (show_cluster_plan_) draw_graph_plan(plan);
    draw_graph_editor(*project);
}

std::shared_ptr<zetta::pipeline::Run> App::latest_run_for_open_graph() const {
    if (!editing_graph_ || graph_loaded_id_.empty()) return nullptr;

    // Deployments of this graph (by its saved name) — the deployment
    // controller's runs reference their deployment, not a graph id.
    std::vector<std::string> deployment_ids;
    if (!selected_graph_.empty()) {
        for (const auto& deployment : node_.deployments()) {
            if (deployment->graph_name() == selected_graph_) {
                deployment_ids.push_back(deployment->id());
            }
        }
    }

    std::shared_ptr<zetta::pipeline::Run> latest;
    for (const auto& run : node_.runs()) {
        const bool references_graph =
            run->graph() == graph_loaded_id_ ||
            (!selected_graph_.empty() && run->graph() == selected_graph_) ||
            (!run->deployment().empty() &&
             std::find(deployment_ids.begin(), deployment_ids.end(),
                       run->deployment()) != deployment_ids.end());
        if (!references_graph) continue;
        // Newest by the ISO-8601 stamps (lexicographic order == time order).
        if (!latest || run->created() > latest->created() ||
            (run->created() == latest->created() &&
             run->updated() > latest->updated())) {
            latest = run;
        }
    }
    return latest;
}

void App::run_open_graph(zetta::pipeline::Project& project) {
    // Execute the SAVED contents (the run must describe what is stored, not an
    // unsaved working copy — the button is disabled while dirty as a belt; this
    // re-resolve is the suspenders).
    const auto stored = project.find_graph_by_id(graph_loaded_id_);
    if (!stored) {
        graph_error_ = "run failed: the open graph is no longer stored";
        return;
    }
    graph_error_.clear();

    // A churn-shaped graph runs on the REAL churn engine over the node's live
    // JobCoordinator — the same routing the governed console applies (#580) —
    // so the overlay shows real featurize/train/index statuses and the model
    // step's output ref is a real, resolvable Model id. It runs OFF the UI
    // thread (#624, AsyncChurnRun — one churn run at a time, shared with the
    // Deploy panel), so the live-run overlay animates the run AS it executes
    // instead of only seeing its terminal state. Anything else (or a node with
    // no compute plane attached) runs through the structural
    // DefaultStepExecutor — quick, so it stays synchronous.
    if (node_.job_coordinator() != nullptr &&
        zetta::usecases::churn::is_churn_graph(stored->graph())) {
        if (churn_run_) {
            graph_error_ = "a churn run is already in flight — wait for it to "
                           "finish (or cancel it in the Deploy panel)";
            return;
        }
        zetta::usecases::churn::ChurnSolutionOptions options;
        options.project = project.id();
        options.graph = stored->id();
        churn_run_from_deploy_ = false;
        churn_run_from_onboarding_ = false;
        churn_run_ = zetta::usecases::churn::AsyncChurnRun::start(
            node_, stored->graph(),
            zetta::usecases::churn::demo_churn_dataset(), options);
    } else {
        node_.run_graph(stored->graph(), project.id(), stored->id());
    }
    // No status line here: the run record itself is the feedback — the live-run
    // overlay + strip below the canvas pick it up as the run executes.
    show_run_overlay_ = true;
}

zetta::pipeline::ClusterPlan App::build_graph_plan() const {
    if (!editing_graph_) return zetta::pipeline::ClusterPlan{};

    // Members projected over = the live cluster (system.nodes / the spatial
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

void App::draw_graph_canvas(const zetta::pipeline::ClusterPlan* plan,
                            const zetta::pipeline::RunProgress* run_progress) {
    ir::ExecutionGraph& graph = *editing_graph_;

    // Index the plan by step id so each node box can show its projected placement.
    std::unordered_map<std::string, const zetta::pipeline::StepPlan*> plan_of;
    if (plan != nullptr) {
        for (const zetta::pipeline::StepPlan& step : plan->steps) plan_of[step.node_id] = &step;
    }

    // Index the live-run projection by step id (#626) so each box can wear its
    // run status. Rebuilt each frame from the projection the caller computed —
    // presentation only, no run state is stored between frames.
    std::unordered_map<std::string, const zetta::pipeline::StepRunProgress*> run_of;
    if (run_progress != nullptr) {
        for (const zetta::pipeline::StepRunProgress& step : run_progress->steps) {
            run_of[step.step_id] = &step;
        }
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
                                  header_pos, kCanvasBoxMuted, header.c_str());
                } else {
                    draw->AddText(header_pos, kCanvasBoxMuted, header.c_str());
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
                                        ? kCanvasEdgeControl
                                        : kCanvasEdgeData;
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

                // Live-run overlay (#626): the step's projected run status.
                const auto run_it = run_of.find(item.node->id);
                const zetta::pipeline::StepRunProgress* step_run =
                    run_it != run_of.end() ? run_it->second : nullptr;
                const bool step_failed =
                    step_run != nullptr &&
                    step_run->status == zetta::pipeline::StepRunStatus::Failed;
                const bool step_running =
                    step_run != nullptr &&
                    step_run->status == zetta::pipeline::StepRunStatus::Running;

                // The running step wears a soft amber pulse just outside its
                // border — time-based easing only, no stored animation state.
                if (step_running) {
                    const float pulse =
                        0.35f +
                        0.35f * static_cast<float>(
                                    std::sin(ImGui::GetTime() * 5.0));
                    ImVec4 glow = tone_color(Tone::Warn);
                    glow.w = pulse;
                    const float grow = font * 0.18f;
                    draw->AddRect(
                        ImVec2(item.top_left.x - grow, item.top_left.y - grow),
                        ImVec2(item.bottom_right.x + grow,
                               item.bottom_right.y + grow),
                        ImGui::GetColorU32(glow), rounding, 0, font * 0.14f);
                }

                const ImU32 fill = selected ? kCanvasBoxFillSelected
                                            : (hovered ? kCanvasBoxFillHover : kCanvasBoxFill);
                draw->AddRectFilled(item.top_left, item.bottom_right, fill,
                                    rounding);
                // A failed step is pinned red; the coral selection accent still
                // wins ("you are here" stays coral — status never borrows it).
                const ImU32 border =
                    selected ? kCanvasBoxBorderSelected
                             : (step_failed
                                    ? ImGui::GetColorU32(tone_color(Tone::Error))
                                    : kCanvasBoxBorder);
                draw->AddRect(item.top_left, item.bottom_right, border,
                              rounding, 0, selected || step_failed ? 2.0f : 1.0f);
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
                draw->AddText(ImVec2(name_x, name_y), kCanvasBoxText,
                              item.node->name.c_str());

                // Live-run overlay (#626): a status dot in the top-right corner
                // (tone_of colours — pending reads as a hollow ring), and the
                // short id of the first output the step materialized, bottom-
                // right in mono (the placement badge keeps the bottom-left).
                if (step_run != nullptr) {
                    const ImU32 tone = ImGui::GetColorU32(
                        tone_color(tone_of(step_run->status)));
                    const float dot_radius = font * 0.18f;
                    const ImVec2 dot_center(
                        item.bottom_right.x - pad * 0.6f - dot_radius,
                        item.top_left.y + pad * 0.6f + dot_radius);
                    if (step_run->status ==
                        zetta::pipeline::StepRunStatus::Pending) {
                        draw->AddCircle(dot_center, dot_radius, tone, 0,
                                        font * 0.08f);
                    } else {
                        draw->AddCircleFilled(dot_center, dot_radius, tone);
                    }
                    if (!step_run->outputs.empty()) {
                        const std::string ref =
                            short_output_ref(step_run->outputs.front().ref);
                        const float small = font * 0.78f;
                        const ImU32 ref_color =
                            ImGui::GetColorU32(tone_color(Tone::Ok));
                        if (mono_font_) {
                            const ImVec2 size = mono_font_->CalcTextSizeA(
                                small, FLT_MAX, 0.0f, ref.c_str());
                            draw->AddText(
                                mono_font_, small,
                                ImVec2(item.bottom_right.x - pad * 0.5f - size.x,
                                       item.top_left.y + node_h - font * 0.30f -
                                           small),
                                ref_color, ref.c_str());
                        } else {
                            const ImVec2 size = ImGui::CalcTextSize(ref.c_str());
                            draw->AddText(
                                ImVec2(item.bottom_right.x - pad * 0.5f - size.x,
                                       item.top_left.y + node_h - font * 0.30f -
                                           size.y),
                                ref_color, ref.c_str());
                        }
                    }
                }
                draw->PopClipRect();

                // Hovering a step with run progress explains it: status, the
                // failing reason verbatim, and what it materialized.
                if (hovered && step_run != nullptr) {
                    std::string tip = item.node->name + " · " +
                                      std::string{zetta::pipeline::to_string(
                                          step_run->status)};
                    if (!step_run->reason.empty()) {
                        tip += "\n" + step_run->reason;
                    }
                    for (const zetta::pipeline::RunOutput& output :
                         step_run->outputs) {
                        tip += "\n" + output.kind + " · " + output.name +
                               " · " + output.ref;
                    }
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }

            // Reserve the laid-out area so the child scrolls to fit it.
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(content_right - origin.x + col_gap,
                                content_bottom - origin.y + row_gap));
        }
    }
    ImGui::EndChild();
}

void App::draw_graph_run_status(const zetta::pipeline::Run* run,
                                const zetta::pipeline::RunProgress& progress) {
    ImGui::Spacing();
    eyebrow(mono_font_, "LIVE RUN");
    if (run == nullptr) {
        ImGui::TextDisabled(
            "No runs reference this graph yet · Run graph above, or start one "
            "from the console, deploy, or MCP.");
        return;
    }

    // Identity strip: the overlaid run, its live status, the step tally, and
    // the cross-entity link into the Runs view (logs / outputs / lineage).
    std::size_t succeeded = 0;
    std::size_t reached = 0;  // steps the run has started (any non-pending state)
    for (const zetta::pipeline::StepRunProgress& step : progress.steps) {
        if (step.status == zetta::pipeline::StepRunStatus::Succeeded) ++succeeded;
        if (step.status != zetta::pipeline::StepRunStatus::Pending) ++reached;
    }
    const Tone run_tone = tone_of(run->status());
    status_dot(tone_color(run_tone));
    const std::string run_short_id = run->id().substr(0, run->id().find('-'));
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::TextColored(kInkPrimary, "run %s", run_short_id.c_str());
    if (mono_font_) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(tone_color(run_tone), "[%s]",
                       std::string(to_string(run->status())).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu step(s) completed · %zu reached", succeeded,
                        progress.steps.size(), reached);
    ImGui::SameLine();
    entity_link("open in runs", View::Runs, run->id());
    ImGui::TextDisabled("created %s · updated %s", run->created().c_str(),
                        run->updated().c_str());

    // A failed run pins its failing step + reason (verbatim from the run log)
    // under the canvas — visible without hovering anything.
    if (!progress.failure_reason.empty()) {
        std::string pinned = progress.failure_reason;
        for (const zetta::pipeline::StepRunProgress& step : progress.steps) {
            if (step.status != zetta::pipeline::StepRunStatus::Failed) continue;
            const auto failed_node = editing_graph_->find_node(step.step_id);
            pinned = "step '" +
                     (failed_node ? failed_node->name : step.step_id) +
                     "' failed: " + step.reason;
            break;
        }
        banner(Tone::Error, pinned);
    }

    // The selected step's run detail: status + what it materialized, with a
    // click-through to a produced model's panel when the ref is a real,
    // resolvable Model id (the churn engine's model step). Refs that resolve
    // to nothing (the default executor's logical handles) render as plain
    // mono — the overlay never fakes a destination.
    const zetta::pipeline::StepRunProgress* selected_step = nullptr;
    for (const zetta::pipeline::StepRunProgress& step : progress.steps) {
        if (step.step_id == selected_node_id_) selected_step = &step;
    }
    if (selected_step == nullptr || selected_step->outputs.empty()) return;
    for (const zetta::pipeline::RunOutput& output : selected_step->outputs) {
        mono_line(kInkQuiet, "output:   ",
                  output.kind + " · " + output.name + " · " + output.ref);
        if (output.kind == "model" && node_.find_model(output.ref)) {
            ImGui::SameLine();
            entity_link("open in models", View::Models, output.ref);
        }
    }
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
    const auto health_color = [](const zetta::cluster::NodeStatus& node) {
        return tone_color(tone_of(node));
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
                                here ? kCanvasBoxFillSelected : kCanvasBoxFill, rounding);
            draw->AddRect(top_left, bottom_right,
                          here ? kCanvasBoxBorderSelected : kCanvasBoxBorder, rounding, 0,
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
                draw->AddText(mono_font_, font, id_pos, kCanvasBoxText, id.c_str());
            } else {
                draw->AddText(id_pos, kCanvasBoxText, id.c_str());
            }
            const ImVec2 addr_pos(top_left.x + box_pad,
                                  top_left.y + box_pad + font * 1.4f);
            if (mono_font_) {
                draw->AddText(mono_font_, font * 0.82f, addr_pos, kCanvasBoxMuted,
                              node.address.c_str());
            } else {
                draw->AddText(addr_pos, kCanvasBoxMuted, node.address.c_str());
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
        ImGui::TextColored(tone_color(Tone::Warn), "● unsaved edits");
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
            // The save outcome surfaces HERE, in the workspace (#632) — not
            // on another view's status line.
            graph_status_ = "Saved graph '" + updated->name() + "' (" +
                            std::to_string(updated->graph().node_count()) +
                            " steps, " +
                            std::to_string(updated->graph().edge_count()) +
                            " edges)";
        } else {
            graph_status_.clear();
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
    banner(Tone::Error, graph_error_);
    if (graph_error_.empty()) banner(Tone::Ok, graph_status_);
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
