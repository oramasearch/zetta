#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/cloud/cloud_inspector.hpp"
#include "zetta/cloud/cloud_provider.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"

#include <imgui.h>

#include <string>
#include <vector>

namespace zetta::gui {

void App::draw_cloud() {
    page_header("Cloud inspector",
                "Inspect a deployment target · every provider is "
                "permission-gated, deny by default");

    // The GUI surface over the provider backends (local / on-prem / Kubernetes /
    // AWS / GCP / Azure / OCI). **Permission-gated, deny-by-default**: inspecting
    // a provider runs its read-only identity/inventory commands (which use the
    // provider's credentials via its CLI), and zetta does NOTHING until the user
    // explicitly consents — per inspection, never remembered (CLAUDE.md hard
    // rule). Only on consent does run_cloud_inspect() shell out (through the
    // shared CommandRunner) and cache the result on the node
    // (system.cloud_status). The credential FILES are never read or printed
    // — only the read-only commands run.
    ImGui::TextColored(kInkMuted,
        "Inspect a deployment target. Running a provider's read-only commands "
        "(which read its config and credentials) requires your explicit "
        "permission, every time. Deny by default: nothing runs until you allow "
        "it. Credential files are never displayed.");
    ImGui::Spacing();

    const zetta::cloud::CloudProviderCatalog& catalog = zetta::cloud::CloudProviderCatalog::builtin();

    // The provider rows, grouped under their category eyebrows. A green dot =
    // open (Local, reads nothing); an amber dot = locked behind consent.
    std::vector<MasterRow> rows;
    rows.reserve(catalog.providers().size());
    for (const zetta::cloud::CloudProvider& provider : catalog.providers()) {
        rows.push_back({.id = provider.id,
                        .title = provider.name,
                        .section = provider.category,
                        .tone = provider.requires_consent() ? Tone::Warn
                                                            : Tone::Active});
    }

    master_detail(
        "cloud", rows, selected_cloud_provider_,
        take_pending_selection(View::Cloud), "(no providers)",
        "Select a provider to inspect.",
        [&](const MasterRow& row) {
            const zetta::cloud::CloudProvider* provider = catalog.find(row.id);
            if (!provider) return;

            // Moving off a provider revokes any consent it held — consent
            // never carries across providers (deny-by-default, ask every
            // time). Runs before anything below reads the consent state.
            if (!cloud_consent_provider_.empty() &&
                cloud_consent_provider_ != provider->id) {
                cloud_consent_provider_.clear();
            }

            ImGui::Text("%s", provider->name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(kInkQuiet, "%s", provider->category.c_str());
            ImGui::TextColored(kInkSecondary, "%s", provider->summary.c_str());

            // Local needs no consent: it is this running node, reached in
            // memory. Show the node facts we already hold (no cloud access).
            if (!provider->requires_consent()) {
                ImGui::Spacing();
                eyebrow(mono_font_, "THIS NODE");
                ImGui::Spacing();
                status_label(Tone::Ok, "No external access. Nothing to authorize.");
                if (mono_font_) ImGui::PushFont(mono_font_);
                ImGui::TextColored(kInkSecondary, "id        %s",
                                   cluster_.node_id().c_str());
                ImGui::TextColored(kInkSecondary, "data dir  %s",
                                   node_.config().data_dir.c_str());
                for (const std::string& address : cluster_.listen_addresses()) {
                    ImGui::TextColored(kInkSecondary, "listening %s",
                                       address.c_str());
                }
                if (mono_font_) ImGui::PopFont();
                return;
            }

            // What an inspect WOULD touch — named up front so consent is
            // informed. The paths + commands render in mono (they are code).
            // The CLI may read these files; zetta itself never reads or prints
            // them.
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
            for (const zetta::cloud::InspectCommand& command : provider->inspect_commands) {
                ImGui::BulletText("%s", command.display.c_str());
            }
            if (mono_font_) ImGui::PopFont();

            // The consent gate — a tinted callout box so the permission
            // boundary reads as a deliberate stop, deny-by-default. Until the
            // user explicitly allows this one inspection nothing is read or
            // run. On allow, run_cloud_inspect() shells out to the read-only
            // commands and caches the result; the result is rendered below and
            // stays queryable as system.cloud_status.
            ImGui::Spacing();
            const bool consented = cloud_consent_provider_ == provider->id;
            // Amber (caution) wash while locked; green (success) once granted.
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  tone_background(consented ? Tone::Ok : Tone::Warn));
            ImGui::BeginChild("##consent", ImVec2(0, 0),
                              ImGuiChildFlags_Borders |
                                  ImGuiChildFlags_AutoResizeY);
            if (!consented) {
                status_label(Tone::Warn, "Permission required");
                ImGui::TextColored(kInkSecondary,
                    "zetta will not run the commands above until you allow it. "
                    "Consent applies to this one inspection and is not "
                    "remembered. Credential files are never read or shown.");
                ImGui::Spacing();
                const std::string allow_label =
                    "Allow inspection of " + provider->name;
                if (primary_button(allow_label.c_str())) {
                    cloud_consent_provider_ = provider->id;
                    run_cloud_inspect(*provider);  // the one cloud call, behind consent
                }
            } else {
                status_label(Tone::Ok, "Permission granted for this inspection");
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
        });
}

// Render the cached result of the consented inspect for `provider` (found on the
// node). Reads only already-produced state — it triggers no cloud call.
void App::draw_cloud_inspect_result(const zetta::cloud::CloudProvider& provider) {
    const std::vector<zetta::cloud::CloudInspectReport> reports = node_.cloud_inspections();
    const zetta::cloud::CloudInspectReport* report = nullptr;
    for (const zetta::cloud::CloudInspectReport& candidate : reports) {
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
        status_dot(Tone::Warn);
        ImGui::TextColored(kInkSecondary, "%s", report->detail.c_str());
        return;
    }

    // Headline reachability / identity.
    status_dot(report->reachable ? Tone::Active : Tone::Error);
    if (report->authenticated && !report->identity.empty()) {
        ImGui::TextColored(tone_color(Tone::Ok), "Connected");
        ImGui::SameLine();
        ImGui::TextColored(kInkSecondary, "· %s", report->identity.c_str());
    } else if (report->reachable) {
        ImGui::TextColored(kInkSecondary, "Reachable");
    } else {
        ImGui::TextColored(tone_color(Tone::Error), "Not reachable");
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
    for (const zetta::cloud::CloudInspectCommand& command : report->commands) {
        status_dot(command.ok ? Tone::Ok : Tone::Error);
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "%s", command.display.c_str());
        if (mono_font_) ImGui::PopFont();
        if (!command.ok && !command.error.empty()) {
            ImGui::TextColored(tone_color(Tone::Error), "    %s", command.error.c_str());
        }
    }
    if (!report->checked_at.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kInkQuiet, "checked at %s", report->checked_at.c_str());
    }
}

void App::run_cloud_inspect(const zetta::cloud::CloudProvider& provider) {
    // The single point where the GUI touches a cloud — reached only from the
    // consent gate's Allow / Re-inspect actions. Synchronous: the inspect runs on
    // the UI thread (which owns the node), so recording the result races with
    // nothing. The default CommandRunner shells out to the provider's CLI; the
    // result is cached for the panel and for system.cloud_status.
    zetta::cloud::CloudInspector inspector;
    node_.record_cloud_inspection(inspector.inspect(provider));
}

}  // namespace zetta::gui
