// SPDX-License-Identifier: MIT

// The Reports view (#960 item 4) — what a finished run FOUND, in the desktop
// Studio surface. The information architecture and copy come from the design's
// Reports screens (the design repository's Reports surface); the numbers
// come from the engine's own `get_run_report`.
//
// **One behavior path.** This calls the capability registry rather than reading
// the Model and the at-risk table itself. The registry is the only surface that
// produces a report, so the GUI, the web app, MCP and chat cannot disagree about
// what a run found — which they would the moment two of them assembled it
// separately.
//
// **The report's discipline travels with the figures.** A surface that shows
// the numbers without it undoes the work that produced them: the held-out figure
// leads and says what it was measured on, accuracy never appears without the
// accuracy a constant answer would score, and `missing` is drawn rather than
// skipped — a run that measured no held-out figure has to say so, or a reader
// takes the in-sample number for quality.
#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"
#include "gui/tones.hpp"

#include "zetta/api/capabilities.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/run.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

std::string fixed(double value, int places) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
    return buffer;
}

// A count with thousands separators — "3,870,209" reads as a scale, "3870209"
// reads as a serial number. Entity keys are NOT run through this: an account id
// is an identifier, and "8,336" is a number that does not exist.
std::string grouped(double value) {
    std::string digits = fixed(value < 0 ? -value : value, 0);
    for (std::size_t at = digits.size(); at > 3;) {
        at -= 3;
        digits.insert(at, ",");
    }
    return value < 0 ? "-" + digits : digits;
}

std::optional<double> number_at(const nlohmann::json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return std::nullopt;
    return found->get<double>();
}

}  // namespace

void App::draw_reports() {
    page_header("Reports",
                "What a finished run found: model quality, what drives the "
                "outcome, and who is most at risk");

    // Only runs that actually emitted a report. A run without one is not a
    // broken report — it is a run of a pipeline that does not produce one.
    std::vector<MasterRow> rows;
    for (const std::shared_ptr<zetta::pipeline::Run>& run : node_.runs()) {
        if (!run) continue;
        bool has_report = false;
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind == "report") has_report = true;
        }
        if (!has_report) continue;
        rows.push_back({.id = run->id(),
                        .title = run->graph().empty() ? entity_short_id(run->id())
                                                      : run->graph(),
                        .meta = run->updated(),
                        .id_title = run->graph().empty()});
    }
    // Newest first: the report someone wants is almost always the last one.
    std::sort(rows.begin(), rows.end(), [](const MasterRow& left, const MasterRow& right) {
        return left.meta > right.meta;
    });

    if (rows.empty()) {
        if (empty_state("No reports yet",
                        "A Pipeline run produces its report here when it "
                        "finishes: model quality, the drivers, and the "
                        "ranked at-risk list.",
                        "Open runs")) {
            navigate_to(View::Runs);
        }
        return;
    }

    std::optional<std::string> pending = take_pending_selection(View::Reports);
    master_detail("reports", rows, selected_report_run_, std::move(pending),
                  "(no reports)",
                  "Select a run to read what it found.",
                  [&](const MasterRow& row) { draw_run_report(row.id); });
}

