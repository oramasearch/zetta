// The view registry + navigation + sidebar (#628, docs/gui/architecture.md).
// Each view is declared exactly once in the registry table below; the sidebar,
// navigation, and the command palette (#625) all consume the same rows.

#include <cfloat>
#include <string>
#include <string_view>

#include <imgui.h>

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"

namespace zetta::gui {

std::span<const App::ViewInfo> App::view_registry() {
    // Sidebar order: the Orama Studio shell (#875, docs/product/experience/
    // design-sync.md) — the project lifecycle leads (Workspace), then the data
    // the work draws on (Data library), the operating surfaces (Operate), the
    // technical tools (Developer), and Infrastructure demoted to Advanced.
    // Sections group under mono eyebrow headers; the sectionless Home row
    // leads. View enum names stay the engine's (View::Graph, View::Gate);
    // labels speak the Studio vocabulary (Pipeline, Verification).
    static const ViewInfo kViews[] = {
        {View::Home, "", "Home", &App::draw_home, nullptr},
        {View::Graph, "Workspace", "Pipeline workspace", &App::draw_graph_workspace,
         &App::projects_badge},
        {View::Gate, "Workspace", "Verification", &App::draw_gate, nullptr},
        {View::Deploy, "Workspace", "Deploy", &App::draw_deploy, nullptr},
        {View::Explorer, "Data library", "Explorer", &App::draw_explorer, nullptr},
        {View::Datasets, "Data library", "Datasets", &App::draw_datasets, nullptr},
        {View::Search, "Data library", "Search", &App::draw_search, nullptr},
        // Activity leads Operate: the feed is the first thing an operator
        // looks at ("what has been happening on this cluster", #622).
        {View::Activity, "Operate", "Activity", &App::draw_activity,
         nullptr},
        // Traces sits beside it: the same "what happened", per operation —
        // every span's intent · plan · duration · result shape (#640).
        {View::Traces, "Operate", "Traces", &App::draw_traces, nullptr},
        {View::Runs, "Operate", "Runs", &App::draw_runs, nullptr},
        {View::Jobs, "Operate", "Compute jobs", &App::draw_jobs,
         &App::jobs_badge},
        {View::Tests, "Operate", "Test suite", &App::draw_test_suite,
         nullptr},
        // Developer mirrors the July 24 web IA: Models sits with the maker
        // tools; governance and infrastructure demote to Advanced.
        {View::Models, "Developer", "Models", &App::draw_models, nullptr},
        {View::Query, "Developer", "SQL query", &App::draw_query, nullptr},
        {View::Console, "Developer", "Console", &App::draw_console, nullptr},
        {View::Agents, "Developer", "Agents", &App::draw_agents, nullptr},
        {View::Chat, "Developer", "Chat", &App::draw_chat, nullptr},
        {View::DataGen, "Developer", "Data generators", &App::draw_datagen, nullptr},
        {View::Admin, "Advanced", "Governance", &App::draw_admin, nullptr},
        {View::Cluster, "Advanced", "Infrastructure", &App::draw_cluster,
         &App::cluster_badge},
        {View::Cloud, "Advanced", "Cloud inspector", &App::draw_cloud,
         nullptr},
    };
    return kViews;
}

void App::navigate_to(View view, const std::string& entity_id) {
    active_view_ = view;
    if (!entity_id.empty()) pending_selection_[view] = entity_id;
}

std::optional<std::string> App::take_pending_selection(View view) {
    const auto found = pending_selection_.find(view);
    if (found == pending_selection_.end()) return std::nullopt;
    std::string entity_id = std::move(found->second);
    pending_selection_.erase(found);
    return entity_id;
}

// Inline link text: ink that underlines under the cursor, navigating on click.
// No accent (links are wayfinding, not the surface's focal action).
void App::entity_link(const char* label, View view,
                      const std::string& entity_id) {
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkSecondary, "%s", label);
    if (g_mono_font) ImGui::PopFont();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 text_min = ImGui::GetItemRectMin();
        const ImVec2 text_max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(text_min.x, text_max.y - 1.0f),
            ImVec2(text_max.x, text_max.y - 1.0f),
            ImGui::GetColorU32(kInkSecondary));
    }
    if (ImGui::IsItemClicked()) navigate_to(view, entity_id);
}

