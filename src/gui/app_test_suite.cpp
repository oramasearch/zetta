#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"

#include "zetta/node.hpp"
#include "zetta/testkit/test_suite.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

// Map a test outcome to the design system's semantic colour (green pass, red
// fail, amber error, quiet skip) — status rides on a dot AND the word, never colour
// alone.
std::string test_outcome_label(testkit::TestOutcome outcome) {
    std::string label(testkit::to_string(outcome));
    for (char& character : label) {
        if (character >= 'a' && character <= 'z') character -= 32;  // ASCII upper
    }
    return label;
}

}  // namespace

// The in-app Test suite (issue #462): a thin renderer over the headless
// zetta::testkit catalog. Pick a canned case from the categorised list, tune its
// options, optionally point it at custom data, and run it — the case exercises the
// real engine (a distributed case stands up a real multi-node loopback cluster) and
// reports a structured pass/fail with metrics + a log. The run happens on a worker
// thread so the UI stays responsive. See docs/testing/test-suite.md.
void App::draw_test_suite() {
    const testkit::TestSuite& suite = testkit::TestSuite::builtin();
    const std::string subtitle =
        std::to_string(suite.size()) +
        " canned cases over the real engine. Distributed cases stand up a "
        "real loopback cluster; runs execute off the UI thread.";
    page_header("Test suite", subtitle.c_str());

    // Reap a finished worker (the run flag flips false as its last act).
    if (test_worker_ && !test_running_.load()) {
        test_worker_->join();
        test_worker_.reset();
    }

    // A run pins the selection to the case in flight: the worker owns the
    // live log + result until it finishes, so the catalog can't switch state
    // mid-run (the pre-scaffold list disabled itself for the same reason).
    if (test_running_.load() && !test_running_id_.empty()) {
        selected_test_ = test_running_id_;
    }

    // Keep the editable options in sync with the selected case.
    const testkit::TestCase* selected = suite.find(selected_test_);
    if (selected && test_options_case_ != selected->id) {
        test_options_ = std::make_unique<testkit::TestOptions>(selected->defaults());
        test_options_case_ = selected->id;
        std::lock_guard<std::mutex> lock(test_mutex_);
        test_result_.reset();
        test_log_.clear();
    }

    // The catalog as master-detail rows (#634), grouped by category via the
    // scaffold's section eyebrows; the case in flight reads Active.
    const testkit::TestCategory categories[] = {
        testkit::TestCategory::Compute,     testkit::TestCategory::Ddf,
        testkit::TestCategory::Ml,          testkit::TestCategory::Distributed,
        testkit::TestCategory::Governance,  testkit::TestCategory::Pipeline,
        testkit::TestCategory::Examples};
    std::vector<MasterRow> rows;
    for (const testkit::TestCategory category : categories) {
        for (const testkit::TestCase* test_case : suite.in_category(category)) {
            MasterRow row;
            row.id = test_case->id;
            row.title = test_case->name;
            row.section = std::string(testkit::category_title(category));
            row.tone = (test_running_.load() && test_running_id_ == test_case->id)
                           ? Tone::Active
                           : Tone::Muted;
            rows.push_back(std::move(row));
        }
    }

    master_detail("##test_suite", rows, selected_test_,
                  take_pending_selection(View::Tests),
                  "No test cases in this build.",
                  "Select a case to configure and run it.",
                  [&](const MasterRow& row) {
                      if (const testkit::TestCase* current = suite.find(row.id)) {
                          draw_test_detail(*current);
                      }
                  });
}

// The detail pane: description, options, the run action, and the last result +
// live log for the selected case.
void App::draw_test_detail(const testkit::TestCase& test_case) {
    const testkit::TestCase* selected = &test_case;

    // The standard detail opener (#634): mono title + the headline state.
    Tone header_tone = Tone::Muted;
    std::string header_status;
    if (test_running_.load()) {
        header_tone = Tone::Active;
        header_status = "running";
    } else if (test_result_) {
        header_tone = tone_of(test_result_->outcome);
        header_status = test_outcome_label(test_result_->outcome);
    }
    detail_header(selected->name, header_tone, header_status);
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

    // Run — the pane's one focal action (the design system's primary button).
    ImGui::Spacing();
    ImGui::BeginDisabled(test_running_.load());
    if (primary_button(test_running_.load() ? "Running…" : "Run test")) {
        start_test(*selected);
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // Result / progress. The headline outcome is the detail_header's pill; the
    // body carries the timing, the banner, and the metrics.
    if (test_running_.load()) {
        ImGui::TextColored(kInkMuted, "running %s…", test_running_id_.c_str());
    } else if (test_result_) {
        const testkit::TestResult& result = *test_result_;
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkQuiet, "%.3fs", result.elapsed_seconds);
        if (mono_font_) ImGui::PopFont();
        // The run's outcome as the per-panel banner (#632): the case's summary,
        // or its failure reason verbatim, toned by the outcome.
        const std::string& message =
            result.summary.empty() ? result.error : result.summary;
        banner(tone_of(result.outcome), message);

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
        // Every case runs inside a trace (#640): record into the live node's
        // ring (mutex-guarded, safe from this worker thread) so the run shows
        // up in system.traces / the trace pane like any other operation.
        context.recorder = &node_.span_recorder();
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
