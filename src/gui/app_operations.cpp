#include "gui/app.hpp"
#include "gui/app_internal.hpp"

#include "zetta/bytes.hpp"
#include "zetta/cloud_inspector.hpp"
#include "zetta/cloud_provider.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/run.hpp"
#include "zetta/scaling_policy.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

// A run's status, color-coded along its lifecycle: pending (neutral) → running
// (amber) → succeeded (green) | failed (red). Same palette as the cluster panel.
ImVec4 run_status_color(RunStatus status) {
    switch (status) {
        case RunStatus::Pending:   return kInkQuiet;
        case RunStatus::Running:   return kWarnColor;
        case RunStatus::Succeeded: return kSuccessColor;
        case RunStatus::Failed:    return kErrorColor;
    }
    return kInkQuiet;
}

// A compute job's status, color-coded along its lifecycle: queued (neutral) →
// running (amber) → succeeded (green) | failed (red). Same palette as runs.
ImVec4 job_status_color(JobStatus status) {
    switch (status) {
        case JobStatus::Queued:    return kInkQuiet;
        case JobStatus::Running:   return kWarnColor;
        case JobStatus::Succeeded: return kSuccessColor;
        case JobStatus::Failed:    return kErrorColor;
    }
    return kInkQuiet;
}

// A scaling action's color: scale-up green (more capacity), hold neutral,
// scale-down amber (shedding nodes). A quick read of the autoscaler's intent.
ImVec4 scaling_action_color(ScalingAction action) {
    switch (action) {
        case ScalingAction::ScaleUp:   return kSuccessColor;
        case ScalingAction::Hold:      return kInkQuiet;
        case ScalingAction::ScaleDown: return kWarnColor;
    }
    return kInkQuiet;
}

// A log line's severity color — error red, warn amber, anything else neutral.
ImVec4 log_level_color(const std::string& level) {
    if (level == "error") return kErrorColor;
    if (level == "warn")  return kWarnColor;
    return kInkQuiet;
}

// Join lineage refs (sources / steps) for a single wrapped line.
std::string join_refs(const std::vector<std::string>& refs) {
    std::string joined;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (i) joined += ", ";
        joined += refs[i];
    }
    return joined;
}

// Render one trained model produced by a run: its identity + weights fingerprint,
// its evaluation metrics (name→number), and its ranked feature drivers (the
// SHAP-style explainability surface). A pure view over an already-validated Model
// handle; metrics/drivers come from the model's free-form (already schema-
// validated) fields, so no new JSON boundary is introduced.
void render_model_card(const Model& model) {
    const std::string short_id = model.id().substr(0, model.id().find('-'));
    ImGui::SeparatorText(
        (model.name().empty() ? ("model " + short_id) : model.name()).c_str());
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkQuiet, "kind: %s  ·  id: %s", model.kind().c_str(),
                       short_id.c_str());
    if (model.has_weights()) {
        ImGui::TextColored(
            kInkQuiet, "weights: %s%s", format_bytes(model.weights()->size).c_str(),
            model.has_weights_body() ? "" : " (awaiting body transfer)");
    } else {
        ImGui::TextColored(kInkQuiet, "weights: (not yet attached)");
    }
    if (g_mono_font) ImGui::PopFont();

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

    // Metrics — the evaluation scores (auc / f1 / …). A flat name→number map; a
    // value in [0, 1] also gets a bar so a score reads at a glance.
    ImGui::TextDisabled("Metrics");
    if (!model.metrics().is_object() || model.metrics().empty()) {
        ImGui::TextDisabled("(no metrics recorded)");
    } else if (ImGui::BeginTable("##model_metrics", 2, table_flags)) {
        ImGui::TableSetupColumn("Metric");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        for (const auto& [name, value] : model.metrics().items()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(1);
            const double number = value.is_number() ? value.get<double>() : 0.0;
            if (number >= 0.0 && number <= 1.0) {
                ImGui::ProgressBar(static_cast<float>(number), ImVec2(-FLT_MIN, 0));
            } else {
                ImGui::Text("%.4g", number);
            }
        }
        ImGui::EndTable();
    }

    // Ranked drivers — the feature importances, strongest |influence| first. An
    // optional explainability annotation in the model's feature_schema; absent
    // until an Explain job records it.
    ImGui::TextDisabled("Ranked drivers");
    const std::vector<ModelDriver> drivers = model.drivers();
    if (drivers.empty()) {
        ImGui::TextDisabled("(no ranked drivers recorded)");
    } else {
        // Normalize the bars against the strongest |importance| so the ranking
        // reads visually; a leading "+/-" keeps the sign (push up vs. down).
        const double top = std::abs(drivers.front().importance);
        if (ImGui::BeginTable("##model_drivers", 3, table_flags)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Feature");
            ImGui::TableSetupColumn("Importance");
            ImGui::TableHeadersRow();
            int rank = 0;
            for (const ModelDriver& driver : drivers) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", ++rank);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(driver.feature.c_str());
                ImGui::TableSetColumnIndex(2);
                const float fraction =
                    top > 0.0 ? static_cast<float>(std::abs(driver.importance) / top)
                              : 0.0f;
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%+.3f", driver.importance);
                ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
            }
            ImGui::EndTable();
        }
    }
}

