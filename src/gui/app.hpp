#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations keep SDL3 + Dear ImGui headers out of this file — only
// app.cpp (and no other translation unit) needs the full definitions.
struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;
struct ImGuiStyle;
struct ImFont;

namespace zetta {

class Node;
class Cluster;
struct ValidationReport;
struct CloudProvider;
class Project;
class ProjectGraph;
namespace ir {
class ExecutionGraph;
}
namespace query {
class QueryService;
}
namespace repl {
class ConsoleModel;
}
namespace testkit {
class TestOptions;
struct TestResult;
struct TestCase;
}  // namespace testkit

namespace gui {

class ReplConsole;  // the CMP-6 REPL component (src/gui/repl_console.hpp)

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
    // The single active view shown in the content area. The menu bar selects
    // which one is live, so the window shows one focused panel at a time instead
    // of every panel stacked in one long scroll. Ordered along the product flow:
    // your data → architect the pipeline → operate it → govern it.
    enum class View {
        Home,      // project-first landing (the default)
        Explorer,  // Projects | Databases | Tables | Rows | Large Objects
        Query,     // the SQL console
        Console,   // the SQL / simulated-Python REPL console (CMP-6)
        Graph,     // the execution-graph workspace
        Gate,      // the correctness gate
        Deploy,    // the AI Solution Library → one-button deploy
        Runs,      // runs / outputs / lineage
        Jobs,      // compute jobs + scaling preview
        Cluster,   // membership + replication
        Cloud,     // cloud provider inspector (permission-gated)
        Admin,     // principals / keys / audit
        Tests,     // the in-app test suite (zetta::testkit catalog)
    };

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
    // Build + present one ImGui frame. Factored out of run() so the SDL event
    // watch can re-render during a live window resize (macOS blocks the normal
    // loop inside its modal resize runloop, which otherwise stretches the last
    // frame). Does NOT tick the node — that stays in run().
    void render_frame();
    // SDL event watch: re-render on window resize/expose so the UI tracks the
    // drag. Registered in run(); `userdata` is the App.
    static bool on_sdl_event(void* userdata, SDL_Event* event);
    void draw_menu_bar();     // selects the view; Quit toggles running_
    void draw_active_view();  // render whichever View is selected (the menu switcher)
    void draw_about_modal();  // the Help → About Orama Zetta dialog
    void draw_home();         // project-first landing (hero + New project + projects)
    void draw_dashboard();
    void draw_explorer();    // Projects | Databases | Tables | Rows | Large Objects columns
    void draw_projects();    // top-level AI projects (create/select/rename/delete)
    void draw_databases();
    void draw_tables();
    void draw_rows();
    void draw_large_objects();
    void draw_query();       // the SQL text area + results
    void draw_console();     // the SQL / sim-Python REPL console (CMP-6 component)
    void draw_gate();        // correctness gate: run it over a graph, show the report
    void draw_deploy();      // AI Solution Library → gate → one-button deploy → status
    // Run the churn solution on the REAL engine (featurize → train → explain →
    // rank → playbook) over a built-in demo dataset, registering a real Model +
    // jobs on the node (#398). Synchronous on the UI thread (like run_cloud_inspect):
    // spins up an isolated single-node loopback JobCoordinator so the SPMD
    // collectives never block on this node's real peers, runs, then tears it down.
    // Called only from the Deploy panel's explicit "Run on real engine" action.
    void run_churn_demo();
    void draw_cluster();     // role, listen address, peers, join command
    void draw_cloud();       // cloud provider inspector + deny-by-default consent gate
    // Run a provider's read-only inspect (CloudInspector) and cache the result on
    // the node. Called ONLY from the consent gate's Allow / Re-inspect actions,
    // never on a passive render — this is the one place the GUI touches a cloud,
    // and only after explicit consent. Synchronous (briefly blocks the UI thread,
    // which owns the node, so no races); a deliberate per-click action.
    void run_cloud_inspect(const CloudProvider& provider);
    // Render the cached inspect result for `provider` (from the node). Read-only;
    // triggers no cloud call. Shown inside the granted consent gate.
    void draw_cloud_inspect_result(const CloudProvider& provider);
    void draw_runs();        // run status/logs + outputs + lineage + model/metrics/drivers
    void draw_jobs();        // compute jobs (system.zetta_jobs) + per-job objective/scaling
    void draw_admin();       // principals + keys (scope + grants) + the audit log
    void draw_test_suite();  // the in-app test suite: canned cases + options + run
    // Start the selected case on a worker thread (so distributed cases, which stand
    // up a real loopback cluster, don't block the UI). Snapshots the current options
    // + custom-data path; the worker writes the result + live log under test_mutex_.
    void start_test(const testkit::TestCase& test_case);

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
    // Geist Mono face for labels / counts / metrics / paths (the design system's
    // mono role). Non-owning — the ImGui font atlas owns it; re-captured whenever
    // the atlas is rebuilt in apply_display_scale().
    ImFont* mono_font_ = nullptr;
    bool initialized_ = false;
    bool show_demo_ = false;

