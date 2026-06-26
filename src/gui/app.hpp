#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations keep SDL3 + Dear ImGui headers out of this file — only
// app.cpp (and no other translation unit) needs the full definitions.
struct SDL_Window;
struct SDL_Renderer;
struct ImGuiStyle;

namespace zetta {

class Node;
class Cluster;
struct ValidationReport;
class Project;
class ProjectGraph;
namespace ir {
class ExecutionGraph;
}
namespace query {
class QueryService;
}

namespace gui {

// The headed run mode: an SDL3 + Dear ImGui window rendering a live view of a
// zetta::Node. Owns the window, renderer, and ImGui context for its lifetime —
// construct it, init(), run(), then let it destruct.
class App {
public:
    App(Node& node, Cluster& cluster);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Initialize SDL + ImGui and open the window. Returns false on failure
    // (a diagnostic is written to stderr first).
    bool init();

    // Run the event/render loop until the user quits. Ticks the node once per
    // frame. Must be called only after a successful init().
    void run();

private:
    // A file to ingest, queued from a drop event or the native file dialog
    // (whose callback may fire on another thread — hence the mutex). Auto
    // routes by extension: .json/.jsonl import as rows, anything else stores as
    // a large object. N8nArtifact stages a workflow JSON for the New Project
    // dialog (validated on stage, imported into the new project only on Create)
    // instead of ingesting it immediately.
    enum class FileIntent { Auto, Records, LargeObject, N8nArtifact };
    struct IncomingFile {
        std::string path;
        FileIntent intent;
    };

    // Re-rasterize the font and rescale the style for the current display.
    // See app.cpp for the content-scale vs pixel-density rationale.
    void apply_display_scale();
    void draw_menu_bar(bool& running);
    void draw_dashboard();
    void draw_explorer();    // Projects | Databases | Tables | Rows | Large Objects columns
    void draw_projects();    // top-level AI projects (create/select/rename/delete)
    void draw_databases();
    void draw_tables();
    void draw_rows();
    void draw_large_objects();
    void draw_query();       // the SQL text area + results
    void draw_gate();        // correctness gate: run it over a graph, show the report
    void draw_deploy();      // AI Solution Library → gate → one-button deploy → status
    void draw_cluster();     // role, listen address, peers, join command
    void draw_runs();        // run status/logs + outputs + lineage + model/metrics/drivers
    void draw_jobs();        // compute jobs (system.zetta_jobs) + per-job objective/scaling
    void draw_admin();       // principals + keys (scope + grants) + the audit log

    // Per-job objective/scaling controls inside the Jobs panel (FEAT-5 / SCR):
    // a live preview that scores a ScalingPolicy against an editable sample via
    // the built ScalingPolicy::evaluate() — set the reward objective + bounds and
    // add/remove nodes, see the resulting ScalingDecision. A preview only: policy
    // persistence + the live autoscaler are later slices, so nothing is written.
    void draw_job_scaling_controls();
    // Reset the scaling-control fields to sensible defaults (called when the
    // selected job changes, tracked via scaling_loaded_job_).
    void reset_scaling_controls();

    // New Project flow (SCR-2 / FEAT-1): a modal to name a project, paste an NL
    // intent, and import / drop an n8n workflow JSON as a starting artifact —
    // wiring the GUI to Node::create_project + Project::capture_intent +
    // import_n8n_workflow_file. Drawn each frame inside the root window.
    void draw_new_project_modal();
    // Stage a dropped/picked n8n workflow file for the New Project dialog:
    // validate it against the n8n schema and record the result, without storing
    // anything yet (the import happens on Create). Runs on the main thread.
    void stage_n8n_artifact(const std::string& path);
    // Clear the New Project form (name, intent, staged artifact, error).
    void reset_new_project_form();