// Render the full detail of one run: identity + status, logs, outputs, the
// trained model(s) it produced (metrics + ranked drivers), and lineage. A pure
// view over the (already schema-validated) Run handle — no mutation. `models` are
// the node's models whose producing-run ref is this run (resolved by the caller).
void render_run_detail(const Run& run,
                       const std::vector<std::shared_ptr<Model>>& models) {
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkPrimary, "run %s", run.id().c_str());
    if (g_mono_font) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(run_status_color(run.status()), "[%s]",
                       std::string(to_string(run.status())).c_str());
    if (!run.project().empty()) mono_line(kInkQuiet, "project:  ", run.project());
    if (!run.graph().empty()) mono_line(kInkQuiet, "graph:    ", run.graph());
    mono_line(kInkQuiet, "created:  ", run.created());
    if (run.updated() != run.created()) {
        mono_line(kInkQuiet, "updated:  ", run.updated());
    }

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

    // Logs — time + level (color-coded) + message; the failing step + reason of
    // a failed run live here.
    ImGui::SeparatorText("Logs");
    if (run.logs().empty()) {
        ImGui::TextDisabled("(no log entries)");
    } else {
        for (const RunLogEntry& entry : run.logs()) {
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kInkQuiet, "%s", entry.time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(log_level_color(entry.level), "%-5s",
                               entry.level.c_str());
            if (g_mono_font) ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", entry.message.c_str());
        }
    }

    // Outputs — the produced datasets / models / indexes.
    ImGui::SeparatorText("Outputs");
    if (run.outputs().empty()) {
        ImGui::TextDisabled("(no outputs)");
    } else if (ImGui::BeginTable("##run_outputs", 3, table_flags)) {
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Ref");
        ImGui::TableHeadersRow();
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        for (const RunOutput& output : run.outputs()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(output.kind.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(output.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(output.ref.c_str());
        }
        if (g_mono_font) ImGui::PopFont();
        ImGui::EndTable();
    }

    // Trained models — the model(s) this run produced, with their metrics and
    // ranked drivers (the FEAT-5 "surface the trained Model + metrics + ranked
    // drivers" slice). Linked by the Model's producing-run ref (Model::run() ==
    // this run's id), the durable edge — the executor's logical model output ref
    // is just a placeholder handle in R1.
    if (!models.empty()) {
        ImGui::SeparatorText("Trained models");
        for (const std::shared_ptr<Model>& model : models) {
            ImGui::PushID(model->id().c_str());
            render_model_card(*model);
            ImGui::PopID();
        }
    }

    // Lineage — where the results came from (sources, steps), how fresh, and how
    // confident (a 0..1 score, shown as a bar).
    ImGui::SeparatorText("Lineage");
    const RunLineage& lineage = run.lineage();
    const bool has_lineage = !lineage.sources.empty() || !lineage.steps.empty() ||
                             !lineage.freshness.empty() || lineage.confidence != 0.0;
    if (!has_lineage) {
        ImGui::TextDisabled("(no lineage)");
        return;
    }
    ImGui::TextDisabled("sources:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", lineage.sources.empty()
                                 ? "—"
                                 : join_refs(lineage.sources).c_str());
    ImGui::TextDisabled("steps:");
    ImGui::SameLine();
    ImGui::TextWrapped(
        "%s", lineage.steps.empty() ? "—" : join_refs(lineage.steps).c_str());
    ImGui::TextDisabled("freshness: %s",
                        lineage.freshness.empty() ? "—" : lineage.freshness.c_str());
    ImGui::TextDisabled("confidence:");
    ImGui::SameLine();
    ImGui::ProgressBar(static_cast<float>(lineage.confidence), ImVec2(-FLT_MIN, 0));
}

}  // namespace