    // Which panel the content area is showing (set from the menu bar). Defaults
    // to the project-first Home landing.
    View active_view_ = View::Home;
    // Asks draw_about_modal() to open the About popup on the next frame (set from
    // the Help menu), mirroring the New Project modal's request flag.
    bool about_requested_ = false;

    // Main-loop run flag (a member so render_frame() / the menu's Quit can set it
    // without threading a reference through every call).
    bool running_ = true;
    // Guards render_frame() against re-entry from the resize event watch.
    bool rendering_ = false;

    // User text-size multiplier (View → Text size), applied on top of the OS
    // content scale. Changing it requests a font-atlas rebuild between frames
    // (font size is baked at rasterization). applied_ui_scale_ tracks the value
    // the current atlas was built for.
    float ui_scale_ = 1.0f;
    float applied_ui_scale_ = 0.0f;
    bool font_rebuild_requested_ = false;

    // Explorer selection (held by name; re-resolved each frame so a dropped
    // database/table can't leave a dangling handle) and input fields. ImGui's
    // imgui_stdlib InputText overloads bind directly to std::string.
    std::string selected_project_;
    std::string selected_db_;
    std::string selected_table_;
    std::string selected_run_;  // run id; re-resolved each frame like the others
    std::string selected_job_;  // compute-job id; re-resolved each frame
    std::string selected_principal_;  // principal name; re-resolved each frame

    // Cloud Inspector (permission-gated). `selected_cloud_provider_` is the
    // catalog id selected in the list. `cloud_consent_provider_` is the id the
    // user has *explicitly* consented to inspect right now — deny-by-default and
    // reset whenever the selection changes, so consent is never carried over to a
    // different provider or remembered across selections (CLAUDE.md hard rule).
    // Only after consent does run_cloud_inspect() run the provider's read-only
    // commands; the result is cached on the node (system.zetta_cloud_status).
    std::string selected_cloud_provider_;
    std::string cloud_consent_provider_;

    // Test suite panel (zetta::testkit). `selected_test_` is the case id selected in
    // the catalog list; `test_options_` holds its editable option values (rebuilt
    // from the case's defaults when the selection changes — tracked by
    // `test_options_case_`); `test_custom_data_` is an optional dataset directory for
    // the data-driven cases. A case runs on `test_worker_` so the distributed cases
    // (which stand up a real loopback cluster, with blocking convergence waits) don't
    // freeze the UI; the worker writes `test_result_` + the live `test_log_` under
    // `test_mutex_`, and `test_running_` gates the Run button + list. `test_running_id_`
    // names the case in flight. Held by unique_ptr with the testkit types forward-
    // declared, to keep the testkit header out of app.hpp.
    std::string selected_test_;
    std::string test_options_case_;
    std::unique_ptr<testkit::TestOptions> test_options_;
    std::string test_custom_data_;
    std::unique_ptr<std::thread> test_worker_;
    std::atomic<bool> test_running_{false};
    std::mutex test_mutex_;
    std::unique_ptr<testkit::TestResult> test_result_;
    std::vector<std::string> test_log_;
    std::string test_running_id_;

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

    // Console panel (CMP-6 / #524): the reusable SQL/sim-Python REPL. The model
    // (zetta-core, no ImGui) holds the session state — selected frontend, input
    // buffer, scrollback transcript — so a future CLI can render the same model;
    // the component is its thin ImGui renderer. The run handler (built in
    // draw_console) maps SQL through query_ and surfaces simulated-Python as an
    // honest "not wired yet" seam until the frontend→IR layer lands. Held by
    // unique_ptr with both types forward-declared, to keep their headers out of
    // app.hpp (like query_).
    std::unique_ptr<repl::ConsoleModel> console_model_;
    std::unique_ptr<ReplConsole> console_;

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
    // "Run on real engine" (churn) result line + its colour. Set by run_churn_demo().
    std::string churn_run_status_;
    bool churn_run_failed_ = false;

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
