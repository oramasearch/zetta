// The Activity view (#622, docs/gui/activity-feed.md): ONE chronological feed
// of everything the cluster was asked to do — who asked (actor + surface),
// what for, and how it went — with click-through to the owning panel. A thin
// renderer over zetta::observability::activity_entries (the audit trail + runs
// merged at call time; the same source system.activity serves, RULE-7): the
// feed is recomputed each frame, the panel stores only its filters.

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"

#include "zetta/node.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/util/timestamp.hpp"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

using zetta::observability::ActivityEntry;
using zetta::observability::ActivityOutcome;
using zetta::observability::ActivitySurface;

// The outcome word beside the tone colour (a word and a colour, never colour
// alone). Info entries (created / queued) have no outcome yet: a quiet dot.
const char* outcome_label(ActivityOutcome outcome) {
    switch (outcome) {
        case ActivityOutcome::Ok:     return "ok";
        case ActivityOutcome::Denied: return "denied";
        case ActivityOutcome::Failed: return "failed";
        case ActivityOutcome::Info:   return "·";
    }
    return "·";
}

// Compact display form of a target: uuids collapse to their first segment
// (the Home project rows' short-id convention); everything else as-is.
std::string short_target(const std::string& target) {
    if (target.size() == 36 && target.find('-') == 8) {
        return target.substr(0, 8);
    }
    return target;
}

}  // namespace

void App::draw_activity() {
    // No row selection yet — consume any deep link so it doesn't go stale.
    take_pending_selection(View::Activity);

    page_header("Activity",
                "Everything this cluster was asked to do: who asked, from "
                "where, and how it went");

    const std::vector<ActivityEntry> entries =
        zetta::observability::activity_entries(node_);
    if (entries.empty()) {
        if (empty_state("Nothing has happened yet",
                        "Runs, deploys, compute jobs, key changes, and MCP "
                        "calls land here as one chronological feed.",
                        "Open deploy")) {
            navigate_to(View::Deploy);
        }
        return;
    }

    // Filters — presentation only. The window compares fixed-width ISO-8601
    // stamps against a cutoff; surfaces map to the feed's derived surface.
    static const char* kWindows[] = {"Last hour", "Last 24 hours", "All time"};
    static const long long kWindowSeconds[] = {3600, 86400, 0};
    static const char* kSurfaces[] = {"All surfaces", "GUI", "Console", "MCP"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::Combo("##activity_window", &activity_window_, kWindows, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::Combo("##activity_surface", &activity_surface_, kSurfaces, 4);

    const long long window_seconds = kWindowSeconds[activity_window_];
    const std::string cutoff =
        window_seconds == 0 ? std::string()
                            : zetta::util::iso8601_seconds_ago(window_seconds);
    const auto surface_matches = [this](ActivitySurface surface) {
        switch (activity_surface_) {
            case 1:  return surface == ActivitySurface::Local;
            case 2:  return surface == ActivitySurface::Console;
            case 3:  return surface == ActivitySurface::Api;
            default: return true;
        }
    };

    std::vector<const ActivityEntry*> visible;
    visible.reserve(entries.size());
    for (const ActivityEntry& entry : entries) {
        if (!cutoff.empty() && entry.time < cutoff) continue;
        if (!surface_matches(entry.surface)) continue;
        visible.push_back(&entry);
    }

    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::TextColored(kInkQuiet, "%zu of %zu entries", visible.size(),
                       entries.size());
    if (mono_font_) ImGui::PopFont();
    ImGui::Spacing();

    if (visible.empty()) {
        ImGui::TextColored(kInkMuted,
                           "No activity in this window. Widen the window or "
                           "clear the surface filter.");
        return;
    }

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (mono_font_) ImGui::PushFont(mono_font_);
    if (ImGui::BeginTable("##activity_feed", 7, table_flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);  // keep the header row pinned
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Surface", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Actor", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Outcome", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();

        int row_index = 0;
        for (const ActivityEntry* entry : visible) {
            ImGui::PushID(row_index++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kInkQuiet, "%s", entry->time.c_str());
            ImGui::TableSetColumnIndex(1);
            // The surface is how a GUI-driven and an MCP-driven action tell
            // apart at a glance: external surfaces render in primary ink,
            // local ones quiet.
            ImGui::TextColored(entry->surface == ActivitySurface::Local
                                   ? kInkQuiet
                                   : kInkPrimary,
                               "%s", activity_surface_label(entry->surface));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(kInkSecondary, "%s",
                               entry->actor.empty() ? "·" : entry->actor.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(kInkPrimary, "%s", entry->action.c_str());
            ImGui::TableSetColumnIndex(4);
            if (entry->target.empty()) {
                ImGui::TextColored(kInkQuiet, "·");
            } else if (entry->ref_kind == "run") {
                entity_link(short_target(entry->target).c_str(), View::Runs,
                            entry->ref_id);
            } else if (entry->ref_kind == "job") {
                entity_link(short_target(entry->target).c_str(), View::Jobs,
                            entry->ref_id);
            } else if (entry->ref_kind == "graph") {
                // The graph name — the workspace resolves it to the stored
                // graph and opens it (#633).
                entity_link(entry->target.c_str(), View::Graph, entry->ref_id);
            } else if (entry->ref_kind == "deployment") {
                // The deployed graph's name — the deployments table matches
                // it for the row highlight (#633).
                entity_link(entry->target.c_str(), View::Deploy,
                            entry->ref_id);
            } else if (entry->ref_kind == "dataset") {
                entity_link(short_target(entry->target).c_str(), View::Datasets,
                            entry->ref_id);
            } else if (entry->ref_kind == "cloud") {
                entity_link(entry->target.c_str(), View::Cloud, {});
            } else {
                entity_link(entry->target.c_str(), View::Admin, {});
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(tone_color(tone_of(entry->outcome)), "%s",
                               outcome_label(entry->outcome));
            ImGui::TableSetColumnIndex(6);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kInkSecondary, "%s",
                               entry->detail.empty() ? "·"
                                                     : entry->detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (mono_font_) ImGui::PopFont();
}

}  // namespace zetta::gui
