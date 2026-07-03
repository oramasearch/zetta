#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/compute/compute_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

#include <imgui.h>

#include <cfloat>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zetta::gui {

// The full detail of one run: identity + status, its place in the object
// graph as one-click entity links (#633) — project / graph / deployment /
// the compute jobs it spawned — then logs, outputs, the trained model(s) it
// produced (metrics + ranked drivers), and lineage. A read-only view over the
// (already schema-validated) Run handle; every reference is re-resolved
// against the live node this frame, and a ref that resolves to nothing stays
// plain mono — a link never fakes a destination.
void App::draw_run_detail(const zetta::pipeline::Run& run) {
    detail_header("run " + run.id(), tone_of(run.status()),
                  std::string(to_string(run.status())));
    if (!run.project().empty()) {
        // Runs carry the project uuid; the explorer selects by name.
        const std::string project_name = project_name_for(run.project());
        if (project_name.empty()) {
            mono_line(kInkQuiet, "project:  ", run.project());
        } else {
            link_line("project:  ", project_name, View::Explorer, project_name);
        }
    }
    if (!run.graph().empty()) {
        // A uuid graph ref collapses to its first segment (the activity feed's
        // short-target convention); a saved name displays as-is.
        const bool uuid_like =
            run.graph().size() == 36 && run.graph().find('-') == 8;
        const std::string display =
            uuid_like ? entity_short_id(run.graph()) : run.graph();
        // Link the graph ref (a stored-graph id or saved name) when a project
        // actually stores it — the workspace's deep link resolves either form.
        if (graph_is_stored(run.graph())) {
            link_line("graph:    ", display, View::Graph, run.graph());
        } else {
            mono_line(kInkQuiet, "graph:    ", display);
        }
    }
    if (!run.deployment().empty()) {
        if (node_.find_deployment(run.deployment())) {
            link_line("deploy:   ", entity_short_id(run.deployment()),
                      View::Deploy, run.deployment());
        } else {
            mono_line(kInkQuiet, "deploy:   ", entity_short_id(run.deployment()));
        }
    }
    mono_line(kInkQuiet, "created:  ", run.created());
    if (run.updated() != run.created()) {
        mono_line(kInkQuiet, "updated:  ", run.updated());
    }

    // The compute jobs this run spawned — resolved by the job's back-ref
    // (ComputeJob::run() == this run's id), each a one-click hop to Jobs.
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> run_jobs;
    for (const std::shared_ptr<zetta::compute::ComputeJob>& job : node_.jobs()) {
        if (job && job->run() == run.id()) run_jobs.push_back(job);
    }
    if (!run_jobs.empty()) {
        ImGui::SeparatorText("Compute jobs");
        for (const std::shared_ptr<zetta::compute::ComputeJob>& job : run_jobs) {
            ImGui::PushID(job->id().c_str());
            entity_link(entity_short_id(job->id()).c_str(), View::Jobs,
                        job->id());
            ImGui::SameLine();
            ImGui::TextDisabled("%s · %s",
                                std::string(to_string(job->kind())).c_str(),
                                std::string(to_string(job->status())).c_str());
            ImGui::PopID();
        }
    }

    const ImGuiTableFlags table_flags = kDataTableFlags;

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
            // A model ref that resolves to a real Model is one click from its
            // panel; unresolvable refs (the default executor's logical
            // handles) stay plain — the same rule as the graph-run overlay.
            if (output.kind == "model" && node_.find_model(output.ref)) {
                ImGui::PushID(output.ref.c_str());
                entity_link(output.ref.c_str(), View::Models, output.ref);
                ImGui::PopID();
            } else {
                ImGui::TextUnformatted(output.ref.c_str());
            }
        }
        if (g_mono_font) ImGui::PopFont();
        ImGui::EndTable();
    }

    // Trained models — the model(s) this run produced, with their metrics and
    // ranked drivers (the FEAT-5 "surface the trained Model + metrics + ranked
    // drivers" slice). Linked by the Model's producing-run ref (Model::run() ==
    // this run's id), the durable edge — the executor's logical model output ref
    // is just a placeholder handle in R1. Each card links into the Models view.
    std::vector<std::shared_ptr<zetta::ml::Model>> run_models;
    for (const std::shared_ptr<zetta::ml::Model>& model : node_.models()) {
        if (model && model->run() == run.id()) run_models.push_back(model);
    }
    if (!run_models.empty()) {
        ImGui::SeparatorText("Trained models");
        for (const std::shared_ptr<zetta::ml::Model>& model : run_models) {
            ImGui::PushID(model->id().c_str());
            render_model_card(*model);
            entity_link("open in models", View::Models, model->id());
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

void App::draw_runs() {
    page_header("Runs",
                "Status, logs, outputs, and lineage for each execution of a "
                "deployed graph");

    // A run is the record of one execution of a deployed graph (status / logs /
    // outputs / lineage). This panel is a read-only viewer: runs arrive via the
    // API, tests, or replication from a peer (executing a graph — StartRun — is
    // a later slice). Held by id and re-resolved each frame, like the explorer.
    const std::vector<std::shared_ptr<zetta::pipeline::Run>> runs = node_.runs();
    std::optional<std::string> pending = take_pending_selection(View::Runs);
    if (runs.empty()) {
        if (empty_state("No runs yet",
                        "A run records one execution of a deployed graph: "
                        "status, logs, outputs, and lineage.",
                        "Open deploy")) {
            navigate_to(View::Deploy);
        }
        return;
    }
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
        "runs", rows, selected_run_, std::move(pending), "(no runs)",
        "Select a run to view its status, logs, outputs, models, and lineage.",
        [&](const MasterRow& row) {
            const auto run = node_.find_run(row.id);
            if (run) draw_run_detail(*run);
        });
}

}  // namespace zetta::gui