    // Graph Workspace (SCR-3 / CMP-1): view + edit the selected project's
    // ExecutionGraph IR as a node/edge canvas; edits persist back to the IR.
    void draw_graph_workspace();
    // Render the working-copy graph as a canvas: nodes laid out in columns by
    // StepKind (connect→…→act), edges drawn between them; clicking selects a node.
    void draw_graph_canvas();
    // The add/rename/delete controls + Save/Revert below the canvas.
    void draw_graph_editor(Project& project);
    // Load a fresh working copy of `stored` for editing (clears selection/dirty).
    void open_graph(const ProjectGraph& stored);

    void handle_incoming_files();  // drain + route the queue (main thread)
    void enqueue_file(std::string path, FileIntent intent);
    static void on_open_records(void* userdata, const char* const* files, int filter);
    static void on_open_large_object(void* userdata, const char* const* files, int filter);
    static void on_open_n8n(void* userdata, const char* const* files, int filter);

    Node& node_;
    Cluster& cluster_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool initialized_ = false;
    bool show_demo_ = false;

    // Explorer selection (held by name; re-resolved each frame so a dropped
    // database/table can't leave a dangling handle) and input fields. ImGui's
    // imgui_stdlib InputText overloads bind directly to std::string.
    std::string selected_project_;
    std::string selected_db_;
    std::string selected_table_;
    std::string selected_run_;  // run id; re-resolved each frame like the others
    std::string selected_job_;  // compute-job id; re-resolved each frame
    std::string selected_principal_;  // principal name; re-resolved each frame
    std::string new_db_input_;
    std::string new_table_input_;
    std::string rename_project_input_;
    std::string rename_db_input_;
    std::string doc_input_{R"({"name": "example", "value": 1})"};
    // Per-column inline error messages.
    std::string project_error_;
    std::string db_error_;
    std::string table_error_;
    std::string row_error_;

    // New Project modal (SCR-2 / FEAT-1): the full "name + paste intent + import
    // n8n artifact" front door, opened from the Projects column or File menu.
    // `new_project_modal_requested_` asks to open it on the next frame;
    // `new_project_modal_open_` mirrors whether it is currently visible and
    // drives file-drop routing in run() (a drop while it's open stages the n8n
    // artifact instead of importing rows into the selected table). The artifact
    // is *staged* (path + validation result) and imported into the project only
    // on Create, so the project is built atomically and an invalid workflow
    // never leaves a half-built project behind.
    bool new_project_modal_requested_ = false;
    bool new_project_modal_open_ = false;
    std::string new_project_name_;
    std::string new_project_intent_;
    std::string new_project_artifact_path_;    // staged n8n file ("" = none)
    std::string new_project_artifact_name_;    // its display filename
    std::string new_project_artifact_status_;  // staging/validation result line
    bool new_project_artifact_valid_ = false;  // staged file passed n8n validation
    std::string new_project_error_;            // inline create error

    // Query panel: the SQL being composed, the dialect runner, and the last
    // result (rows pre-dumped to strings for display, plus status/error).
    std::unique_ptr<query::QueryService> query_;
    std::string query_input_{
        "-- Postgres-flavored SQL. Examples:\n"
        "CREATE DATABASE shop;\n"
        "CREATE TABLE shop.items (sku text, qty int);\n"
        "INSERT INTO shop.items (sku, qty) VALUES ('A1', 5), ('B2', 3);\n"
        "SELECT * FROM shop.items WHERE sku = 'A1';"};
    std::vector<std::string> query_result_columns_;
    std::vector<std::string> query_result_rows_;
    std::string query_status_;
    std::string query_error_;
    std::string query_time_;  // formatted elapsed time of the last run

    // Correctness-gate panel (SCR-4 / CMP-2): the execution-graph IR in its
    // canonical JSON form to validate, an inline parse/validation error (a
    // reason, never a stack trace), and the last ValidationReport — null until
    // the gate is run. The graph JSON enters through ir::from_json, which
    // validates it against the IR schema at the boundary.
    std::string gate_graph_input_;
    std::string gate_error_;
    std::unique_ptr<ValidationReport> gate_report_;