void App::draw_cluster() {
    ImGui::SeparatorText("Cluster");
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::TextColored(kInkSecondary, "node       %s", cluster_.node_id().c_str());
    for (const std::string& address : cluster_.listen_addresses()) {
        ImGui::TextColored(kInkSecondary, "listening  %s", address.c_str());
    }
    if (mono_font_) ImGui::PopFont();
    ImGui::TextDisabled(
        "All nodes are equal; leader election and failover are planned.");

    ImGui::TextUnformatted("Add another node with:");
    std::string command = cluster_.peer_command();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::InputText("##peer_command", &command, ImGuiInputTextFlags_ReadOnly);
    if (mono_font_) ImGui::PopFont();
    if (ImGui::Button("Copy command")) {
        ImGui::SetClipboardText(command.c_str());
    }

    ImGui::SeparatorText("Nodes & replication");
    const std::vector<NodeStatus> nodes = cluster_.nodes();
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("nodes", 6, table_flags)) {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Role");
        ImGui::TableSetupColumn("Entries");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Replication");
        ImGui::TableHeadersRow();
        if (mono_font_) ImGui::PushFont(mono_font_);
        for (const NodeStatus& node : nodes) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // First UUID segment — enough to tell nodes apart at a glance.
            const std::string id = node.node_id.substr(0, node.node_id.find('-'));
            ImGui::TextUnformatted(node.is_self ? (id + " (self)").c_str()
                                                : id.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(node.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(to_string(node.role).c_str());
            ImGui::TableSetColumnIndex(3);
            if (node.replication.known) {
                ImGui::Text("%zu", node.replication.entries);
            } else {
                ImGui::TextDisabled("—");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(node.replication.known
                                       ? node.replication.version.c_str()
                                       : "—");
            ImGui::TableSetColumnIndex(5);
            if (node.is_self) {
                ImGui::TextDisabled("—");
            } else if (!node.replication.known) {
                ImGui::TextDisabled("no status yet");
            } else if (node.replication.in_sync) {
                ImGui::TextColored(kSuccessColor,
                                   "in sync (%.1fs)",
                                   node.replication.seconds_since_sync);
            } else {
                ImGui::TextColored(kWarnColor,
                                   "syncing (%.1fs)",
                                   node.replication.seconds_since_sync);
            }
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::EndTable();
    }
}

void App::draw_cloud() {
    ImGui::SeparatorText("Cloud inspector");

    // The GUI surface over the provider backends (local / on-prem / Kubernetes /
    // AWS / GCP / Azure / OCI). **Permission-gated, deny-by-default**: inspecting
    // a provider runs its read-only identity/inventory commands (which use the
    // provider's credentials via its CLI), and zetta does NOTHING until the user
    // explicitly consents — per inspection, never remembered (CLAUDE.md hard
    // rule). Only on consent does run_cloud_inspect() shell out (through the
    // shared CommandRunner) and cache the result on the node
    // (system.zetta_cloud_status). The credential FILES are never read or printed
    // — only the read-only commands run.
    ImGui::TextColored(kInkMuted,
        "Inspect a deployment target. Running a provider's read-only commands "
        "(which read its config and credentials) requires your explicit "
        "permission, every time. Deny by default: nothing runs until you allow "
        "it. Credential files are never displayed.");
    ImGui::Spacing();

    const CloudProviderCatalog& catalog = CloudProviderCatalog::builtin();

    const float list_w = ImGui::GetContentRegionAvail().x * 0.30f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 18.0f;

    // Left: the provider list, grouped under mono category eyebrows. A green dot
    // = open (Local, reads nothing); an amber dot = locked behind consent.
    ImGui::BeginChild("##cloud_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    std::string current_category;
    for (const CloudProvider& provider : catalog.providers()) {
        if (provider.category != current_category) {
            current_category = provider.category;
            if (&provider != &catalog.providers().front()) ImGui::Spacing();
            eyebrow(mono_font_, to_upper(current_category).c_str());
        }
        ImGui::PushID(provider.id.c_str());
        status_dot(provider.requires_consent() ? kWarnColor : kDotActive);
        if (ImGui::Selectable(provider.name.c_str(),
                              selected_cloud_provider_ == provider.id)) {
            // Selecting a different provider resets any consent — consent never
            // carries across providers (deny-by-default, ask every time).
            if (selected_cloud_provider_ != provider.id) {
                cloud_consent_provider_.clear();
            }
            selected_cloud_provider_ = provider.id;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected provider's detail + the consent gate.
    ImGui::SameLine();
    ImGui::BeginChild("##cloud_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    const CloudProvider* provider = catalog.find(selected_cloud_provider_);
    if (!provider) {
        ImGui::TextDisabled("Select a provider to inspect.");
        ImGui::EndChild();
        return;
    }

    ImGui::Text("%s", provider->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kInkQuiet, "%s", provider->category.c_str());
    ImGui::TextColored(kInkSecondary, "%s", provider->summary.c_str());

    // Local needs no consent: it is this running node, reached in memory. Show
    // the node facts we already hold (no cloud access) and stop.
    if (!provider->requires_consent()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "THIS NODE");
        ImGui::Spacing();
        status_dot(kDotActive);
        ImGui::TextColored(kSuccessColor,
                           "No external access. Nothing to authorize.");
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "id        %s",
                           cluster_.node_id().c_str());
        ImGui::TextColored(kInkSecondary, "data dir  %s",
                           node_.config().data_dir.c_str());
        for (const std::string& address : cluster_.listen_addresses()) {
            ImGui::TextColored(kInkSecondary, "listening %s", address.c_str());
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::EndChild();
        return;
    }

    // What an inspect WOULD touch — named up front so consent is informed. The
    // paths + commands render in mono (they are code). The CLI may read these
    // files; zetta itself never reads or prints them.
    ImGui::Spacing();
    eyebrow(mono_font_, "THE CLI MAY READ");
    if (mono_font_) ImGui::PushFont(mono_font_);
    for (const std::string& path : provider->config_paths) {
        ImGui::BulletText("%s", path.c_str());
    }
    if (mono_font_) ImGui::PopFont();
    ImGui::Spacing();
    eyebrow(mono_font_, "AND ZETTA WOULD RUN, READ-ONLY");
    if (mono_font_) ImGui::PushFont(mono_font_);
    for (const InspectCommand& command : provider->inspect_commands) {
        ImGui::BulletText("%s", command.display.c_str());
    }
    if (mono_font_) ImGui::PopFont();

    // The consent gate — a tinted callout box so the permission boundary reads
    // as a deliberate stop, deny-by-default. Until the user explicitly allows
    // this one inspection nothing is read or run. On allow, run_cloud_inspect()
    // shells out to the read-only commands and caches the result; the result is
    // rendered below and stays queryable as system.zetta_cloud_status.
    ImGui::Spacing();
    const bool consented = cloud_consent_provider_ == provider->id;
    // Amber (caution) wash while locked; green (success) wash once granted.
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          consented ? ImVec4(0.882f, 0.961f, 0.933f, 1.0f)
                                    : ImVec4(0.980f, 0.933f, 0.855f, 1.0f));
    ImGui::BeginChild("##consent", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (!consented) {
        status_dot(kWarnColor);
        ImGui::TextColored(kWarnColor, "Permission required");
        ImGui::TextColored(kInkSecondary,
            "zetta will not run the commands above until you allow it. Consent "
            "applies to this one inspection and is not remembered. Credential "
            "files are never read or shown.");
        ImGui::Spacing();
        const std::string allow_label =
            "Allow inspection of " + provider->name;
        if (primary_button(allow_label.c_str())) {
            cloud_consent_provider_ = provider->id;
            run_cloud_inspect(*provider);  // the one cloud call, behind consent
        }
    } else {
        status_dot(kDotActive);
        ImGui::TextColored(kSuccessColor, "Permission granted for this inspection");
        draw_cloud_inspect_result(*provider);
        ImGui::Spacing();
        if (ImGui::Button("Re-inspect")) {
            run_cloud_inspect(*provider);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revoke permission")) {
            cloud_consent_provider_.clear();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

// Render the cached result of the consented inspect for `provider` (found on the
// node). Reads only already-produced state — it triggers no cloud call.
void App::draw_cloud_inspect_result(const CloudProvider& provider) {
    const std::vector<CloudInspectReport> reports = node_.cloud_inspections();
    const CloudInspectReport* report = nullptr;
    for (const CloudInspectReport& candidate : reports) {
        if (candidate.provider_id == provider.id) {
            report = &candidate;
            break;
        }
    }
    if (report == nullptr) {
        ImGui::TextColored(kInkSecondary, "Inspecting…");
        return;
    }

    // A provider described but not yet runnable (bare-metal needs a host).
    if (!report->ran) {
        status_dot(kWarnColor);
        ImGui::TextColored(kInkSecondary, "%s", report->detail.c_str());
        return;
    }

    // Headline reachability / identity.
    status_dot(report->reachable ? kDotActive : kErrorColor);
    if (report->authenticated && !report->identity.empty()) {
        ImGui::TextColored(kSuccessColor, "Connected");
        ImGui::SameLine();
        ImGui::TextColored(kInkSecondary, "· %s", report->identity.c_str());
    } else if (report->reachable) {
        ImGui::TextColored(kInkSecondary, "Reachable");
    } else {
        ImGui::TextColored(kErrorColor, "Not reachable");
    }
    if (!report->detail.empty()) {
        ImGui::TextColored(kInkSecondary, "%s", report->detail.c_str());
    }

    // Regions / resources, in mono (they are identifiers).
    if (!report->locations.empty()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "REGIONS");
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextWrapped("%s (%zu)", join_refs(report->locations).c_str(),
                           report->locations.size());
        if (mono_font_) ImGui::PopFont();
    }
    if (!report->resources.empty()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "NODES");
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextWrapped("%s (%zu)", join_refs(report->resources).c_str(),
                           report->resources.size());
        if (mono_font_) ImGui::PopFont();
    }

    // Per-command outcomes — a green/red dot, the command (mono), and an error
    // when one failed, so a missing CLI or a parse failure is legible.
    ImGui::Spacing();
    eyebrow(mono_font_, "COMMANDS");
    for (const CloudInspectCommand& command : report->commands) {
        status_dot(command.ok ? kDotActive : kErrorColor);
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "%s", command.display.c_str());
        if (mono_font_) ImGui::PopFont();
        if (!command.ok && !command.error.empty()) {
            ImGui::TextColored(kErrorColor, "    %s", command.error.c_str());
        }
    }
    if (!report->checked_at.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kInkQuiet, "checked at %s", report->checked_at.c_str());
    }
}

void App::run_cloud_inspect(const CloudProvider& provider) {
    // The single point where the GUI touches a cloud — reached only from the
    // consent gate's Allow / Re-inspect actions. Synchronous: the inspect runs on
    // the UI thread (which owns the node), so recording the result races with
    // nothing. The default CommandRunner shells out to the provider's CLI; the
    // result is cached for the panel and for system.zetta_cloud_status.
    CloudInspector inspector;
    node_.record_cloud_inspection(inspector.inspect(provider));
}

void App::draw_runs() {
    ImGui::SeparatorText("Runs / outputs / lineage");

    // A run is the record of one execution of a deployed graph (status / logs /
    // outputs / lineage). This panel is a read-only viewer: runs arrive via the
    // API, tests, or replication from a peer (executing a graph — StartRun — is
    // a later slice). Held by id and re-resolved each frame, like the explorer.
    const std::vector<std::shared_ptr<Run>> runs = node_.runs();
    ImGui::TextDisabled("%zu run(s)", runs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 14.0f;

    // Left: the run list. Runs have no human name, so each row is a status badge
    // plus the first segment of the UUID — enough to tell them apart.
    ImGui::BeginChild("##run_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (runs.empty()) ImGui::TextDisabled("(no runs)");
    for (const std::shared_ptr<Run>& run : runs) {
        ImGui::PushID(run->id().c_str());
        ImGui::TextColored(run_status_color(run->status()), "%-9s",
                           std::string(to_string(run->status())).c_str());
        ImGui::SameLine();
        const std::string short_id = run->id().substr(0, run->id().find('-'));
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (ImGui::Selectable(short_id.c_str(), selected_run_ == run->id())) {
            selected_run_ = run->id();
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected run's status / logs / outputs / lineage.
    ImGui::SameLine();
    ImGui::BeginChild("##run_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const auto run = node_.find_run(selected_run_)) {
        // Resolve the trained model(s) this run produced via the durable reverse
        // link (Model::run() == run id), so the detail pane can surface their
        // metrics + ranked drivers alongside the run's outputs.
        std::vector<std::shared_ptr<Model>> run_models;
        for (const std::shared_ptr<Model>& model : node_.models()) {
            if (model->run() == run->id()) run_models.push_back(model);
        }
        render_run_detail(*run, run_models);
    } else {
        ImGui::TextDisabled(
            "Select a run to view its status, logs, outputs, models, and "
            "lineage.");
    }
    ImGui::EndChild();
}

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
        "so this previews ScalingPolicy::evaluate(); it doesn't change the job.");

    // 1) Objective — "set the reward". The three reward models the autoscaler can
    //    optimize, in declaration order (the combo index maps to ScalingObjective).
    static constexpr std::array<ScalingObjective, 3> kObjectives{
        ScalingObjective::MinimizeCost, ScalingObjective::RuntimeTarget,
        ScalingObjective::DataShape};
    if (scaling_objective_ < 0 ||
        scaling_objective_ >= static_cast<int>(kObjectives.size())) {
        scaling_objective_ = 0;
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo(
            "Reward objective",
            std::string(to_string(kObjectives[scaling_objective_])).c_str())) {
        for (int index = 0; index < static_cast<int>(kObjectives.size());
             ++index) {
            const bool is_selected = index == scaling_objective_;
            if (ImGui::Selectable(
                    std::string(to_string(kObjectives[index])).c_str(),
                    is_selected)) {
                scaling_objective_ = index;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const ScalingObjective objective = kObjectives[scaling_objective_];

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
    if (objective == ScalingObjective::RuntimeTarget) {
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
    ScalingPolicy policy;
    policy.objective = objective;
    policy.bounds.min_nodes = static_cast<std::uint32_t>(scaling_min_nodes_);
    policy.bounds.max_nodes = static_cast<std::uint32_t>(scaling_max_nodes_);
    policy.bounds.cost_cap = scaling_cost_cap_;
    policy.target_seconds = scaling_target_seconds_;
    const ScalingSample sample{scaling_progress_, scaling_elapsed_, scaling_cost_,
                               static_cast<std::uint32_t>(scaling_nodes_)};
    const ScalingDecision decision = policy.evaluate(sample);

    ImGui::SeparatorText("Decision");
    ImGui::Text("recommended nodes: %u", decision.recommended_nodes);
    ImGui::SameLine();
    ImGui::TextColored(scaling_action_color(decision.action), "(%s)",
                       std::string(to_string(decision.action)).c_str());
    ImGui::Text("reward: %.4g", decision.reward);
    if (!decision.within_cost_cap) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "  cost cap breached");
    }
    ImGui::TextWrapped("%s", decision.reason.c_str());
}

void App::draw_jobs() {
    ImGui::SeparatorText("Compute jobs");

    // The GUI form of system.zetta_jobs: the live compute-job registry (one row
    // per unit of native distributed accelerated compute — featurize / train /
    // explain / score / a DDF op — with its lifecycle status). Read-only over the
    // replicated, already-validated ComputeJob handles, like the Runs panel —
    // creating/scheduling a job is a later slice. The detail pane adds the per-job
    // objective/scaling preview (draw_job_scaling_controls).
    const std::vector<std::shared_ptr<ComputeJob>> jobs = node_.jobs();
    ImGui::TextDisabled("%zu job(s)", jobs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 22.0f;

    // Left: the job list. Jobs have no human name, so each row is a color-coded
    // status badge plus the kind and the first UUID segment.
    ImGui::BeginChild("##job_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (jobs.empty()) ImGui::TextDisabled("(no jobs)");
    for (const std::shared_ptr<ComputeJob>& job : jobs) {
        ImGui::PushID(job->id().c_str());
        ImGui::TextColored(job_status_color(job->status()), "%-9s",
                           std::string(to_string(job->status())).c_str());
        ImGui::SameLine();
        const std::string short_id = job->id().substr(0, job->id().find('-'));
        const std::string label =
            std::string(to_string(job->kind())) + " · " + short_id;
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (ImGui::Selectable(label.c_str(), selected_job_ == job->id())) {
            selected_job_ = job->id();
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected job's detail + the objective/scaling preview.
    ImGui::SameLine();
    ImGui::BeginChild("##job_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const std::shared_ptr<ComputeJob> job = node_.find_job(selected_job_)) {
        // Reset the scaling controls whenever the selection changes, so they
        // start from sensible defaults per job rather than leaking the previous
        // job's edits.
        if (scaling_loaded_job_ != job->id()) {
            reset_scaling_controls();
            scaling_loaded_job_ = job->id();
        }

        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::TextColored(kInkPrimary, "job %s", job->id().c_str());
        if (g_mono_font) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(job_status_color(job->status()), "[%s]",
                           std::string(to_string(job->status())).c_str());
        mono_line(kInkQuiet, "kind:     ", std::string(to_string(job->kind())));
        mono_line(kInkQuiet, "created:  ", job->created());
        if (job->updated() != job->created()) {
            mono_line(kInkQuiet, "updated:  ", job->updated());
        }
        if (!job->detail().empty()) {
            ImGui::TextWrapped("detail: %s", job->detail().c_str());
        }

        // Id references — the job's place in the pipeline (project / graph / run /
        // scaling-policy reward). Shown only when set.
        const auto ref_line = [](const char* label, const std::string& value) {
            if (!value.empty()) mono_line(kInkQuiet, label, value);
        };
        ref_line("project:  ", job->project());
        ref_line("graph:    ", job->graph());
        ref_line("run:      ", job->run());
        ref_line("policy:   ", job->scaling_policy());

        draw_job_scaling_controls();
    } else {
        ImGui::TextDisabled(
            "Select a job to view its status, references, and scaling controls.");
    }
    ImGui::EndChild();
}

}  // namespace zetta::gui