void App::draw_run_report(const std::string& run_id) {
    if (capability_registry_ == nullptr) {
        banner(Tone::Warn,
               "No capability registry is wired, so this node cannot assemble a "
               "report.");
        return;
    }
    api::ApiContext context{.node = node_,
                            .cluster = &cluster_,
                            .acting_key = {},
                            .trace = zetta::observability::make_root_trace(),
                            .recorder = &node_.span_recorder()};
    const api::ApiResult result =
        capability_registry_->call("get_run_report", {{"run_id", run_id}}, context);
    if (!result.ok) {
        banner(Tone::Warn, result.error.message);
        return;
    }
    const nlohmann::json& report = result.value;
    const nlohmann::json& model = report.value("model", nlohmann::json::object());
    const nlohmann::json& dataset = report.value("dataset", nlohmann::json::object());

    // --- How good the model is ------------------------------------------------
    section_card("report_quality", "How good the model is", [&] {
        const std::optional<double> held = number_at(model, "held_out_auc");
        const std::optional<double> in_sample = number_at(model, "in_sample_auc");
        if (held) {
            // The held-out figure leads. It is the one that describes behavior on
            // an account the model has not seen.
            stat_tile("HELD OUT \xC2\xB7 AUC", fixed(*held, 3), Tone::Ok);
            if (in_sample) {
                ImGui::SameLine(0.0f, 32.0f);
                stat_tile("IN SAMPLE \xC2\xB7 AUC", fixed(*in_sample, 3));
            }
            ImGui::Spacing();
            const std::optional<double> held_rows = number_at(model, "held_out_rows");
            gloss("Measured on " +
                  (held_rows ? grouped(*held_rows) + " rows" : std::string("rows")) +
                  " the model never trained on. Draw one account that churned and one "
                  "that did not: it scores the churner higher about " +
                  fixed(*held * 100.0, 0) +
                  " times in a hundred. The gap to the in-sample figure is what says "
                  "whether it memorized its training set.");
        } else if (in_sample) {
            stat_tile("IN SAMPLE \xC2\xB7 AUC", fixed(*in_sample, 3), Tone::Warn);
            ImGui::Spacing();
            gloss("Measured over the rows the model TRAINED on, so it flatters. This "
                  "run asked for no holdout, so there is no figure here for how it "
                  "behaves on an account it has not seen.");
        }
        // Accuracy never appears alone: on any problem where one outcome is
        // uncommon, a constant answer scores close to the model.
        if (const std::optional<double> accuracy = number_at(model, "in_sample_accuracy")) {
            ImGui::Spacing();
            const std::optional<double> baseline =
                number_at(dataset, "majority_class_accuracy");
            stat_tile("ACCURACY", fixed(*accuracy * 100.0, 1) + "%");
            if (baseline) {
                ImGui::SameLine(0.0f, 32.0f);
                stat_tile("ALWAYS \"NO CHURN\"", fixed(*baseline * 100.0, 1) + "%");
                ImGui::Spacing();
                gloss("Accuracy barely separates a working model from a constant "
                      "answer here, which is what it does whenever one outcome is "
                      "uncommon. Judge the model by the ranking figure above.");
            }
        }
    });

    // --- What it was built from ------------------------------------------------
    if (!dataset.empty()) {
        section_card("report_dataset", "What it was built from", [&] {
            bool first = true;
            const auto tile = [&](const char* label, const char* key, int places,
                                  bool percent) {
                const std::optional<double> value = number_at(dataset, key);
                if (!value) return;
                if (!first) ImGui::SameLine(0.0f, 32.0f);
                first = false;
                stat_tile(label, percent ? fixed(*value * 100.0, places) + "%"
                                         : grouped(*value));
            };
            tile("ACCOUNTS", "accounts", 0, false);
            tile("EVENTS", "events", 0, false);
            tile("CHURN RATE", "churn_rate", 1, true);
        });
    }

    // --- What drives the outcome -----------------------------------------------
    if (const auto drivers = report.find("drivers");
        drivers != report.end() && drivers->is_array() && !drivers->empty()) {
        section_card("report_drivers", "What drives the outcome", [&] {
            double top = 0.0;
            for (const nlohmann::json& driver : *drivers) {
                top = std::max(top, std::abs(driver.value("weight", 0.0)));
            }
            for (const nlohmann::json& driver : *drivers) {
                const double weight = driver.value("weight", 0.0);
                const float fraction =
                    top > 0.0 ? static_cast<float>(std::abs(weight) / top) : 0.0f;
                weight_bar(driver.value("feature", ""), fraction, fixed(weight, 3));
            }
            ImGui::Spacing();
            gloss("A weight is how much a column moved predictions across the "
                  "population. It is not a cause: for several of these the arrow "
                  "plausibly runs the other way.");
        });
    }

    // --- Who is most at risk ----------------------------------------------------
    if (const auto at_risk = report.find("at_risk");
        at_risk != report.end() && at_risk->is_array() && !at_risk->empty()) {
        section_card("report_at_risk", "Who is most at risk", [&] {
            // Evidence columns are the same on every row (the population's
            // strongest drivers), so the table is comparable down a column as well
            // as across one.
            std::vector<std::string> evidence;
            if (const auto first = at_risk->front().find("evidence");
                first != at_risk->front().end() && first->is_object()) {
                for (const auto& [key, value] : first->items()) evidence.push_back(key);
            }
            const int columns = 3 + static_cast<int>(evidence.size());
            if (ImGui::BeginTable("at_risk", columns, kDataTableFlags)) {
                // Sentence case, per the design system.
                ImGui::TableSetupColumn("Rank");
                ImGui::TableSetupColumn("Account");
                ImGui::TableSetupColumn("Risk");
                for (const std::string& column : evidence) {
                    ImGui::TableSetupColumn(column.c_str());
                }
                ImGui::TableHeadersRow();
                for (const nlohmann::json& entry : *at_risk) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    table_numeric(std::to_string(entry.value("rank", 0LL)));
                    ImGui::TableNextColumn();
                    // An identifier, not a quantity: never thousands-separated.
                    table_numeric(std::to_string(entry.value("entity", 0LL)));
                    ImGui::TableNextColumn();
                    table_numeric(fixed(entry.value("risk", 0.0), 3));
                    const auto values = entry.find("evidence");
                    for (const std::string& column : evidence) {
                        ImGui::TableNextColumn();
                        if (values != entry.end() && values->contains(column) &&
                            (*values)[column].is_number()) {
                            table_numeric(fixed((*values)[column].get<double>(), 0));
                        } else {
                            ImGui::TextColored(kInkQuiet, "\xC2\xB7");
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            gloss("Read the evidence columns ACROSS each other, not down any one. An "
                  "account high on several is in a different situation from one "
                  "carried by a single large number, and the second is the likelier "
                  "false positive. A ranked list is a way to spend attention where it "
                  "pays best, not a prediction about any one account.");
        });
    }

    // --- What to check before acting on this ------------------------------------
    // Drawn, never skipped. A missing held-out figure that goes unshown is a
    // reader quoting the in-sample number as quality.
    section_card("report_caveats", "What to check before acting on this", [&] {
        if (const auto missing = report.find("missing");
            missing != report.end() && missing->is_array()) {
            for (const nlohmann::json& entry : *missing) {
                ImGui::TextColored(kWarnColor, "%s", entry.value("field", "").c_str());
                gloss(entry.value("why", ""));
                ImGui::Spacing();
            }
        }
        gloss("Risk ranks; it is not a calibrated probability unless calibration was "
              "enabled for this run. A driver is a weight, not a cause: the model "
              "leaning on a column does not establish that the column causes the "
              "outcome.");
    });
}

}  // namespace zetta::gui
