// SPDX-License-Identifier: MIT

// First-run onboarding (#623): the guided UC-1 churn demo grown out of Home's
// guided-start card. Five steps walk a fresh node to an explained churn result
// with no prior knowledge and no file dialogs — load the built-in demo dataset,
// create the churn project from a pre-filled intent, watch the correctness gate
// pass (and deploy), run the real pipeline over the live cluster, and land on
// the ranked at-risk accounts + drivers. Each step names the panel it just
// exercised (teaching the map while doing), every completed step keeps an
// entity_link into that panel, and Help · Guided demo replays any step later.
//
// Thin-renderer discipline: step progress is DERIVED from live engine state
// each frame (the demo tables, a project with a captured intent, a deployment,
// a succeeded run) — the only GUI-owned state is presentation (the active/
// dismissed flags and the finished run's outcome summary, an action result like
// the Deploy panel's status line). docs/gui/onboarding.md.

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"

#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/project_input.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

#include <imgui.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zetta::gui {
namespace {

// The demo's stable names: the loaded tables (step 1's done signal in
// system.datasets), the pre-filled project, and the compiled graph (whose
// deployment is step 3's done signal). Everything else is derived.
constexpr const char* kDemoAccountsTable = "churn_demo_accounts";
constexpr const char* kDemoEventsTable = "churn_demo_events";
constexpr const char* kDemoProjectName = "churn-demo";
constexpr const char* kDemoGraphName = "churn-demo";

// The pre-filled natural-language intent. It carries the churn signal words the
// intent compiler recognizes, so compiling it deterministically produces the
// churn pipeline skeleton (connect → clean → model → index → serve → act).
constexpr const char* kDemoIntent =
    "Reduce churn: score every active account for churn risk and explain why "
    "each one is at risk.";

// One guided step's header row: numbered, toned by where the user is —
// green/done · active/current · muted/still ahead. The label carries the state
// word so meaning never rides on colour alone.
void step_header(int number, const char* title, bool done, bool current) {
    const Tone tone = done ? Tone::Ok : current ? Tone::Active : Tone::Muted;
    char label[96];
    std::snprintf(label, sizeof(label), "%d · %s", number, title);
    status_label(tone, label);
}

// The "· see it in <panel>" teaching line a completed step leaves behind.
void panel_note_prefix() {
    ImGui::SameLine();
    ImGui::TextColored(kInkQuiet, "·");
    ImGui::SameLine();
}

}  // namespace

