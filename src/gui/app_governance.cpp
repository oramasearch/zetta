#include "gui/app.hpp"
#include "gui/app_internal.hpp"

#include "zetta/access_key.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/capability.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/scope.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/timestamp.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

// Render one access key: its short id + created stamp, the resources its **scope**
// reaches (deny-by-default), and the rights its **capabilities** grant (each TTL'd;
// an expired grant is flagged red). A pure view over the already-validated key —
// `now` is the current ISO-8601 stamp the capability TTLs are compared against.
void render_access_key(const AccessKey& key, const std::string& now) {
    const std::string short_id = key.id().substr(0, key.id().find('-'));
    ImGui::SeparatorText(("key " + short_id).c_str());
    mono_line(kInkQuiet, "created:  ", key.created());

    // Scope — which resources this key may touch (the resource axis, RULE-3).
    const Scope& scope = key.scope();
    if (scope.unrestricted) {
        ImGui::TextColored(kWarnColor, "scope: unrestricted (admin)");
    } else if (scope.grants.empty()) {
        ImGui::TextDisabled("scope: (no resources, denies every resource)");
    } else {
        ImGui::TextDisabled("scope:");
        ImGui::Indent();
        for (const Resource& resource : scope.grants) {
            ImGui::BulletText("%s: %s",
                              std::string(to_string(resource.kind)).c_str(),
                              resource.name.c_str());
        }
        ImGui::Unindent();
    }

    // Capabilities — which rights this key may exercise (the verb axis, ENT-22),
    // optionally TTL'd. No wildcard: a key with none denies every operation.
    const CapabilitySet& capabilities = key.capabilities();
    if (capabilities.grants.empty()) {
        ImGui::TextDisabled("capabilities: (none, denies every operation)");
    } else {
        ImGui::TextDisabled("capabilities:");
        ImGui::Indent();
        for (const Capability& capability : capabilities.grants) {
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
void render_principal_detail(const Principal& principal) {
    ImGui::Text("%s", principal.name().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]",
                        std::string(to_string(principal.kind())).c_str());
    mono_line(kInkQuiet, "id:       ", principal.id());
    mono_line(kInkQuiet, "created:  ", principal.created());

    const std::vector<std::shared_ptr<AccessKey>>& keys = principal.keys();
    ImGui::Spacing();
    ImGui::TextDisabled("%zu access key(s)", keys.size());
    if (keys.empty()) {
        ImGui::TextDisabled("(no keys, this principal can authenticate nothing)");
        return;
    }
    const std::string now = now_iso8601();
    for (const std::shared_ptr<AccessKey>& key : keys) {
        ImGui::PushID(key->id().c_str());
        render_access_key(*key, now);
        ImGui::PopID();
    }
}

// Render the immutable audit trail as a scrolling table: each governed action
// (graph change / deploy / data access) with its time, actor, target, the
// allow/deny outcome (color-coded), and the human-readable detail. Chronological,
// matching Node::audit_events(); a pure view over already-validated events.
void render_audit_log(const std::vector<std::shared_ptr<AuditEvent>>& events) {
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
        for (const std::shared_ptr<AuditEvent>& event : events) {
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

// Map a test outcome to the design system's semantic colour (green pass, red
// fail, amber error, quiet skip) — status rides on a dot AND the word, never colour
// alone.
ImVec4 test_outcome_color(testkit::TestOutcome outcome) {
    switch (outcome) {
        case testkit::TestOutcome::Pass:    return kSuccessColor;
        case testkit::TestOutcome::Fail:    return kErrorColor;
        case testkit::TestOutcome::Skipped: return kInkQuiet;
        case testkit::TestOutcome::Error:   return kWarnColor;
    }
    return kInkQuiet;
}

std::string test_outcome_label(testkit::TestOutcome outcome) {
    std::string label(testkit::to_string(outcome));
    for (char& character : label) {
        if (character >= 'a' && character <= 'z') character -= 32;  // ASCII upper
    }
    return label;
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
        const std::shared_ptr<Principal> principal = node_.find_principal(name);
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
    if (const std::shared_ptr<Principal> principal =
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

// The in-app Test suite (issue #462): a thin renderer over the headless
// zetta::testkit catalog. Pick a canned case from the categorised list, tune its
// options, optionally point it at custom data, and run it — the case exercises the
// real engine (a distributed case stands up a real multi-node loopback cluster) and
// reports a structured pass/fail with metrics + a log. The run happens on a worker
// thread so the UI stays responsive. See docs/test-suite.md.
void App::draw_test_suite() {
    ImGui::SeparatorText("Test suite");

    // Reap a finished worker (the run flag flips false as its last act).
    if (test_worker_ && !test_running_.load()) {
        test_worker_->join();
        test_worker_.reset();
    }

    const testkit::TestSuite& suite = testkit::TestSuite::builtin();
    ImGui::TextDisabled(
        "%zu canned cases over the real engine. Distributed cases stand up a real "
        "loopback cluster; runs execute off the UI thread.",
        suite.size());

    // Keep the editable options in sync with the selected case.
    const testkit::TestCase* selected = suite.find(selected_test_);
    if (selected && test_options_case_ != selected->id) {
        test_options_ = std::make_unique<testkit::TestOptions>(selected->defaults());
        test_options_case_ = selected->id;
        std::lock_guard<std::mutex> lock(test_mutex_);
        test_result_.reset();
        test_log_.clear();
    }

    const float list_w = ImGui::GetContentRegionAvail().x * 0.30f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 24.0f;

    // Left: the catalog, grouped by category.
    ImGui::BeginChild("##test_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(test_running_.load());  // don't switch cases mid-run
    const testkit::TestCategory categories[] = {
        testkit::TestCategory::Compute,     testkit::TestCategory::Ddf,
        testkit::TestCategory::Ml,          testkit::TestCategory::Distributed,
        testkit::TestCategory::Governance,  testkit::TestCategory::Pipeline,
        testkit::TestCategory::Examples};
    for (const testkit::TestCategory category : categories) {
        const std::vector<const testkit::TestCase*> cases = suite.in_category(category);
        if (cases.empty()) continue;
        ImGui::Spacing();
        eyebrow(mono_font_, std::string(testkit::category_title(category)).c_str());
        for (const testkit::TestCase* test_case : cases) {
            ImGui::PushID(test_case->id.c_str());
            if (ImGui::Selectable(test_case->name.c_str(),
                                  selected_test_ == test_case->id)) {
                selected_test_ = test_case->id;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    // Right: the selected case — description, options, run, and the last result.
    ImGui::SameLine();
    ImGui::BeginChild("##test_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (!selected) {
        ImGui::TextDisabled("Select a case to configure and run it.");
        ImGui::EndChild();
        return;
    }

    ImGui::TextColored(kInkPrimary, "%s", selected->name.c_str());
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::TextColored(kInkQuiet, "%s", selected->id.c_str());
    if (mono_font_) ImGui::PopFont();
    if (!selected->description.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", selected->description.c_str());
    }

    // Options modal (rendered inline, per the design system's density).
    if (!selected->options.empty() && test_options_) {
        ImGui::Spacing();
        eyebrow(mono_font_, "OPTIONS");
        ImGui::BeginDisabled(test_running_.load());
        for (const testkit::TestOption& option : selected->options) {
            ImGui::PushID(option.key.c_str());
            const std::string label = option.label + "##" + option.key;
            switch (option.type) {
                case testkit::OptionType::Bool: {
                    bool value = test_options_->get_bool(option.key, option.default_bool);
                    if (ImGui::Checkbox(label.c_str(), &value))
                        test_options_->set_bool(option.key, value);
                    break;
                }
                case testkit::OptionType::Int: {
                    int value = static_cast<int>(
                        test_options_->get_int(option.key, option.default_int));
                    ImGui::SetNextItemWidth(200.0f);
                    if (option.max_int > option.min_int) {
                        if (ImGui::SliderInt(label.c_str(), &value,
                                             static_cast<int>(option.min_int),
                                             static_cast<int>(option.max_int)))
                            test_options_->set_int(option.key, value);
                    } else if (ImGui::InputInt(label.c_str(), &value)) {
                        test_options_->set_int(option.key, value);
                    }
                    break;
                }
                case testkit::OptionType::Double: {
                    float value = static_cast<float>(
                        test_options_->get_double(option.key, option.default_double));
                    ImGui::SetNextItemWidth(200.0f);
                    if (option.max_double > option.min_double) {
                        if (ImGui::SliderFloat(label.c_str(), &value,
                                               static_cast<float>(option.min_double),
                                               static_cast<float>(option.max_double)))
                            test_options_->set_double(option.key, value);
                    } else if (ImGui::InputFloat(label.c_str(), &value)) {
                        test_options_->set_double(option.key, value);
                    }
                    break;
                }
                case testkit::OptionType::Choice: {
                    int value = test_options_->get_choice(
                        option.key, static_cast<int>(option.default_int));
                    const char* preview =
                        (value >= 0 && value < static_cast<int>(option.choices.size()))
                            ? option.choices[value].c_str()
                            : "";
                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::BeginCombo(label.c_str(), preview)) {
                        for (int i = 0; i < static_cast<int>(option.choices.size()); ++i) {
                            if (ImGui::Selectable(option.choices[i].c_str(), value == i))
                                test_options_->set_choice(option.key, i);
                        }
                        ImGui::EndCombo();
                    }
                    break;
                }
                case testkit::OptionType::Text: {
                    std::string value =
                        test_options_->get_text(option.key, option.default_text);
                    ImGui::SetNextItemWidth(280.0f);
                    if (ImGui::InputText(label.c_str(), &value))
                        test_options_->set_text(option.key, std::move(value));
                    break;
                }
            }
            if (!option.help.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option.help.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }

    // Custom data (used by the data-driven cases — ML / pipeline).
    ImGui::Spacing();
    ImGui::BeginDisabled(test_running_.load());
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("Custom dataset dir##test_custom",
                             "optional: a churn dataset directory", &test_custom_data_);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "ML and pipeline cases load this churn dataset directory instead of the "
            "built-in synthetic one. Leave empty to use the built-in dataset.");
    ImGui::EndDisabled();

    // Run.
    ImGui::Spacing();
    ImGui::BeginDisabled(test_running_.load());
    ImGui::PushStyleColor(ImGuiCol_Button, kInkPrimary);
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    if (ImGui::Button(test_running_.load() ? "Running…" : "Run test")) {
        start_test(*selected);
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();

    ImGui::Separator();

    // Result / progress.
    if (test_running_.load()) {
        ImGui::TextColored(kInkMuted, "running %s…", test_running_id_.c_str());
    } else if (test_result_) {
        const testkit::TestResult& result = *test_result_;
        const ImVec4 color = test_outcome_color(result.outcome);
        status_dot(color);
        ImGui::TextColored(color, "%s", test_outcome_label(result.outcome).c_str());
        ImGui::SameLine();
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkQuiet, "· %.3fs", result.elapsed_seconds);
        if (mono_font_) ImGui::PopFont();
        const std::string& message =
            result.summary.empty() ? result.error : result.summary;
        if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());

        if (!result.metrics.empty()) {
            ImGui::Spacing();
            if (ImGui::BeginTable("##test_metrics", 2,
                                  ImGuiTableFlags_SizingFixedFit)) {
                for (const testkit::TestMetric& metric : result.metrics) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(kInkMuted, "%s", metric.label.c_str());
                    ImGui::TableNextColumn();
                    if (mono_font_) ImGui::PushFont(mono_font_);
                    ImGui::TextColored(kInkPrimary, "%s", metric.value.c_str());
                    if (mono_font_) ImGui::PopFont();
                }
                ImGui::EndTable();
            }
        }
    } else {
        ImGui::TextDisabled("Not run yet.");
    }

    // The live / final log.
    std::vector<std::string> log_copy;
    {
        std::lock_guard<std::mutex> lock(test_mutex_);
        log_copy = test_log_;
    }
    if (!log_copy.empty()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "LOG");
        if (mono_font_) ImGui::PushFont(mono_font_);
        for (const std::string& line : log_copy) {
            ImGui::TextColored(kInkSecondary, "%s", line.c_str());
        }
        if (mono_font_) ImGui::PopFont();
    }

    ImGui::EndChild();
}

void App::start_test(const testkit::TestCase& test_case) {
    if (test_running_.load()) return;
    // Snapshot the options + custom-data path so the worker is self-contained. The
    // TestCase lives in the process-lifetime static catalog, so the pointer is safe.
    auto options = std::make_shared<testkit::TestOptions>(
        test_options_ ? *test_options_ : test_case.defaults());
    const std::string custom = test_custom_data_;
    const testkit::TestCase* test_case_ptr = &test_case;
    {
        std::lock_guard<std::mutex> lock(test_mutex_);
        test_log_.clear();
        test_result_.reset();
    }
    test_running_id_ = test_case.id;
    test_running_.store(true);
    test_worker_ = std::make_unique<std::thread>([this, test_case_ptr, options, custom] {
        testkit::TestContext context;
        context.custom_data_path = custom;
        context.log = [this](std::string line) {
            std::lock_guard<std::mutex> lock(test_mutex_);
            test_log_.push_back(std::move(line));
        };
        testkit::TestResult result =
            testkit::run_test_case(*test_case_ptr, *options, context);
        {
            std::lock_guard<std::mutex> lock(test_mutex_);
            for (const std::string& line : result.log) test_log_.push_back(line);
            test_result_ = std::make_unique<testkit::TestResult>(std::move(result));
        }
        test_running_.store(false);  // last act: the render loop then reaps the worker
    });
}

}  // namespace zetta::gui
