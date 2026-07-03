#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/util/bytes.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <cfloat>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace zetta::gui {

// Render one trained model produced by a run: its identity + weights fingerprint,
// its evaluation metrics (name→number), and its ranked feature drivers (the
// SHAP-style explainability surface). A pure view over an already-validated Model
// handle; metrics/drivers come from the model's free-form (already schema-
// validated) fields, so no new JSON boundary is introduced.
void render_model_card(const zetta::ml::Model& model) {
    const std::string short_id = model.id().substr(0, model.id().find('-'));
    ImGui::SeparatorText(
        (model.name().empty() ? ("model " + short_id) : model.name()).c_str());
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkQuiet, "kind: %s  ·  id: %s", model.kind().c_str(),
                       short_id.c_str());
    if (model.has_weights()) {
        ImGui::TextColored(
            kInkQuiet, "weights: %s%s", zetta::util::format_bytes(model.weights()->size).c_str(),
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
    const std::vector<zetta::ml::ModelDriver> drivers = model.drivers();
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
            for (const zetta::ml::ModelDriver& driver : drivers) {
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

void App::reset_add_model_form() {
    add_model_name_.clear();
    add_model_kind_ = "classifier";
    add_model_metrics_.clear();
    add_model_features_.clear();
    add_model_weights_path_.clear();
    add_model_weights_name_.clear();
    add_model_error_.clear();
}

void App::draw_add_model_form() {
    // The inline register/import affordance, collapsed by default so the list
    // stays the focus. A model is keyed by id (no name collision), so Register
    // always creates a new one. The pasted metrics / feature_schema are parsed
    // here, and the assembled value is re-validated against the Model JSON Schema
    // (validate_model_value) at this boundary — so malformed input (e.g. a
    // non-numeric metric) fails fast here instead of being rejected on the next
    // reconcile. Weights are optional and attached after create via set_weights.
    if (!ImGui::CollapsingHeader("Add a model")) return;

    ImGui::TextDisabled(
        "Register a trained model on this node. Kind is required; metrics and "
        "feature schema are optional JSON; weights are optional. It then appears "
        "below and in system.models.");
    ImGui::Spacing();

    // 1) Name (optional) + kind (required).
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##am_name", "optional, e.g. churn-v1",
                             &add_model_name_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Kind");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##am_kind", "required, e.g. classifier",
                             &add_model_kind_);

    // 2) Metrics (optional) — a JSON object of name→number (the schema requires
    //    numeric values). Surfaced in the detail pane's Metrics table.
    ImGui::Spacing();
    ImGui::TextUnformatted("Metrics (JSON)");
    ImGui::InputTextMultiline("##am_metrics", &add_model_metrics_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::TextDisabled(
        "Optional object of numbers, e.g. {\"auc\": 0.91, \"f1\": 0.85}.");

    // 3) Feature schema (optional) — a free-form JSON object. A `drivers` array
    //    inside it renders as the detail pane's ranked drivers.
    ImGui::Spacing();
    ImGui::TextUnformatted("Feature schema (JSON)");
    ImGui::InputTextMultiline("##am_features", &add_model_features_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::TextDisabled(
        "Optional object. A \"drivers\":[{\"feature\",\"importance\"}] array shows "
        "as ranked drivers.");

    // 4) Weights (optional) — a file staged via the native dialog (or a drop
    //    routed to FileIntent::ModelWeights), attached on Register.
    ImGui::Spacing();
    ImGui::TextUnformatted("Weights (file)");
    if (ImGui::Button("Choose weights file...")) {
        SDL_ShowOpenFileDialog(&App::on_open_model_weights, this, window_, nullptr,
                               0, nullptr, false);
    }
    ImGui::SameLine();
    if (add_model_weights_path_.empty()) {
        ImGui::TextDisabled("optional; opaque bytes, fingerprinted on attach");
    } else {
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "%s", add_model_weights_name_.c_str());
        if (mono_font_) ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            add_model_weights_path_.clear();
            add_model_weights_name_.clear();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (!add_model_error_.empty()) {
        ImGui::TextColored(tone_color(Tone::Error), "%s", add_model_error_.c_str());
    }

    if (primary_button("Register model")) {
        add_model_error_.clear();

        // Inline JSON-object parser for the optional metrics / feature_schema
        // fields: blank → an empty object; otherwise the text must parse to an
        // object (the Model schema requires both to be objects).
        const auto blank = [](const std::string& text) {
            return text.find_first_not_of(" \t\r\n") == std::string::npos;
        };
        const auto parse_object = [&](const std::string& text, const char* label,
                                      nlohmann::json& out,
                                      std::string& error) -> bool {
            if (blank(text)) {
                out = nlohmann::json::object();
                return true;
            }
            nlohmann::json parsed =
                nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded()) {
                error = std::string(label) + " is not valid JSON";
                return false;
            }
            if (!parsed.is_object()) {
                error = std::string(label) + " must be a JSON object";
                return false;
            }
            out = std::move(parsed);
            return true;
        };

        zetta::ml::ModelValue value;
        value.name = add_model_name_;
        value.kind = add_model_kind_;
        nlohmann::json metrics;
        nlohmann::json features;
        std::string error;
        if (blank(add_model_kind_)) {
            add_model_error_ = "kind is required";
        } else if (!parse_object(add_model_metrics_, "metrics", metrics, error)) {
            add_model_error_ = error;
        } else if (!parse_object(add_model_features_, "feature schema", features,
                                 error)) {
            add_model_error_ = error;
        } else {
            value.metrics = std::move(metrics);
            value.feature_schema = std::move(features);
            // Re-validate the assembled value against the Model JSON Schema (e.g.
            // metrics values must be numbers) before create — the same gate the
            // record must pass when it reloads / reconciles. create_model stamps
            // `created` + leaves weights unset, both schema-valid, so a value that
            // passes here also passes on reload.
            if (!zetta::ml::validate_model_value(zetta::ml::to_json(value), &error)) {
                add_model_error_ = error;
            } else if (auto model = node_.create_model(value)) {
                std::string summary = "Registered model";
                if (!model->name().empty()) summary += " '" + model->name() + "'";
                summary += " (" + model->kind() + ")";
                // Attach optional weights from the staged file (set_weights writes
                // the body to disk, fingerprints it, and replicates the metadata).
                if (!add_model_weights_path_.empty()) {
                    std::ifstream in(add_model_weights_path_, std::ios::binary);
                    if (in) {
                        const std::string bytes(
                            (std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
                        summary += model->set_weights(bytes)
                                       ? " + weights (" +
                                             zetta::util::format_bytes(bytes.size()) + ")"
                                       : " (weights attach failed)";
                    } else {
                        summary += " (weights file unreadable)";
                    }
                }
                selected_model_ = model->id();
                io_status_ = summary;
                reset_add_model_form();
            } else {
                add_model_error_ = "could not create the model";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) reset_add_model_form();
}

void App::draw_models() {
    page_header("Models",
                "Trained models: metrics, ranked drivers, and weights");

    // The GUI form of system.models: the trained-model registry (ENT-14 /
    // FEAT-6). A thin, read-only viewer over the replicated, already-validated
    // Model handles — models arrive from a train job, the API, or replication —
    // plus the inline "Add model" affordance to register/import one. Held by id and
    // re-resolved each frame, like the Runs panel.
    const std::vector<std::shared_ptr<zetta::ml::Model>> models = node_.models();
    ImGui::TextDisabled("%zu model(s)", models.size());

    draw_add_model_form();

    // Models have no lifecycle status (muted dot); each row's title is the
    // name (or the UUID's first segment when unnamed) with the kind as meta.
    std::vector<MasterRow> rows;
    rows.reserve(models.size());
    for (const std::shared_ptr<zetta::ml::Model>& model : models) {
        rows.push_back({.id = model->id(),
                        .title = model->name().empty()
                                     ? entity_short_id(model->id())
                                     : model->name(),
                        .meta = model->kind(),
                        .id_title = model->name().empty()});
    }

    master_detail(
        "models", rows, selected_model_, take_pending_selection(View::Models),
        "(no models)",
        "Select a model to view its metrics, ranked drivers, and weights "
        "fingerprint.",
        [&](const MasterRow& row) {
            // The selected model's detail — metrics, ranked drivers, and
            // weights fingerprint (render_model_card, reused from the Runs
            // panel), plus its creation stamp and lineage refs.
            const std::shared_ptr<zetta::ml::Model> model = node_.find_model(row.id);
            if (!model) return;
            render_model_card(*model);
            ImGui::Spacing();
            mono_line(kInkQuiet, "created:  ", model->created());
            if (!model->project().empty()) {
                mono_line(kInkQuiet, "project:  ", model->project());
            }
            if (!model->run().empty()) {
                mono_line(kInkQuiet, "run:      ", model->run());
            }
        });
}

}  // namespace zetta::gui
