// SPDX-License-Identifier: MIT

// Home as a real dashboard (SCR-1, #629): the operator's first questions —
// is the cluster healthy, what ran recently, which projects moved, is
// anything blocked — answered from existing live read sources only
// (Cluster::nodes(), the telemetry snapshot, Node::runs()/jobs()/projects,
// the activity feed). Every tile and row is a click-through into the owning
// panel (stat_tile_link / entity_link → navigate_to). The empty node renders
// the guided-start card instead (#623 grows it). docs/gui/home-dashboard.md.

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/labels.hpp"
#include "gui/tones.hpp"

#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_input.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/duration.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/version.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <optional>
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

// A uuid collapsed to its first segment (the established short-id
// convention); non-uuid text renders as-is.
std::string short_id(const std::string& id) {
    const std::size_t dash = id.find('-');
    return dash == std::string::npos ? id : id.substr(0, dash);
}

// "06-25 14:03" from the fixed-width ISO stamp — enough to answer "what
// happened since yesterday" at a glance; the owning panel has the full stamp.
std::string day_clock(const std::string& stamp) {
    if (stamp.size() < 16) return stamp;
    std::string out = stamp.substr(5, 11);
    out[5] = ' ';  // the 'T'
    return out;
}

// "14:03:21" from the fixed-width ISO stamp (the activity-strip clock).
std::string clock_of(const std::string& stamp) {
    return stamp.size() >= 19 ? stamp.substr(11, 8) : stamp;
}

// The standard gap between stat tiles laid out on one row.
void tile_gap() { ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f); }

// The velocity KPI (intent → deployed): for every deployment a run ties back
// to a project (run.deployment + run.project), the time from that project's
// earliest captured intent to the deployment's creation. Median over the
// linked pairs; nullopt while the data doesn't exist yet.
std::optional<double> median_intent_to_deploy_seconds(const zetta::Node& node) {
    // deployment id → owning project id, via the runs that reference both.
    std::map<std::string, std::string> deployment_project;
    for (const auto& run : node.runs()) {
        if (run && !run->deployment().empty() && !run->project().empty()) {
            deployment_project.emplace(run->deployment(), run->project());
        }
    }
    if (deployment_project.empty()) return std::nullopt;

    // project id → earliest captured-intent stamp.
    std::map<std::string, std::string> first_intent;
    for (const std::string& name : node.project_names()) {
        const auto project = node.find_project(name);
        if (!project) continue;
        for (const auto& input : project->inputs()) {
            if (!input ||
                input->kind() != zetta::pipeline::ProjectInputKind::Intent) {
                continue;
            }
            std::string& earliest = first_intent[project->id()];
            if (earliest.empty() || input->created_at() < earliest) {
                earliest = input->created_at();
            }
        }
    }

    std::vector<double> deltas;
    for (const auto& deployment : node.deployments()) {
        if (!deployment) continue;
        const auto linked = deployment_project.find(deployment->id());
        if (linked == deployment_project.end()) continue;
        const auto intent = first_intent.find(linked->second);
        if (intent == first_intent.end()) continue;
        const std::optional<double> deployed =
            zetta::util::parse_iso8601_seconds(deployment->created());
        const std::optional<double> asked =
            zetta::util::parse_iso8601_seconds(intent->second);
        if (!deployed || !asked || *deployed < *asked) continue;
        deltas.push_back(*deployed - *asked);
    }
    if (deltas.empty()) return std::nullopt;
    std::nth_element(deltas.begin(), deltas.begin() + deltas.size() / 2,
                     deltas.end());
    return deltas[deltas.size() / 2];
}

}  // namespace

