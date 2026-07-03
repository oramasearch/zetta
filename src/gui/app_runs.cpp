#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

#include <imgui.h>

#include <cfloat>
#include <memory>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

// A run's status, color-coded along its lifecycle: pending (neutral) → running
// (amber) → succeeded (green) | failed (red). Same palette as the cluster panel.
ImVec4 run_status_color(zetta::pipeline::RunStatus status) {
    switch (status) {
        case zetta::pipeline::RunStatus::Pending:   return kInkQuiet;
        case zetta::pipeline::RunStatus::Running:   return kWarnColor;
        case zetta::pipeline::RunStatus::Succeeded: return kSuccessColor;
        case zetta::pipeline::RunStatus::Failed:    return kErrorColor;
    }
    return kInkQuiet;
}

// A log line's severity color — error red, warn amber, anything else neutral.
ImVec4 log_level_color(const std::string& level) {
    if (level == "error") return kErrorColor;
    if (level == "warn")  return kWarnColor;
    return kInkQuiet;
}

// Render the full detail of one run: identity + status, logs, outputs, the
// trained model(s) it produced (metrics + ranked drivers), and lineage. A pure
// view over the (already schema-validated) Run handle — no mutation. `models` are
// the node's models whose producing-run ref is this run (resolved by the caller).
void render_run_detail(const zetta::pipeline::Run& run,
                       const std::vector<std::shared_ptr<zetta::ml::Model>>& models) {
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
        for (const zetta::pipeline::RunLogEntry& entry : run.logs()) {
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
        for (const zetta::pipeline::RunOutput& output : run.outputs()) {
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
        for (const std::shared_ptr<zetta::ml::Model>& model : models) {
            ImGui::PushID(model->id().c_str());
            render_model_card(*model);
            ImGui::PopID();
        }
    }

    // Lineage — where the results came from (sources, steps), how fresh, and how
    // confident (a 0..1 score, shown as a bar).
    ImGui::SeparatorText("Lineage");
    const zetta::pipeline::RunLineage& lineage = run.lineage();
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

void App::draw_runs() {
    ImGui::SeparatorText("Runs / outputs / lineage");

    // A run is the record of one execution of a deployed graph (status / logs /
    // outputs / lineage). This panel is a read-only viewer: runs arrive via the
    // API, tests, or replication from a peer (executing a graph — StartRun — is
    // a later slice). Held by id and re-resolved each frame, like the explorer.
    const std::vector<std::shared_ptr<zetta::pipeline::Run>> runs = node_.runs();
    ImGui::TextDisabled("%zu run(s)", runs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 14.0f;

    // Left: the run list. Runs have no human name, so each row is a status badge
    // plus the first segment of the UUID — enough to tell them apart.
    ImGui::BeginChild("##run_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (runs.empty()) ImGui::TextDisabled("(no runs)");
    for (const std::shared_ptr<zetta::pipeline::Run>& run : runs) {
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
        std::vector<std::shared_ptr<zetta::ml::Model>> run_models;
        for (const std::shared_ptr<zetta::ml::Model>& model : node_.models()) {
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

}  // namespace zetta::gui