// A detail pane's "label: ref" line where the ref is reachable: the quiet mono
// label, then the ref as an entity_link (#633). Callers verify reachability
// first and fall back to a plain mono_line — a link never fakes a destination.
void App::link_line(const char* label, const std::string& display, View view,
                    const std::string& entity_id) {
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkQuiet, "%s", label);
    if (g_mono_font) ImGui::PopFont();
    ImGui::SameLine(0.0f, 0.0f);
    entity_link(display.c_str(), view, entity_id);
}

std::string App::project_name_for(const std::string& project_ref) const {
    if (project_ref.empty()) return {};
    // Some records carry the name itself (the explorer's selection key).
    if (node_.find_project(project_ref)) return project_ref;
    // Others (runs, models, jobs) carry the project uuid — resolve it through
    // the public name list.
    for (const std::string& project_name : node_.project_names()) {
        const auto project = node_.find_project(project_name);
        if (project && project->id() == project_ref) return project_name;
    }
    return {};
}

bool App::graph_is_stored(const std::string& graph_ref) const {
    if (graph_ref.empty()) return false;
    for (const std::string& project_name : node_.project_names()) {
        const auto project = node_.find_project(project_name);
        if (project && (project->find_graph_by_id(graph_ref) ||
                        project->find_graph(graph_ref))) {
            return true;
        }
    }
    return false;
}

std::string App::jobs_badge() const {
    std::size_t running = 0;
    for (const auto& job : node_.jobs()) {
        if (job && job->status() == zetta::compute::JobStatus::Running) {
            ++running;
        }
    }
    return running == 0 ? std::string() : std::to_string(running);
}

std::string App::cluster_badge() const {
    return std::to_string(cluster_.nodes().size());
}

std::string App::projects_badge() const {
    const std::size_t count = node_.project_names().size();
    return count == 0 ? std::string() : std::to_string(count);
}

void App::draw_sidebar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::GetFontSize() * 11.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
    ImGui::BeginChild("##sidebar", ImVec2(width, 0.0f),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    // The Studio lockup leads the rail (the design's brand row): the accent
    // dot + the wordmark, in the heading weight when available.
    {
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        const float dot_radius = ImGui::GetFontSize() * 0.18f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(dot_pos.x + dot_radius,
                   dot_pos.y + ImGui::GetTextLineHeight() * 0.55f),
            dot_radius, ImGui::GetColorU32(kAccent));
        ImGui::Dummy(ImVec2(dot_radius * 2.0f + style.ItemSpacing.x, 0.0f));
        ImGui::SameLine();
        if (heading_font_) ImGui::PushFont(heading_font_);
        ImGui::TextColored(kInkPrimary, "Orama Studio");
        if (heading_font_) ImGui::PopFont();
        ImGui::Spacing();
    }

    bool first_row = true;
    std::string_view current_section;
    for (const ViewInfo& view : view_registry()) {
        if (first_row || std::string_view(view.section) != current_section) {
            if (!first_row) {
                ImGui::Spacing();
                ImGui::Spacing();
            }
            current_section = view.section;
            if (!current_section.empty()) eyebrow(g_mono_font, view.section);
            first_row = false;
        }

        const bool active = view.id == active_view_;
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        if (ImGui::Selectable(view.name, active)) navigate_to(view.id);
        if (active) ImGui::PopStyleColor();

        const ImVec2 row_min = ImGui::GetItemRectMin();
        const ImVec2 row_max = ImGui::GetItemRectMax();
        if (active) {
            // The accent "you are here" bar on the rail's edge — the design
            // system's one surgical accent use in navigation.
            const float bar_x = row_min.x - style.WindowPadding.x;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(bar_x, row_min.y), ImVec2(bar_x + 3.0f, row_max.y),
                ImGui::GetColorU32(kAccent));
        }
        if (view.badge) {
            const std::string badge_text = (this->*view.badge)();
            if (!badge_text.empty()) {
                ImFont* badge_font =
                    g_mono_font ? g_mono_font : ImGui::GetFont();
                const float badge_font_size = badge_font->FontSize;
                const ImVec2 badge_size = badge_font->CalcTextSizeA(
                    badge_font_size, FLT_MAX, 0.0f, badge_text.c_str());
                const ImVec2 badge_pos(
                    row_max.x - badge_size.x - style.FramePadding.x,
                    row_min.y + (row_max.y - row_min.y - badge_size.y) * 0.5f);
                ImGui::GetWindowDrawList()->AddText(
                    badge_font, badge_font_size, badge_pos,
                    ImGui::GetColorU32(kInkQuiet), badge_text.c_str());
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace zetta::gui