void App::draw_about_modal() {
    if (about_requested_) {
        ImGui::OpenPopup("About Orama Studio");
        about_requested_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f),
                             ImGuiCond_Appearing);

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("About Orama Studio", &stay_open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("Orama Studio");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kVersion);
    ImGui::TextColored(kAccent, "Intelligence Velocity for Big Thinkers.");
    ImGui::Spacing();
    ImGui::TextColored(kInkSecondary,
        "Orama Studio turns a question into running, deployed intelligence. "
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
        "Orama Studio is the product surface of the Orama Zetta platform; "
        "\"zetta\" is the platform binary codename.");

    ImGui::Spacing();
    if (ImGui::Button("Close") || !stay_open) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_home() {
    // Home takes no entity deep link — consume any so it doesn't go stale.
    take_pending_selection(View::Home);

    // The Studio home (#875 second pass): create-first per the design — the
    // hero question, the two entry paths, then Continue your work. The
    // operator dashboard this replaced lives on the Operate views (Runs,
    // Activity, Compute jobs, Infrastructure).
    page_header("What would you like to create?",
                "Start with a goal, or let Studio study the data you already "
                "have.");
    if (header_action("New project…")) new_project_modal_requested_ = true;

    // The live sources, read once per frame (a thin renderer keeps no copy).
    const std::vector<zetta::cluster::NodeStatus> nodes = cluster_.nodes();
    const std::vector<std::shared_ptr<zetta::pipeline::Run>> runs =
        node_.runs();
    const std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs =
        node_.jobs();
    const std::vector<zetta::observability::ActivityEntry> activity =
        zetta::observability::activity_entries(node_);
    const std::vector<std::string> project_names = node_.project_names();

    // ── The two entry paths, side by side ─────────────────────────────────
    if (ImGui::BeginTable("##home_paths", 2,
                          ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // The design's card hierarchy: small accent eyebrow, then the big
        // title in the heading face, then the body and the one action.
        section_card("##home_create", "", [&] {
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kAccent, "START WITH A GOAL");
            if (g_mono_font) ImGui::PopFont();
            ImGui::Spacing();
            if (heading_font_) ImGui::PushFont(heading_font_);
            ImGui::TextColored(kInkPrimary, "Create a Solution");
            if (heading_font_) ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(
                kInkMuted,
                "Describe your goal. Studio proposes a Solution, builds it, "
                "verifies it against your success measures, and gets it ready "
                "for production.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (primary_button("Describe your goal…")) {
                new_project_modal_requested_ = true;
            }
        });
        ImGui::TableNextColumn();
        // Honest state: the Auto-Suggest model that reads a Workspace Profile
        // (your data, connections, and daily work) is in training — the card
        // holds the design's place with no mock flow behind it.
        section_card("##home_discover", "", [&] {
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kInkQuiet, "START WITH YOUR DATA");
            if (g_mono_font) ImGui::PopFont();
            ImGui::Spacing();
            if (heading_font_) ImGui::PushFont(heading_font_);
            ImGui::TextColored(kInkPrimary, "Discover Opportunities");
            if (heading_font_) ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(
                kInkMuted,
                "Not sure where to start? Studio studies your Workspace "
                "Profile · the data you hold, your connections, and your "
                "daily work · and recommends what you could create from it.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            status_label(Tone::Muted, "in training");
            ImGui::SameLine();
            ImGui::TextColored(kInkQuiet, "· not available on this node yet");
        });
        ImGui::EndTable();
    }

    // ── The guided demo (#623): first-run onboarding on the empty node ────
    // A truly empty node opens on the guided UC-1 churn walkthrough
    // (app_onboarding.cpp); the card stays while the demo is mid-flight this
    // session (its own steps create projects/runs) or when replayed from
    // Help · Guided demo. Dismissing it falls back to the plain guided-start
    // empty state below; once a project exists it never auto-shows again.
    const bool node_is_empty =
        project_names.empty() && runs.empty() && activity.empty();
    if (onboarding_active_ || (node_is_empty && !onboarding_dismissed_)) {
        draw_guided_demo_card();
        if (node_is_empty) return;
    } else if (node_is_empty) {
        // Scoped id: the action label repeats the page header's New project…
        // (both visible here), and this one renders in the root window.
        ImGui::PushID("home_guided_start");
        if (empty_state(
                "Turn a question into deployed intelligence",
                "Describe a data or AI problem in plain language, or bring an "
                "artifact you already have (an n8n workflow, a SQL script, a "
                "notebook), and Orama Studio architects the Pipeline, deploys "
                "it onto infrastructure you control, and runs it.",
                "New project…")) {
            new_project_modal_requested_ = true;
        }
        ImGui::PopID();
        return;
    }

    // ── Needs attention: failed runs/jobs, denials, replication lag ───────
    // Collected first, rendered only when something actually needs attention
    // (the design's home is create-first; an all-clear card earns no space).
    struct AttentionItem {
        Tone tone;
        std::string what;   // the status word beside the dot
        std::string label;  // the click-through link text
        View view;
        std::string entity;  // deep-link id ("" = navigate only)
        std::string when;
    };
    std::vector<AttentionItem> items;
    std::size_t failed_runs = 0;
    std::size_t failed_jobs = 0;
    std::size_t denials = 0;
    constexpr std::size_t kPerKind = 3;
    {

        for (auto run = runs.rbegin(); run != runs.rend(); ++run) {  // newest first
            if (!*run || (*run)->status() != zetta::pipeline::RunStatus::Failed) {
                continue;
            }
            if (++failed_runs > kPerKind) continue;  // keep counting for the tally
            std::string label = "run " + short_id((*run)->id());
            if (!(*run)->graph().empty()) {
                label += " · " + short_id((*run)->graph());
            }
            items.push_back({Tone::Error, "run failed", label, View::Runs,
                             (*run)->id(), day_clock((*run)->updated())});
        }

        for (auto job = jobs.rbegin(); job != jobs.rend(); ++job) {
            if (!*job || (*job)->status() != zetta::compute::JobStatus::Failed) {
                continue;
            }
            if (++failed_jobs > kPerKind) continue;
            items.push_back({Tone::Error, "job failed",
                             "job " + short_id((*job)->id()), View::Jobs,
                             (*job)->id(), day_clock((*job)->updated())});
        }

        // Denials in the last day — the gate refusing a deploy, governance
        // refusing a caller — from the same feed the Activity view renders.
        const std::string yesterday = zetta::util::iso8601_seconds_ago(86400);
        for (const zetta::observability::ActivityEntry& entry : activity) {
            if (entry.outcome != zetta::observability::ActivityOutcome::Denied) {
                continue;
            }
            if (entry.time < yesterday) break;  // the feed is newest first
            if (++denials > kPerKind) continue;
            std::string label = entry.action;
            if (!entry.target.empty()) label += " · " + short_id(entry.target);
            View view = View::Activity;
            std::string entity;
            if (entry.ref_kind == "run") {
                view = View::Runs;
                entity = entry.ref_id;
            } else if (entry.ref_kind == "job") {
                view = View::Jobs;
                entity = entry.ref_id;
            } else if (entry.ref_kind == "graph") {
                view = View::Graph;
            } else if (entry.ref_kind == "deployment") {
                view = View::Deploy;
            }
            items.push_back({Tone::Error, "denied", label, view, entity,
                             day_clock(entry.time)});
        }

        for (const zetta::cluster::NodeStatus& node_status : nodes) {
            if (node_status.is_self || !node_status.replication.known ||
                node_status.replication.in_sync) {
                continue;
            }
            items.push_back({Tone::Warn, "node behind",
                             "node " + short_id(node_status.node_id),
                             View::Cluster, node_status.node_id, {}});
        }

    }
    if (!items.empty()) {
        section_card("##home_attention", "Needs attention", [&] {
            for (std::size_t index = 0; index < items.size(); ++index) {
                const AttentionItem& item = items[index];
                ImGui::PushID(static_cast<int>(index));
                status_label(item.tone, item.what.c_str());
                ImGui::SameLine();
                entity_link(item.label.c_str(), item.view, item.entity);
                if (!item.when.empty()) {
                    ImGui::SameLine();
                    mono_line(kInkQuiet, "· ", item.when);
                }
                ImGui::PopID();
            }
            const std::size_t overflow =
                (failed_runs > kPerKind ? failed_runs - kPerKind : 0) +
                (failed_jobs > kPerKind ? failed_jobs - kPerKind : 0) +
                (denials > kPerKind ? denials - kPerKind : 0);
            if (overflow > 0) {
                ImGui::TextColored(kInkQuiet,
                                   "and %s more in the owning panels",
                                   std::to_string(overflow).c_str());
            }
        });
    }

    // ── Continue your work: every Project, its state, the next action ─────
    section_card("##home_continue", "Continue your work", [&] {
        if (project_names.empty()) {
            if (empty_state("No projects yet",
                            "Describe a goal to create your first Solution.",
                            "New project…")) {
                new_project_modal_requested_ = true;
            }
            return;
        }
        // project id → its newest run, one pass (runs() is created-ascending).
        std::map<std::string, std::shared_ptr<zetta::pipeline::Run>> latest_run;
        for (const auto& run : runs) {
            if (!run || run->project().empty()) continue;
            auto& slot = latest_run[run->project()];
            if (!slot || run->updated() > slot->updated()) slot = run;
        }
        // The design's worklist columns: state | project (+ last active) |
        // the next useful action.
        if (ImGui::BeginTable("##home_worklist", 3,
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 8.0f);
            ImGui::TableSetupColumn("project",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 11.0f);
            for (const std::string& name : project_names) {
                const auto project = node_.find_project(name);
                if (!project) continue;
                ImGui::PushID(name.c_str());
                const auto run_found = latest_run.find(project->id());
                const auto run =
                    run_found == latest_run.end() ? nullptr : run_found->second;
                std::string last_active = project->updated();
                if (run && run->updated() > last_active)
                    last_active = run->updated();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (run) {
                    status_label(tone_of(run->status()),
                                 std::string(label_of(run->status())).c_str());
                } else if (project->graph_count() > 0) {
                    status_label(Tone::Ok, "ready");
                } else {
                    status_label(Tone::Muted, "draft");
                }
                if (g_mono_font) ImGui::PushFont(g_mono_font);
                ImGui::TextColored(kInkQuiet, "%s",
                                   day_clock(last_active).c_str());
                if (g_mono_font) ImGui::PopFont();

                ImGui::TableNextColumn();
                if (ImGui::Selectable(name.c_str(), selected_project_ == name)) {
                    selected_project_ = name;
                    navigate_to(View::Graph);  // open it in the workspace
                }

                ImGui::TableNextColumn();
                if (run) {
                    const char* next =
                        run->status() == zetta::pipeline::RunStatus::Running
                            ? "Review run"
                            : run->status() ==
                                      zetta::pipeline::RunStatus::Failed
                                  ? "Review failure"
                                  : "Review results";
                    entity_link(next, View::Runs, run->id());
                } else if (project->graph_count() > 0) {
                    entity_link("Verify and deploy", View::Gate, {});
                } else {
                    if (g_mono_font) ImGui::PushFont(g_mono_font);
                    ImGui::TextColored(kInkMuted, "Capture an intent");
                    if (g_mono_font) ImGui::PopFont();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    });
}

App::View App::view_for_external_capability(
    const std::string& capability) const {
    // The panel that owns what the capability mutated — the strip's
    // click-through target (#620). New mutating capabilities without a more
    // specific home land on the activity feed, which shows everything.
    if (capability == "create_project" || capability == "capture_intent" ||
        capability == "import_n8n") {
        return View::Explorer;
    }
    if (capability == "compile_intent" || capability == "compile_n8n" ||
        capability == "compile_python") {
        return View::Graph;
    }
    if (capability == "deploy") return View::Deploy;
    if (capability == "run" || capability == "testsuite_run") return View::Runs;
    if (capability == "add_dataset") return View::Datasets;
    return View::Activity;
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

    // External activity (#620): the newest mutating call an agent (MCP) or
    // integration just made, as a quiet click-through line above every view —
    // the operator notices without being yanked anywhere (navigation is the
    // click, never automatic). Entries fade after a short linger
    // (poll_background_ops); the durable record is the activity feed.
    if (!recent_external_activity_.empty()) {
        const ExternalActivity& newest = recent_external_activity_.back();
        // Product language when the registry knows the entry's title
        // ("Deploy a graph"), the raw capability name as the fallback.
        std::string action = newest.capability;
        if (capability_registry_) {
            if (const auto entry = capability_registry_->find(newest.capability)) {
                action = entry->title;
            }
        }
        const std::string actor = newest.actor == "local"
                                      ? std::string("local")
                                      : "key " + newest.actor.substr(0, 8);
        const double age = ImGui::GetTime() - newest.seen_seconds;
        const std::string when =
            age < 2.0 ? "just now"
                      : std::to_string(static_cast<int>(age)) + "s ago";
        if (mono_font_) ImGui::PushFont(mono_font_);
        status_dot(Tone::Active);
        const std::string line = "MCP · " + action + " · " + actor + " · " + when;
        entity_link(line.c_str(),
                    view_for_external_capability(newest.capability), {});
        if (recent_external_activity_.size() > 1) {
            ImGui::SameLine();
            ImGui::TextColored(kInkQuiet, "·");
            ImGui::SameLine();
            entity_link("View all activity", View::Activity, {});
        }
        if (mono_font_) ImGui::PopFont();
    }
    ImGui::Spacing();
}

}  // namespace zetta::gui
