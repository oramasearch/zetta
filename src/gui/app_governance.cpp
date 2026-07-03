#include "gui/app.hpp"
#include "gui/app_internal.hpp"

#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/node.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/util/timestamp.hpp"

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

// Render one access key: its short id + created stamp, the resources its **scope**
// reaches (deny-by-default), and the rights its **capabilities** grant (each TTL'd;
// an expired grant is flagged red). A pure view over the already-validated key —
// `now` is the current ISO-8601 stamp the capability TTLs are compared against.
void render_access_key(const zetta::governance::AccessKey& key, const std::string& now) {
    const std::string short_id = key.id().substr(0, key.id().find('-'));
    ImGui::SeparatorText(("key " + short_id).c_str());
    mono_line(kInkQuiet, "created:  ", key.created());

    // Scope — which resources this key may touch (the resource axis, RULE-3).
    const zetta::governance::Scope& scope = key.scope();
    if (scope.unrestricted) {
        ImGui::TextColored(kWarnColor, "scope: unrestricted (admin)");
    } else if (scope.grants.empty()) {
        ImGui::TextDisabled("scope: (no resources, denies every resource)");
    } else {
        ImGui::TextDisabled("scope:");
        ImGui::Indent();
        for (const zetta::governance::Resource& resource : scope.grants) {
            ImGui::BulletText("%s: %s",
                              std::string(to_string(resource.kind)).c_str(),
                              resource.name.c_str());
        }
        ImGui::Unindent();
    }

    // Capabilities — which rights this key may exercise (the verb axis, ENT-22),
    // optionally TTL'd. No wildcard: a key with none denies every operation.
    const zetta::governance::CapabilitySet& capabilities = key.capabilities();
    if (capabilities.grants.empty()) {
        ImGui::TextDisabled("capabilities: (none, denies every operation)");
    } else {
        ImGui::TextDisabled("capabilities:");
        ImGui::Indent();
        for (const zetta::governance::Capability& capability : capabilities.grants) {
            const std::string right =
                std::string(to_string(capability.domain)) + ":" + capability.action;
            if (capability.expires_at.empty()) {
                ImGui::BulletText("%s", right.c_str());
            } else if (capability.expired(now)) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextColored(kFailColor, "%s (expired %s)", right.c_str(),
                                   capability.expires_at.c_str());
            } else {
                ImGui::BulletText("%s (expires %s)", right.c_str(),
                                  capability.expires_at.c_str());
            }
        }
        ImGui::Unindent();
    }
}

// Render the full detail of one principal: identity + its access keys (each with
// its scope + capability grants). A pure view — no mutation.
void render_principal_detail(const zetta::governance::Principal& principal) {
    ImGui::Text("%s", principal.name().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]",
                        std::string(to_string(principal.kind())).c_str());
    mono_line(kInkQuiet, "id:       ", principal.id());
    mono_line(kInkQuiet, "created:  ", principal.created());

    const std::vector<std::shared_ptr<zetta::governance::AccessKey>>& keys = principal.keys();
    ImGui::Spacing();
    ImGui::TextDisabled("%zu access key(s)", keys.size());
    if (keys.empty()) {
        ImGui::TextDisabled("(no keys, this principal can authenticate nothing)");
        return;
    }
    const std::string now = zetta::util::now_iso8601();
    for (const std::shared_ptr<zetta::governance::AccessKey>& key : keys) {
        ImGui::PushID(key->id().c_str());
        render_access_key(*key, now);
        ImGui::PopID();
    }
}

// Render the immutable audit trail as a scrolling table: each governed action
// (graph change / deploy / data access) with its time, actor, target, the
// allow/deny outcome (color-coded), and the human-readable detail. Chronological,
// matching Node::audit_events(); a pure view over already-validated events.
void render_audit_log(const std::vector<std::shared_ptr<zetta::governance::AuditEvent>>& events) {
    ImGui::TextDisabled("%zu event(s)", events.size());
    if (events.empty()) {
        ImGui::TextDisabled("(no audit events)");
        return;
    }
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const float table_h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
    if (ImGui::BeginTable("##audit_log", 6, table_flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);  // keep the header row pinned
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Actor");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Outcome");
        ImGui::TableSetupColumn("Detail");
        ImGui::TableHeadersRow();
        for (const std::shared_ptr<zetta::governance::AuditEvent>& event : events) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(event->time().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(
                std::string(to_string(event->action())).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                event->actor().empty() ? "—" : event->actor().c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(
                event->target().empty() ? "—" : event->target().c_str());
            ImGui::TableSetColumnIndex(4);
            if (event->allowed()) {
                ImGui::TextColored(kSuccessColor, "allow");
            } else {
                ImGui::TextColored(kErrorColor, "deny");
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextWrapped(
                "%s", event->detail().empty() ? "—" : event->detail().c_str());
        }
        ImGui::EndTable();
    }
}

}  // namespace

void App::draw_admin() {
    ImGui::SeparatorText("Admin & audit");

    // The governance surface (SCR-7 / CMP-5, the GUI slice of FEAT-9): a
    // **read-only** viewer over the node's already-validated governance state —
    // principals + their access keys (scope + capability grants) and the
    // immutable audit trail. Mutation (issuing keys, granting capabilities) is a
    // later slice; this realizes the "view principals/keys, grants, and audit
    // events" acceptance. Reads in-memory handles validated when they entered
    // from disk / the CRDT, so it introduces no new JSON boundary.

    // --- Principals & access keys (list + detail, like the Runs panel) ------
    ImGui::SeparatorText("Principals & access keys");
    const std::vector<std::string> principal_names = node_.principal_names();
    ImGui::TextDisabled("%zu principal(s)", principal_names.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;

    // Left: the principals. Each row is a kind badge plus the principal's name;
    // selecting one (held by name, re-resolved each frame) inspects its keys.
    ImGui::BeginChild("##principal_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (principal_names.empty()) ImGui::TextDisabled("(no principals)");
    for (const std::string& name : principal_names) {
        const std::shared_ptr<zetta::governance::Principal> principal = node_.find_principal(name);
        if (!principal) continue;
        ImGui::PushID(name.c_str());
        ImGui::TextDisabled("%-7s",
                            std::string(to_string(principal->kind())).c_str());
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected_principal_ == name)) {
            selected_principal_ = name;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected principal's keys (each with its scope + grants).
    ImGui::SameLine();
    ImGui::BeginChild("##principal_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const std::shared_ptr<zetta::governance::Principal> principal =
            node_.find_principal(selected_principal_)) {
        render_principal_detail(*principal);
    } else {
        ImGui::TextDisabled(
            "Select a principal to view its keys, scope, and capability grants.");
    }
    ImGui::EndChild();

    // --- Audit log (the immutable, append-only trail) -----------------------
    ImGui::SeparatorText("Audit log");
    if (mono_font_) ImGui::PushFont(mono_font_);
    render_audit_log(node_.audit_events());
    if (mono_font_) ImGui::PopFont();
}

}  // namespace zetta::gui
