#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"
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

// Render the full detail of one run: identity + status, logs, outputs, the
// trained model(s) it produced (metrics + ranked drivers), and lineage. A pure
// view over the (already schema-validated) Run handle — no mutation. `models` are
// the node's models whose producing-run ref is this run (resolved by the caller).
void render_run_detail(const zetta::pipeline::Run& run,
                       const std::vector<std::shared_ptr<zetta::ml::Model>>& models) {
    detail_header("run " + run.id(), tone_of(run.status()),
                  std::string(to_string(run.status())));
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
            ImGui::TextColored(tone_color(tone_of_log_level(entry.level)), "%-5s",
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
    page_header("Runs",
                "Status, logs, outputs, and lineage for each execution of a "
                "deployed graph");

    // A run is the record of one execution of a deployed graph (status / logs /
    // outputs / lineage). This panel is a read-only viewer: runs arrive via the
    // API, tests, or replication from a peer (executing a graph — StartRun — is
    // a later slice). Held by id and re-resolved each frame, like the explorer.
    const std::vector<std::shared_ptr<zetta::pipeline::Run>> runs = node_.runs();
    ImGui::TextDisabled("%zu run(s)", runs.size());

    // Runs have no human name, so each row's title is the UUID's first segment
    // with the lifecycle status as the dot + meta.
    std::vector<MasterRow> rows;
    rows.reserve(runs.size());
    for (const std::shared_ptr<zetta::pipeline::Run>& run : runs) {
        rows.push_back({.id = run->id(),
                        .title = entity_short_id(run->id()),
                        .meta = std::string(to_string(run->status())),
                        .tone = tone_of(run->status()),
                        .id_title = true});
    }

    master_detail(
        "runs", rows, selected_run_, take_pending_selection(View::Runs),
        "(no runs)",
        "Select a run to view its status, logs, outputs, models, and lineage.",
        [&](const MasterRow& row) {
            const auto run = node_.find_run(row.id);
            if (!run) return;
            // Resolve the trained model(s) this run produced via the durable
            // reverse link (Model::run() == run id), so the detail pane can
            // surface their metrics + ranked drivers alongside the run's
            // outputs.
            std::vector<std::shared_ptr<zetta::ml::Model>> run_models;
            for (const std::shared_ptr<zetta::ml::Model>& model : node_.models()) {
                if (model->run() == run->id()) run_models.push_back(model);
            }
            render_run_detail(*run, run_models);
        });
}

}  // namespace zetta::gui