    // Deploy panel: the selected AI Solution Library blueprint (by id), the
    // chosen placement on the dial (R1: only Local is functional), and the last
    // deploy action's result line.
    std::string selected_solution_;  // catalog solution id, empty = none
    int deploy_placement_ = 0;       // DeploymentKind index; 0 = Local
    std::string deploy_status_;      // last deploy result (success or error)
    bool deploy_failed_ = false;     // colour deploy_status_ red vs. green

    // Jobs panel — per-job objective/scaling controls (a live preview over the
    // built ScalingPolicy::evaluate(); see draw_job_scaling_controls). These hold
    // the in-flight policy + sample for the selected job; they are reset when the
    // selection changes (scaling_loaded_job_ tracks which job they belong to).
    // Nothing here is persisted — policy storage + the autoscaler are later
    // slices — so the controls are a "what-if" calculator, not a write path.
    std::string scaling_loaded_job_;     // job id the controls below belong to
    int scaling_objective_ = 0;          // ScalingObjective index (0 = minimize_cost)
    int scaling_min_nodes_ = 1;          // bounds floor (>= 1)
    int scaling_max_nodes_ = 8;          // bounds ceiling (>= min)
    double scaling_cost_cap_ = 0.0;      // spend ceiling; 0 = unlimited
    double scaling_target_seconds_ = 600.0;  // RuntimeTarget objective target
    int scaling_nodes_ = 2;              // sample: nodes currently allocated
    double scaling_progress_ = 0.4;      // sample: fraction complete [0, 1]
    double scaling_elapsed_ = 120.0;     // sample: elapsed seconds
    double scaling_cost_ = 50.0;         // sample: accumulated cost

    // File ingestion (drag-and-drop + native dialog) and its last status line.
    std::mutex incoming_mutex_;
    std::vector<IncomingFile> incoming_files_;
    std::string io_status_;

    // Graph Workspace state. The panel edits a *working copy* of the open graph
    // (editing_graph_) and only writes it back to the IR on Save, so an edit is
    // a deliberate, atomic act. The copy is reloaded whenever a different graph
    // is opened (graph_loaded_id_ tracks which one it holds) or the selected
    // project changes (graph_panel_project_). Held by unique_ptr with the type
    // forward-declared, like base_style_, to keep the IR header out of app.hpp.
    std::string graph_panel_project_;  // project the open graph belongs to
    std::string selected_graph_;       // saved name of the open graph ("" = none)
    std::string graph_loaded_id_;      // id the working copy was loaded from
    std::unique_ptr<ir::ExecutionGraph> editing_graph_;  // the working copy
    bool graph_dirty_ = false;         // working copy has structural edits to save
    std::string selected_node_id_;     // node selected in the canvas ("" = none)
    std::string graph_error_;          // inline error for graph/list operations
    std::string new_graph_input_;      // name field for "New graph"
    std::string rename_graph_input_;   // name field for the open graph (rename)
    std::string add_node_name_input_;  // name field for a new node
    int add_node_kind_ = 0;            // StepKind index for a new node
    std::string add_edge_from_;        // from-node id for a new edge
    std::string add_edge_to_;          // to-node id for a new edge
    int add_edge_kind_ = 0;            // EdgeKind index for a new edge
    std::string rename_node_input_;    // name field for renaming the selected node

    // Display-scale tracking — the last values we rasterized/scaled for, so a
    // no-change display event is a cheap no-op.
    float applied_content_scale_ = 0.0f;
    float applied_pixel_density_ = 0.0f;

    // Snapshot of the default ImGui style, taken once at init and re-applied
    // (then scaled) on every display-scale change. Held by pointer so this
    // header needn't include <imgui.h> for the full ImGuiStyle definition.
    std::unique_ptr<ImGuiStyle> base_style_;
};

}  // namespace gui
}  // namespace zetta