void App::draw_guided_demo_card() {
    // ── Derive every step's state from live sources (nothing stored) ─────
    bool accounts_loaded = false;
    bool events_loaded = false;
    for (const zetta::data::DatasetSummary& dataset : node_.datasets()) {
        if (dataset.name == kDemoAccountsTable) accounts_loaded = true;
        if (dataset.name == kDemoEventsTable) events_loaded = true;
    }
    const bool data_loaded = accounts_loaded && events_loaded;

    // The demo project: the first project carrying a captured intent (created
    // by step 2's pre-filled modal — the name is editable there, so detection
    // follows the intent, not the name).
    std::shared_ptr<zetta::pipeline::Project> project;
    std::string intent_input_id;
    for (const std::string& name : node_.project_names()) {
        const auto candidate = node_.find_project(name);
        if (!candidate) continue;
        for (const auto& input : candidate->inputs()) {
            if (input &&
                input->kind() == zetta::pipeline::ProjectInputKind::Intent) {
                project = candidate;
                intent_input_id = input->id();
                break;
            }
        }
        if (project) break;
    }

    // The compiled churn graph inside that project, and its deployment.
    std::shared_ptr<zetta::pipeline::ProjectGraph> graph;
    if (project) {
        for (const auto& stored : project->graphs()) {
            if (stored && zetta::usecases::churn::is_churn_graph(stored->graph())) {
                graph = stored;
                break;
            }
        }
    }
    bool deployed = false;
    if (graph) {
        for (const auto& deployment : node_.deployments()) {
            if (deployment && deployment->graph_name() == graph->name()) {
                deployed = true;
                break;
            }
        }
    }

    // The demo run: the newest run labelled with the project (runs() is
    // created-ascending, so the last match wins).
    std::shared_ptr<zetta::pipeline::Run> demo_run;
    if (project) {
        for (const auto& run : node_.runs()) {
            if (run && run->project() == project->id()) demo_run = run;
        }
    }
    const bool run_in_flight = churn_run_ != nullptr && churn_run_from_onboarding_;
    const bool run_succeeded =
        demo_run && demo_run->status() == zetta::pipeline::RunStatus::Succeeded;

    const bool step_done[5] = {data_loaded, project != nullptr, deployed,
                               run_succeeded, false};
    int current = 0;
    while (current < 5 && step_done[current]) ++current;

    section_card("##home_guided_demo", "Guided demo · churn in five minutes", [&] {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(
            kInkMuted,
            "From an empty node to an explained churn model: five steps, no "
            "files to find. Each step names the panel it uses, so the map "
            "sticks while you go.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        banner(onboarding_failed_ ? Tone::Error : Tone::Ok, onboarding_status_);

        // ── 1 · Load the demo dataset ────────────────────────────────────
        step_header(1, "Load the demo dataset", step_done[0], current == 0);
        if (step_done[0]) {
            panel_note_prefix();
            entity_link("see it in Datasets", View::Datasets, {});
        } else if (current == 0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kInkMuted,
                               "A built-in synthetic churn dataset (accounts + "
                               "events) loads as partitioned tables — the same "
                               "path the Datasets panel uses for your own files.");
            ImGui::PopTextWrapPos();
            if (primary_button("Load demo data")) {
                onboarding_active_ = true;
                onboarding_failed_ = false;
                onboarding_status_.clear();
                const zetta::usecases::churn::ChurnDataset demo =
                    zetta::usecases::churn::demo_churn_dataset();
                const std::filesystem::path demo_dir =
                    std::filesystem::path(node_.config().data_dir) / "demo" /
                    "churn";
                std::string error =
                    zetta::usecases::churn::write_churn_dataset(demo, demo_dir);
                std::size_t rows = 0;
                if (error.empty()) {
                    zetta::data::AddDatasetOptions options;
                    options.members = node_.cluster_members();
                    options.table_name = kDemoAccountsTable;
                    const zetta::data::AddDatasetResult accounts =
                        zetta::data::add_dataset_file(
                            node_, demo_dir / "accounts.jsonl", options);
                    if (!accounts.ok()) {
                        error = accounts.error;
                    } else {
                        rows += accounts.rows;
                        options.table_name = kDemoEventsTable;
                        const zetta::data::AddDatasetResult events =
                            zetta::data::add_dataset_file(
                                node_, demo_dir / "events.jsonl", options);
                        if (!events.ok()) error = events.error;
                        else rows += events.rows;
                    }
                }
                if (!error.empty()) {
                    onboarding_failed_ = true;
                    onboarding_status_ = "Load demo data failed: " + error;
                } else {
                    onboarding_status_ =
                        "Loaded " + std::to_string(demo.accounts.size()) +
                        " accounts and " + std::to_string(demo.events.size()) +
                        " events (" + std::to_string(rows) +
                        " rows, partitioned): the Datasets panel shows their "
                        "placement.";
                }
            }
        }
        ImGui::Spacing();

        // ── 2 · Create the churn project ─────────────────────────────────
        step_header(2, "Create the churn project", step_done[1], current == 1);
        if (step_done[1]) {
            panel_note_prefix();
            entity_link("open it in the Pipeline workspace", View::Graph, {});
        } else if (current == 1) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kInkMuted,
                               "A project starts from an intent in plain "
                               "language. This one is pre-filled — the New "
                               "project screen opens, you just click Create.");
            ImGui::PopTextWrapPos();
            if (primary_button("New project…")) {
                onboarding_active_ = true;
                reset_new_project_form();
                new_project_name_ = kDemoProjectName;
                new_project_intent_ = kDemoIntent;
                new_project_modal_requested_ = true;
            }
        }
        ImGui::Spacing();

        // ── 3 · Watch the gate pass ──────────────────────────────────────
        step_header(3, "Watch the gate pass", step_done[2], current == 2);
        if (step_done[2]) {
            panel_note_prefix();
            entity_link("the report is in Gate", View::Gate, {});
            panel_note_prefix();
            entity_link("the placement is in Deploy", View::Deploy, {});
        } else if (current == 2) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(
                kInkMuted,
                "The intent compiles into a Pipeline, and "
                "Verification checks it before anything deploys: no deploy "
                "without a passed Verification.");
            ImGui::PopTextWrapPos();
            if (primary_button("Compile, verify, and deploy")) {
                onboarding_active_ = true;
                onboarding_failed_ = false;
                onboarding_status_.clear();
                std::shared_ptr<zetta::pipeline::ProjectGraph> compiled = graph;
                if (!compiled && project) {
                    compiled = project->compile_intent_to_graph(intent_input_id,
                                                                kDemoGraphName);
                }
                if (!compiled) {
                    onboarding_failed_ = true;
                    onboarding_status_ =
                        "Compile failed: the intent could not be compiled into "
                        "a stored graph.";
                } else {
                    // Prefill the Gate panel with this graph + its report, so
                    // "see the full report" lands on the run it just watched.
                    gate_graph_input_ = ir::to_json(compiled->graph()).dump(2);
                    gate_error_.clear();
                    const zetta::pipeline::ValidationReport report =
                        zetta::pipeline::default_gate().run(compiled->graph());
                    gate_report_ =
                        std::make_unique<zetta::pipeline::ValidationReport>(
                            report);
                    if (report.block_deploy) {
                        std::string reason;
                        for (const zetta::pipeline::CheckResult& check :
                             report.checks) {
                            if (check.status ==
                                zetta::pipeline::CheckStatus::Fail) {
                                reason = check.name + ": " + check.reason;
                                break;
                            }
                        }
                        onboarding_failed_ = true;
                        onboarding_status_ =
                            "Gate blocked deploy: " +
                            (reason.empty() ? "graph failed validation"
                                            : reason);
                    } else {
                        const zetta::pipeline::DeploymentTarget target{
                            zetta::pipeline::DeploymentKind::Local, {}, {}};
                        const auto deployment = node_.deploy_graph(
                            compiled->graph(), target, compiled->name());
                        if (deployment) {
                            onboarding_status_ =
                                "Gate passed · deployed to local (deployment " +
                                deployment->id().substr(0, 8) +
                                "): the Gate panel has the full report.";
                        } else {
                            onboarding_failed_ = true;
                            onboarding_status_ =
                                "Deploy failed: the gate passed but placement "
                                "was rejected.";
                        }
                    }
                }
            }
        }
        ImGui::Spacing();

        // ── 4 · Run over the cluster ─────────────────────────────────────
        step_header(4, "Run over the cluster", step_done[3],
                    current == 3 || run_in_flight);
        if (run_in_flight) {
            // Live progress from the run's own records (#624) — the same block
            // the Deploy panel renders, plus the cluster-animation pointer.
            draw_churn_run_progress();
            panel_note_prefix();
            entity_link("watch the cluster animation", View::Cluster, {});
        } else if (step_done[3]) {
            panel_note_prefix();
            entity_link("the run is in Runs", View::Runs,
                        demo_run ? demo_run->id() : std::string());
            panel_note_prefix();
            entity_link("its jobs are in Compute jobs", View::Jobs, {});
        } else if (current == 3) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(
                kInkMuted,
                "The real pipeline (featurize · train · explain · rank) runs "
                "on the live cluster — watch the Cluster panel animate the "
                "work while it executes.");
            ImGui::PopTextWrapPos();
            ImGui::BeginDisabled(churn_run_ != nullptr);
            if (primary_button("Run over the cluster")) {
                onboarding_active_ = true;
                onboarding_failed_ = false;
                onboarding_status_.clear();
                zetta::usecases::churn::ChurnSolutionOptions options;
                options.project = project ? project->id() : std::string();
                options.graph = graph ? graph->id() : std::string();
                churn_run_from_deploy_ = false;
                churn_run_from_onboarding_ = true;
                churn_run_ = zetta::usecases::churn::AsyncChurnRun::start(
                    node_, graph->graph(),
                    zetta::usecases::churn::demo_churn_dataset(), options);
            }
            ImGui::EndDisabled();
            if (churn_run_ != nullptr && !run_in_flight) {
                ImGui::SameLine();
                ImGui::TextColored(kInkQuiet,
                                   "another churn run is already in flight");
            }
        }
        ImGui::Spacing();

        // ── 5 · See who is at risk and why ───────────────────────────────
        step_header(5, "See who is at risk and why", run_succeeded && current >= 4,
                    current == 4);
        if (step_done[3]) {
            // The finished run's outcome summary (an action result, like the
            // Deploy panel's status line) — the ranked at-risk accounts with
            // each one's dominant churn driver.
            if (!onboarding_at_risk_.empty()) {
                ImGui::Spacing();
                const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                                    ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("onboarding_at_risk", 3, table_flags)) {
                    ImGui::TableSetupColumn("At-risk account",
                                            ImGuiTableColumnFlags_WidthStretch,
                                            0.45f);
                    ImGui::TableSetupColumn(
                        "Churn risk", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                    ImGui::TableSetupColumn("Dominant driver",
                                            ImGuiTableColumnFlags_WidthStretch,
                                            0.35f);
                    ImGui::TableHeadersRow();
                    if (mono_font_) ImGui::PushFont(mono_font_);
                    for (const OnboardingAtRisk& entry : onboarding_at_risk_) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(entry.account.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%.0f%%", entry.risk * 100.0);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(entry.driver.c_str());
                    }
                    if (mono_font_) ImGui::PopFont();
                    ImGui::EndTable();
                }
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kInkMuted,
                               "The Runs panel holds the full picture: the "
                               "trained model, its metrics, and the ranked "
                               "drivers behind every score.");
            ImGui::PopTextWrapPos();
            if (primary_button("Open the run")) {
                navigate_to(View::Runs, !onboarding_run_id_.empty()
                                            ? onboarding_run_id_
                                            : (demo_run ? demo_run->id()
                                                        : std::string()));
            }
        } else if (current == 4) {
            ImGui::TextColored(kInkMuted,
                               "The ranked at-risk list and its drivers land "
                               "here when the run finishes.");
        }
        ImGui::Spacing();

        // ── Dismiss: session-only; a node with any project never auto-shows
        //    the card again (the issue's "never again" rule falls out of live
        //    state). Help · Guided demo brings it back for a replay.
        if (ImGui::SmallButton("Dismiss")) {
            onboarding_active_ = false;
            onboarding_dismissed_ = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(kInkQuiet,
                           "replay any step later from Help · Guided demo");
    });
}

}  // namespace zetta::gui
