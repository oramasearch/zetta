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

    page_header("Home",
                "Is everything ok, what happened, what needs attention: this "
                "cluster at a glance");
    if (header_action("New project…")) new_project_modal_requested_ = true;

    // The live sources, read once per frame (a thin renderer keeps no copy).
    const std::vector<zetta::cluster::NodeStatus> nodes = cluster_.nodes();
    const std::vector<zetta::cluster::NodeTelemetrySample> telemetry =
        node_.telemetry_snapshot();
    const std::vector<std::shared_ptr<zetta::pipeline::Run>> runs =
        node_.runs();
    const std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs =
        node_.jobs();
    const std::vector<zetta::observability::ActivityEntry> activity =
        zetta::observability::activity_entries(node_);
    const std::vector<std::string> project_names = node_.project_names();

    // ── Cluster health strip: nodes · sync · load · in-flight work ────────
    section_card("##home_health", "Cluster health", [&] {
        std::size_t behind = 0;
        std::size_t unknown = 0;
        for (const zetta::cluster::NodeStatus& node_status : nodes) {
            if (node_status.is_self) continue;
            if (!node_status.replication.known) ++unknown;
            else if (!node_status.replication.in_sync) ++behind;
        }
        const std::size_t in_sync = nodes.size() - behind - unknown;

        // The hottest live gauge across the cluster (cpu or memory, whichever
        // is higher on any node) — the one-number summary of the per-node
        // gauges the Cluster view renders in full.
        float peak_percent = -1.0f;
        for (const zetta::cluster::NodeTelemetrySample& sample : telemetry) {
            if (sample.metrics.cpu_percent_available) {
                peak_percent = std::max(
                    peak_percent, static_cast<float>(sample.metrics.cpu_percent));
            }
            if (sample.metrics.memory_available) {
                peak_percent = std::max(
                    peak_percent,
                    static_cast<float>(sample.metrics.memory_percent));
            }
        }

        std::size_t running_runs = 0;
        for (const auto& run : runs) {
            if (run && run->status() == zetta::pipeline::RunStatus::Running) {
                ++running_runs;
            }
        }
        std::size_t running_jobs = 0;
        for (const auto& job : jobs) {
            if (job && job->status() == zetta::compute::JobStatus::Running) {
                ++running_jobs;
            }
        }

        if (stat_tile_link("nodes", std::to_string(nodes.size()))) {
            navigate_to(View::Cluster);
        }
        tile_gap();
        if (stat_tile_link("in sync",
                           std::to_string(in_sync) + " of " +
                               std::to_string(nodes.size()),
                           behind > 0 ? Tone::Warn : Tone::Ok)) {
            navigate_to(View::Cluster);
        }
        tile_gap();
        if (stat_tile_link("peak load",
                           peak_percent < 0.0f
                               ? std::string("·")
                               : std::format("{:.0f}%", peak_percent),
                           peak_percent < 0.0f
                               ? Tone::Muted
                               : tone_of_utilization(peak_percent))) {
            navigate_to(View::Cluster);
        }
        tile_gap();
        if (stat_tile_link("active runs", std::to_string(running_runs),
                           running_runs > 0 ? Tone::Warn : Tone::Muted)) {
            navigate_to(View::Runs);
        }
        tile_gap();
        if (stat_tile_link("active jobs", std::to_string(running_jobs),
                           running_jobs > 0 ? Tone::Warn : Tone::Muted)) {
            navigate_to(View::Jobs);
        }
        // Velocity KPI — rendered only when the data exists (a project with a
        // captured intent, deployed, with a run tying the two together).
        if (const std::optional<double> velocity =
                median_intent_to_deploy_seconds(node_)) {
            tile_gap();
            if (stat_tile_link(
                    "intent to deployed",
                    zetta::util::format_duration(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::duration<double>(*velocity))))) {
                navigate_to(View::Deploy);
            }
        }
    });

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

    // ── Needs attention: failed runs/jobs, gate/governance denials, lag ───
    section_card("##home_attention", "Needs attention", [&] {
        struct AttentionItem {
            Tone tone;
            std::string what;   // the status word beside the dot
            std::string label;  // the click-through link text
            View view;
            std::string entity;  // deep-link id ("" = navigate only)
            std::string when;
        };
        std::vector<AttentionItem> items;
        constexpr std::size_t kPerKind = 3;

        std::size_t failed_runs = 0;
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

        std::size_t failed_jobs = 0;
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
        std::size_t denials = 0;
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

        if (items.empty()) {
            status_label(Tone::Ok, "all clear");
            ImGui::TextColored(kInkMuted,
                               "No failed runs or jobs, nothing blocked at "
                               "the gate, every node in sync.");
            return;
        }
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
        const std::size_t overflow_runs =
            failed_runs > kPerKind ? failed_runs - kPerKind : 0;
        const std::size_t overflow = overflow_runs +
            (failed_jobs > kPerKind ? failed_jobs - kPerKind : 0) +
            (denials > kPerKind ? denials - kPerKind : 0);
        if (overflow > 0) {
            ImGui::TextColored(kInkQuiet, "and %s more in the owning panels",
                               std::to_string(overflow).c_str());
        }
    });

    // ── Recent runs: what ran, how it went, straight into the Runs panel ──
    section_card("##home_runs", "Recent runs", [&] {
        if (runs.empty()) {
            ImGui::TextColored(kInkMuted,
                               "No runs yet. Deploy a solution or run a graph "
                               "and it lands here.");
            entity_link("Open deploy", View::Deploy, {});
            return;
        }
        const std::string yesterday = zetta::util::iso8601_seconds_ago(86400);
        std::size_t last_day = 0;
        for (const auto& run : runs) {
            if (run && run->created() >= yesterday) ++last_day;
        }
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkQuiet, "%s · %s in the last 24 hours",
                           plural(runs.size(), "run", "runs").c_str(),
                           std::to_string(last_day).c_str());
        if (mono_font_) ImGui::PopFont();
        ImGui::Spacing();

        // runs() is created-ascending; walk the newest few from the back.
        const std::size_t shown = std::min<std::size_t>(runs.size(), 5);
        for (std::size_t offset = 0; offset < shown; ++offset) {
            const auto& run = runs[runs.size() - 1 - offset];
            if (!run) continue;
            ImGui::PushID(run->id().c_str());
            status_label(tone_of(run->status()),
                         std::string(label_of(run->status())).c_str());
            ImGui::SameLine();
            std::string label = "run " + short_id(run->id());
            if (!run->graph().empty()) label += " · " + short_id(run->graph());
            entity_link(label.c_str(), View::Runs, run->id());
            ImGui::SameLine();
            mono_line(kInkQuiet, "· ", day_clock(run->updated()));
            ImGui::PopID();
        }
        ImGui::Spacing();
        entity_link("View all runs", View::Runs, {});
    });

    // ── Projects: the central artifact, with when each last moved ─────────
    section_card("##home_projects", "Projects", [&] {
        if (project_names.empty()) {
            if (empty_state("No projects yet",
                            "Create one to turn an intent into a deployed "
                            "pipeline.",
                            "New project…")) {
                new_project_modal_requested_ = true;
            }
            return;
        }
        // project id → the newest run stamp referencing it, one pass.
        std::map<std::string, std::string> last_run;
        for (const auto& run : runs) {
            if (!run || run->project().empty()) continue;
            std::string& stamp = last_run[run->project()];
            if (run->updated() > stamp) stamp = run->updated();
        }
        for (const std::string& name : project_names) {
            const auto project = node_.find_project(name);
            if (!project) continue;
            ImGui::PushID(name.c_str());
            if (ImGui::Selectable(name.c_str(), selected_project_ == name)) {
                selected_project_ = name;
                navigate_to(View::Graph);  // open it in the workspace
            }
            std::string last_active = project->updated();
            const auto run_stamp = last_run.find(project->id());
            if (run_stamp != last_run.end() &&
                run_stamp->second > last_active) {
                last_active = run_stamp->second;
            }
            if (mono_font_) ImGui::PushFont(mono_font_);
            ImGui::TextColored(
                kInkQuiet, "    %s · %s · last active %s",
                short_id(project->id()).c_str(),
                plural(project->graph_count(), "graph", "graphs").c_str(),
                day_clock(last_active).c_str());
            if (mono_font_) ImGui::PopFont();
            ImGui::PopID();
        }
    });

    // ── Recent activity: the newest few feed entries, each a link ─────────
    section_card("##home_activity", "Recent activity", [&] {
        if (activity.empty()) {
            ImGui::TextColored(kInkMuted,
                               "Nothing yet. Runs, deploys, compute jobs, and "
                               "MCP calls land here as they happen.");
            return;
        }
        const std::size_t shown = std::min<std::size_t>(activity.size(), 5);
        for (std::size_t index = 0; index < shown; ++index) {
            const zetta::observability::ActivityEntry& entry = activity[index];
            ImGui::PushID(static_cast<int>(index));
            status_dot(tone_color(tone_of(entry.outcome)));
            std::string line = clock_of(entry.time) + " · " +
                               activity_surface_label(entry.surface) + " · " +
                               entry.action;
            if (!entry.target.empty()) line += " · " + short_id(entry.target);
            // Click through to the owning panel (the Activity table's ref
            // mapping); entries with no richer owner open the full feed.
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
            entity_link(line.c_str(), view, entity);
            ImGui::PopID();
        }
        ImGui::Spacing();
        entity_link("View all activity", View::Activity, {});
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
