#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"

#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/version.hpp"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

// Compact uptime, e.g. "45s", "1m 7s", "2h 3m 0s" — for the status strip.
std::string format_uptime(double seconds) {
    long total = seconds > 0.0 ? static_cast<long>(seconds) : 0;
    const long hours = total / 3600;
    total %= 3600;
    const long minutes = total / 60;
    const long secs = total % 60;
    std::string out;
    if (hours) out += std::to_string(hours) + "h ";
    if (hours || minutes) out += std::to_string(minutes) + "m ";
    out += std::to_string(secs) + "s";
    return out;
}

// "N thing" / "N things" — singular/plural by count.
std::string plural(std::size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

}  // namespace

void App::draw_about_modal() {
    if (about_requested_) {
        ImGui::OpenPopup("About Orama Zetta");
        about_requested_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f),
                             ImGuiCond_Appearing);

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("About Orama Zetta", &stay_open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("Orama Zetta");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kVersion);
    ImGui::TextColored(kCoral, "Intelligence Velocity for big thinkers.");
    ImGui::Spacing();
    ImGui::TextColored(kInkSecondary,
        "Orama Zetta turns a question into running, deployed intelligence. "
        "Describe a data or AI problem in plain language, or hand over an "
        "artifact you already have, and it architects a correct data + AI "
        "pipeline, deploys it onto infrastructure you control, runs it, and "
        "learns from your data.");
    ImGui::Spacing();
    ImGui::TextColored(kInkMuted,
        "\"The data engineer, the AI engineer, and the platform engineer, "
        "in a bottle.\"");

    ImGui::SeparatorText("This node");
    ImGui::TextDisabled("id:       %s", cluster_.node_id().c_str());
    ImGui::TextDisabled("data dir: %s", node_.config().data_dir.c_str());
    for (const std::string& address : cluster_.listen_addresses()) {
        ImGui::TextDisabled("listening: %s", address.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "\"zetta\" is the binary codename for the product Orama Zetta.");

    ImGui::Spacing();
    if (ImGui::Button("Close") || !stay_open) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_home() {
    // Hero — the product framing, echoing the marketing site. An eyebrow, a
    // headline with a single coral phrase (the only coral on this screen besides
    // the active-view checkmark), the tagline, and a short lead.
    ImGui::Spacing();
    eyebrow(mono_font_, "ORAMA ZETTA · AUTO-AI FOR THE ENTERPRISE");
    ImGui::Spacing();
    ImGui::SetWindowFontScale(1.7f);
    ImGui::TextColored(kInkPrimary, "Production-grade AI,");
    ImGui::SameLine();
    ImGui::TextColored(kCoral, "deployed in minutes.");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextColored(kInkSecondary, "Intelligence Velocity for big thinkers.");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 44.0f);
    ImGui::TextColored(kInkMuted,
        "Describe a data or AI problem in plain language, or bring an artifact "
        "you already have (an n8n workflow, a SQL script, a notebook), and Orama "
        "Zetta architects a correct pipeline, deploys it onto infrastructure you "
        "control, and runs it.");
    ImGui::PopTextWrapPos();

    // Primary action — charcoal fill, white text. Per the design system, "New …"
    // is a charcoal primary action; coral stays reserved for the accent.
    ImGui::Spacing();
    if (primary_button("New project…")) new_project_modal_requested_ = true;

    // Your projects — the central AI-project artifact. Each row opens the project
    // in the Graph workspace. Empty state is voiced, not generic.
    ImGui::Spacing();
    ImGui::Spacing();
    eyebrow(mono_font_, "YOUR PROJECTS");
    ImGui::Spacing();
    const std::vector<std::string> names = node_.project_names();
    if (names.empty()) {
        // The next action is the New project… button directly above (#631).
        empty_state("No projects yet",
                    "Create one to turn an intent into a deployed pipeline.",
                    "");
    }
    for (const std::string& name : names) {
        const auto project = node_.find_project(name);
        if (!project) continue;
        ImGui::PushID(name.c_str());
        if (ImGui::Selectable(name.c_str(), selected_project_ == name)) {
            selected_project_ = name;
            navigate_to(View::Graph);  // open it in the workspace
        }
        const std::string short_id =
            project->id().substr(0, project->id().find('-'));
        const std::size_t graphs = project->graph_names().size();
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkQuiet, "    %s · %s · created %s", short_id.c_str(),
                           plural(graphs, "graph", "graphs").c_str(),
                           project->created().c_str());
        if (mono_font_) ImGui::PopFont();
        ImGui::Spacing();
        ImGui::PopID();
    }

    // Recent activity — a compact strip of the same feed the Activity view
    // renders (#622): the newest few entries, recomputed each frame from
    // zetta::observability::activity_entries (no copy), with the link into
    // the full feed. Quiet metadata lines in the strip grammar.
    ImGui::Spacing();
    ImGui::Spacing();
    eyebrow(mono_font_, "RECENT ACTIVITY");
    ImGui::Spacing();
    const std::vector<zetta::observability::ActivityEntry> recent =
        zetta::observability::activity_entries(node_);
    if (recent.empty()) {
        // A quiet strip, not a call to action: the hero button above is the
        // fresh node's one next step, so this empty state carries no action.
        empty_state("Nothing has happened yet",
                    "Runs, deploys, compute jobs, and MCP calls land here as "
                    "they happen.",
                    "");
        return;
    }
    const std::size_t shown = recent.size() < 5 ? recent.size() : 5;
    if (mono_font_) ImGui::PushFont(mono_font_);
    for (std::size_t i = 0; i < shown; ++i) {
        const zetta::observability::ActivityEntry& entry = recent[i];
        // "14:03:21" from the fixed-width ISO stamp; the full stamp lives in
        // the Activity view.
        const std::string clock =
            entry.time.size() >= 19 ? entry.time.substr(11, 8) : entry.time;
        status_dot(tone_color(tone_of(entry.outcome)));
        std::string line = clock + " · " +
                           activity_surface_label(entry.surface) + " · " +
                           entry.action;
        if (!entry.target.empty()) line += " · " + entry.target;
        ImGui::TextColored(kInkSecondary, "%s", line.c_str());
    }
    if (mono_font_) ImGui::PopFont();
    ImGui::Spacing();
    entity_link("View all activity", View::Activity, {});
}

void App::draw_dashboard() {
    // A quiet, mono metadata strip in the design-system grammar: middle-dot
    // separators, tabular counts, no debug noise (uptime, not raw tick counts).
    // Only non-zero entity counts are shown, so an idle node reads clean.
    std::string strip = "orama zetta · running · up " +
                        format_uptime(node_.uptime_seconds());
    const struct {
        std::size_t count;
        const char* one;
        const char* many;
    } items[] = {
        {node_.project_count(), "project", "projects"},
        {node_.database_count(), "database", "databases"},
        {node_.deployment_count(), "deployment", "deployments"},
        {node_.run_count(), "run", "runs"},
        {node_.model_count(), "model", "models"},
        {node_.job_count(), "job", "jobs"},
        {node_.principal_count(), "principal", "principals"},
        {node_.audit_event_count(), "audit event", "audit events"},
    };
    for (const auto& item : items) {
        if (item.count > 0) strip += " · " + plural(item.count, item.one, item.many);
    }

    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::TextColored(kInkMuted, "%s", strip.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kInkQuiet, "· %s", node_.config().data_dir.c_str());
    if (mono_font_) ImGui::PopFont();

    banner(io_status_error_ ? Tone::Error : Tone::Ok, io_status_);
    ImGui::Spacing();
}

}  // namespace zetta::gui
