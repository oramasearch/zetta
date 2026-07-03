#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gui/cluster_telemetry.hpp"  // per-node gauge animation state (pure data)
#include "gui/cluster_pipeline.hpp"   // distributed-algorithm overlay state (pure data)

// Forward declarations keep SDL3 + Dear ImGui headers out of this file — only
// app.cpp (and no other translation unit) needs the full definitions.
struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;
struct ImGuiStyle;
struct ImFont;

namespace zetta { class Node; }
namespace zetta::cluster { class Cluster; }
namespace zetta::cluster { struct NodeStatus; }
namespace zetta::data { struct DatasetSummary; }
namespace zetta::pipeline { struct ValidationReport; }
namespace zetta::pipeline { class Project; }
namespace zetta::pipeline { class ProjectGraph; }
namespace zetta::pipeline { struct ClusterPlan; }
namespace zetta::pipeline { class Run; }
namespace zetta::pipeline { struct RunProgress; }
namespace zetta::index { struct SearchResult; }
namespace zetta::ir { class ExecutionGraph; }
namespace zetta::observability { class SpanRecorder; }
namespace zetta::query { class QueryService; }
namespace zetta::repl { class ConsoleModel; }
namespace zetta::repl { class GovernedConsole; }
namespace zetta::testkit { class TestOptions; }
namespace zetta::testkit { struct TestResult; }
namespace zetta::testkit { struct TestCase; }
namespace zetta::gui { struct AddNodesOp; }
namespace zetta::gui { struct VmSizesOp; }
namespace zetta::api { class Registry; }

namespace zetta::cloud { class Provisioner; }
namespace zetta::cloud { struct CloudProvider; }

namespace zetta {






namespace gui {

class ReplConsole;  // the CMP-6 REPL component (src/gui/repl_console.hpp)

// In-flight async "Add nodes" op (defined in app_operations.cpp). A
// runtime::spawn'd coroutine runs the provider CLI OFF the UI thread and writes
// its result into this shared state; the UI thread polls it each frame, applies
// the result to the node (apply_added_nodes) on its own thread, and renders a
// progress bar. Held by shared_ptr so the coroutine + its completion keep it alive.

// In-flight async fetch of the VM-size dropdown options for a region (`az vm
// list-skus` is region-dependent + slow, so it runs off the UI thread via
// runtime::spawn, like AddNodesOp). Defined in app_operations.cpp.

// The headed run mode: an SDL3 + Dear ImGui window rendering a live view of a
// zetta::Node. Owns the window, renderer, and ImGui context for its lifetime —
// construct it, init(), run(), then let it destruct.
class App {
public:
    App(Node& node, zetta::cluster::Cluster& cluster);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Initialize SDL + ImGui and open the window. Returns false on failure
    // (a diagnostic is written to stderr first).
    bool init();

    // Run the event/render loop until the user quits. Ticks the node once per
    // frame. Must be called only after a successful init().
    void run();

    // Inject the Provisioner the Cluster view's "Add nodes" control drives for a
    // target (`target_id` is a CloudProvider catalog id — "local", "aws", …). main
    // wires the given-hardware "local" backend (a LocalProvisioner over the live
    // cluster fleet); a configured deployment can inject cloud provisioners for
    // their targets, which stay consent-gated in the control. `provisioner` must
    // outlive the App. Targets with no injected provisioner render but can't act.
    // Runs each frame right after Node::tick() — the owning-loop hook the
    // embedded API server's bridge drains through (docs/api/registry-design.md).
    void set_tick_hook(std::function<void()> hook) { tick_hook_ = std::move(hook); }

    // The node's span ring (the API server's recorder today) — lets GUI-driven
    // searches record their trace into the same ring MCP calls do, so
    // list_traces / the trace pane show one history. Null (the default) = GUI
    // searches record no spans. Non-owning; must outlive the App.
    void set_span_recorder(zetta::observability::SpanRecorder* recorder) {
        span_recorder_ = recorder;
    }

    void set_provisioner(const std::string& target_id, zetta::cloud::Provisioner& provisioner);

    // The capability registry the command palette (#625) renders and executes —
    // the SAME list the MCP server serves (docs/api/registry-design.md: one
    // registry, many renderings). main wires the API server's registry; when
    // none is wired (--no-mcp) the palette builds its own from
    // register_core_capabilities, so the human surface never loses actions.
    // Non-owning; must outlive the App.
    void set_capability_registry(zetta::api::Registry* registry) {
        capability_registry_ = registry;
    }

private:
    // The single active view shown in the content area. The menu bar selects
    // which one is live, so the window shows one focused panel at a time instead
    // of every panel stacked in one long scroll. Ordered along the product flow:
    // your data → architect the pipeline → operate it → govern it.
    enum class View {
        Home,      // project-first landing (the default)
        Explorer,  // Projects | Databases | Tables | Rows | Large Objects
        Datasets,  // datasets (system.datasets): schema/size/format + placement
        Search,    // fulltext search over registered indexes (Node::search, #641)
        Query,     // the SQL console
        Console,   // the SQL / simulated-Python REPL console (CMP-6)
        Graph,     // the execution-graph workspace
        Gate,      // the correctness gate
        Deploy,    // the AI Solution Library → one-button deploy
        Activity,  // the chronological activity feed (#622): audit + runs merged
        Traces,    // the trace pane (#640): every operation's spans, from the ring
        Runs,      // runs / outputs / lineage
        Models,    // trained models (system.models) + register/import
        Jobs,      // compute jobs + scaling preview
        Cluster,   // membership + replication
        Cloud,     // cloud provider inspector (permission-gated)
        Admin,     // principals / keys / audit
        Tests,     // the in-app test suite (zetta::testkit catalog)
    };

    // One row of the view registry (docs/gui/architecture.md): each view is
    // declared exactly once here and consumed by the sidebar, navigation, and
    // the command palette (#625). Adding a view = adding one row + its draw
    // method — nothing else knows the view list.
    struct ViewInfo {
        View id;
        const char* section;  // sidebar group: "" (Home) · "Data" · "Pipeline"
                              // · "Operations" · "Governance"
        const char* name;     // sentence case, product language
        void (App::*draw)();  // the panel body
        // Optional cheap live count rendered as the row's badge — reads the
        // same live sources as the panel each frame, stores nothing.
        // nullptr = no badge; an empty string renders none this frame.
        std::string (App::*badge)() const;
    };
    // The registry — a static table in app_views.cpp, in sidebar order.
    static std::span<const ViewInfo> view_registry();

    // The ONLY way any code changes the active view. When an entity id is
    // given it is stored as that view's pending selection; the panel consumes
    // it on its next draw (take_pending_selection) and selects the entity —
    // the whole foundation cross-entity links (#633) and external deep links
    // (the API activity hook, #620) need.
    void navigate_to(View view, const std::string& entity_id = {});
    std::optional<std::string> take_pending_selection(View view);
    // Render `label` as an inline link that navigates to `view` selecting
    // `entity_id` — the grammar's cross-entity reference (#633).
    void entity_link(const char* label, View view, const std::string& entity_id);
    // The sectioned left navigation rail (#628), grammar-rendered from the
    // registry; coral marks the active row (the design system's "you are here").
    void draw_sidebar();
    // The Cmd+K command palette (#625, app_palette.cpp): fuzzy view
    // navigation + entity jump + the capability registry's actions rendered
    // for humans. Issued every frame from render_frame (a popup, like the
    // New Project modal).
    void draw_command_palette();
    // The palette's list step: query input + the fuzzy-ranked entries (views,
    // entities, registry actions), grouped under mono eyebrows.
    void draw_palette_list_step();
    // The palette's argument step: a schema-driven form for the selected
    // capability's input (one field per property; the schema is the form).
    void draw_palette_argument_step();
    // Execute a registry capability from the palette: assemble the staged
    // argument fields per the entry's input schema, call Registry::call on
    // this (owning) thread — schema validation, authorization, audit, spans,
    // and the consent gate all live in that one path — and record the outcome
    // for the palette to render (the engine's reason verbatim on failure).
    void run_palette_capability(const std::string& capability_name);
    // Sidebar badges: cheap per-frame reads of live engine state.
    std::string jobs_badge() const;     // running compute jobs ("" when none)
    std::string cluster_badge() const;  // cluster member count

    // A file to ingest, queued from a drop event or the native file dialog
    // (whose callback may fire on another thread — hence the mutex). Auto
    // routes by extension: .json/.jsonl import as rows, anything else stores as
    // a large object. Dataset loads a parquet/jsonl/json/csv file as a *partitioned*
    // table via add_dataset_file (#587), so it surfaces in the Datasets view with
    // its partitions placed across the cluster. N8nArtifact stages a workflow JSON
    // for the New Project dialog (validated on stage, imported into the new project
    // only on Create) instead of ingesting it immediately. ModelWeights stages a
    // chosen file as the serialized weights for the Models view's "Add model" form
    // (attached via Model::set_weights on Register), rather than ingesting it.
    enum class FileIntent {
        Auto,
        Records,
        LargeObject,
        Dataset,
        N8nArtifact,
        ModelWeights
    };
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
    void draw_datasets();    // datasets (system.datasets): list + schema/size/format
    // The partition-placement-across-the-cluster overlay inside draw_datasets:
    // one node box per cluster member (the draw_cluster grammar), highlighting
    // which nodes own a partition of the selected dataset.
    void draw_dataset_placement(const zetta::data::DatasetSummary& dataset);
    void draw_query();       // the SQL text area + results
    void draw_search();      // fulltext search over registered indexes (#641/#643)
    // The Search view's cluster strip: node boxes for the last search's
    // answering nodes with the scatter-gather replayed as #590 flows (#643).
    void draw_search_cluster_strip(const zetta::index::SearchResult& result);
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
    void draw_cluster();     // spatial selectable node-box canvas + per-node detail
    // The detail pane below the cluster canvas: full id, role, address,
    // membership, replication status, and (for self) reachable addresses + peer
    // count for the selected node. `nodes` is the live Cluster::nodes() snapshot.
    void draw_cluster_node_detail(const std::vector<zetta::cluster::NodeStatus>& nodes);
    // The live pipeline-activity strip below the cluster canvas (#590, epic #575
    // area 5): the real instrumentation the distributed-algorithm overlay animates
    // — collective rounds + wire/near-data byte totals + placement/rebalance tallies
    // — echoed verbatim, with a legend for the flow colours. `pipeline` is the
    // snapshot draw_cluster already read this frame (a thin renderer; no new reads).
    void draw_pipeline_activity(const PipelineInputs& pipeline);
    // The "Add nodes" control in the Cluster view (epic #575 area 1): pick a
    // target (the provider catalog), a count, and drive zetta::cloud::add_nodes over the
    // injected Provisioner for that target. The given-hardware Local target runs
    // immediately (it brings up no new machines, touches no cloud); every cloud /
    // Kubernetes / on-prem target is **permission-gated, deny-by-default** — adding
    // nodes there runs the provider's CLI to create billable resources, so nothing
    // happens until the user explicitly allows that one action (CLAUDE.md hard
    // rule). A target with no injected Provisioner renders but reports it isn't
    // wired.
    void draw_add_nodes_control();
    // Launch an "Add nodes" op over the injected Provisioner for `target` — reached
    // ONLY from the Local "Add" button or a cloud target's explicit "Allow" (the
    // consent action), never on a passive render. The slow provision (the provider
    // CLI) runs OFF the UI thread as a runtime::spawn'd coroutine, so a minutes-long
    // `az vmss create` no longer freezes the window; poll_add_nodes() applies the
    // result on the UI thread when it finishes. A no-op while an op is already in
    // flight (the button is disabled meanwhile).
    void run_add_nodes(const zetta::cloud::CloudProvider& target);
    // Poll the in-flight async add-nodes op each frame: when the coroutine has
    // finished, apply its result to the node (apply_added_nodes, on the UI thread),
    // record the run's duration as the ETA baseline, set the status line, and clear
    // the op. Cheap no-op when nothing is in flight. Called from render_frame so it
    // completes regardless of which view is showing.
    void poll_add_nodes();
    // The Provisioner injected for `target_id` (set_provisioner), or nullptr when
    // none is wired for that target.
    zetta::cloud::Provisioner* provisioner_for(const std::string& target_id) const;
    void draw_cloud();       // cloud provider inspector + deny-by-default consent gate
    // Run a provider's read-only inspect (CloudInspector) and cache the result on
    // the node. Called ONLY from the consent gate's Allow / Re-inspect actions,
    // never on a passive render — this is the one place the GUI touches a cloud,
    // and only after explicit consent. Synchronous (briefly blocks the UI thread,
    // which owns the node, so no races); a deliberate per-click action.
    void run_cloud_inspect(const zetta::cloud::CloudProvider& provider);
    // Render the cached inspect result for `provider` (from the node). Read-only;
    // triggers no cloud call. Shown inside the granted consent gate.
    void draw_cloud_inspect_result(const zetta::cloud::CloudProvider& provider);
    // The Activity view (#622, docs/gui/activity-feed.md): ONE chronological
    // feed of everything the cluster was asked to do, rendered each frame from
    // zetta::observability::activity_entries (the audit trail + runs merged —
    // the same source system.activity serves; no GUI copy). Filters below are
    // presentation state only.
    void draw_activity();
    // The Traces view (#640, app_traces.cpp): the trace pane over the node's
    // span ring (the same source system.traces / system.spans / get_trace
    // read — RULE-7) — recent traces (intent · duration · status) with a
    // per-span drill-down (plan, result shape, error verbatim).
    void draw_traces();
    void draw_runs();        // run status/logs + outputs + lineage + model/metrics/drivers
    void draw_models();      // standalone models (system.models): list + detail + add
    // The inline "Add model" affordance inside draw_models: register/import a model
    // via Node::create_model (+ optional Model::set_weights). The assembled value is
    // re-validated against the Model JSON Schema at this boundary, so malformed
    // metrics/feature_schema fail fast here instead of corrupting the tree on the
    // next reconcile. Drawn collapsed by default so the list stays the focus.
    void draw_add_model_form();
    // Clear the Add-model form fields (name/kind/metrics/features/staged weights/error).
    void reset_add_model_form();
    void draw_jobs();        // compute jobs (system.jobs) + per-job objective/scaling
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
    // When `plan` is non-null, each step is overlaid with its projected cluster
    // placement — a coloured left stripe + a mono badge (near-data / fan-out /
    // all nodes / coordinator) — turning the canvas into a graph-over-cluster view.
    // When `run_progress` is non-null (#626), each step additionally wears its
    // live run status — a tone_of-coloured dot, an amber pulse on the running
    // step, a red border + reason on a failed one, and the short id of the
    // output it materialized — so the canvas shows a run moving through the graph.
    void draw_graph_canvas(const zetta::pipeline::ClusterPlan* plan,
                           const zetta::pipeline::RunProgress* run_progress);
    // The newest run that references the open graph — by the stored graph id,
    // the graph's saved name, or a deployment of that name (whose runs the
    // deployment controller starts). Scanned from the live Node::runs() each
    // frame (a thin renderer keeps no copy); null when none reference it.
    std::shared_ptr<zetta::pipeline::Run> latest_run_for_open_graph() const;
    // The live-run strip below the canvas (#626): the overlaid run's identity +
    // status (linking into the Runs view), the pinned failing reason when it
    // failed, and the selected step's run detail — status, reason, outputs, and
    // a click-through entity_link to a produced model's panel.
    void draw_graph_run_status(const zetta::pipeline::Run* run,
                               const zetta::pipeline::RunProgress& progress);
    // Execute the open graph's SAVED contents as a real Run on this node,
    // labelled with the project + stored-graph id so the live-run overlay (and
    // any other reader of run.graph) can tie the run back to the graph. A
    // churn-shaped graph runs on the real churn engine over the node's live
    // JobCoordinator (the console/deploy wiring); anything else runs through
    // Node::run_graph's DefaultStepExecutor. Synchronous on the UI thread that
    // owns the node — the same deliberate per-click pattern as run_churn_demo.
    void run_open_graph(zetta::pipeline::Project& project);
    // The cluster-plan section below the canvas (#591): a legend, the current
    // cluster's node strip (the nodes the selected step lands on are highlighted),
    // and a per-step placement table. A thin, read-only projection of how the graph
    // would run over the live cluster (zetta's SPMD + compute-near-data model).
    void draw_graph_plan(const zetta::pipeline::ClusterPlan& plan);
    // Build the cluster-plan projection for the open working-copy graph over the
    // live cluster: resolve each step's source table → partition owners (from the
    // dataset-partition catalog) and project placement via project_cluster_plan.
    zetta::pipeline::ClusterPlan build_graph_plan() const;
    // The add/rename/delete controls + Save/Revert below the canvas.
    void draw_graph_editor(zetta::pipeline::Project& project);
    // Load a fresh working copy of `stored` for editing (clears selection/dirty).
    void open_graph(const zetta::pipeline::ProjectGraph& stored);

    void handle_incoming_files();  // drain + route the queue (main thread)
    void enqueue_file(std::string path, FileIntent intent);
    static void on_open_records(void* userdata, const char* const* files, int filter);
    static void on_open_large_object(void* userdata, const char* const* files, int filter);
    static void on_open_dataset(void* userdata, const char* const* files, int filter);
    static void on_open_n8n(void* userdata, const char* const* files, int filter);
    static void on_open_model_weights(void* userdata, const char* const* files,
                                      int filter);

    Node& node_;
    std::function<void()> tick_hook_;
    zetta::cluster::Cluster& cluster_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    // Geist Mono face for labels / counts / metrics / paths (the design system's
    // mono role). Non-owning — the ImGui font atlas owns it; re-captured whenever
    // the atlas is rebuilt in apply_display_scale().
    ImFont* mono_font_ = nullptr;
    // Geist heading face for page_header titles — same atlas/ownership rules.
    ImFont* heading_font_ = nullptr;
    bool initialized_ = false;
    bool show_demo_ = false;

    // Which panel the content area is showing (set ONLY via navigate_to).
    // Defaults to the project-first Home landing.
    View active_view_ = View::Home;
    // Entity ids handed to navigate_to, waiting for their view's next draw to
    // consume (take_pending_selection). Presentation state only.
    std::map<View, std::string> pending_selection_;
    // Asks draw_about_modal() to open the About popup on the next frame (set from
    // the Help menu), mirroring the New Project modal's request flag.
    bool about_requested_ = false;

    // Command palette (#625, app_palette.cpp). `capability_registry_` is the
    // action source (set_capability_registry; the palette lazily builds
    // `owned_capability_registry_` with the core capabilities when main wired
    // none). Everything else is presentation state: the query, the selected
    // row, the capability awaiting arguments (with its staged text fields),
    // and the last call's outcome (status line + pretty result JSON) — the
    // entry list itself is rebuilt from live sources every frame.
    zetta::api::Registry* capability_registry_ = nullptr;
    std::unique_ptr<zetta::api::Registry> owned_capability_registry_;
    bool palette_requested_ = false;  // Cmd+K / menu asks to open next frame
    std::string palette_query_;
    int palette_selected_ = 0;
    std::string palette_capability_;  // argument step's entry ("" = list step)
    std::map<std::string, std::string> palette_args_;  // field -> staged text
    bool palette_refocus_ = false;  // refocus the step's first input next frame
    std::string palette_status_;    // last call's outcome line ("" = none)
    bool palette_failed_ = false;   // tones palette_status_
    std::string palette_trace_;     // last call's trace id ("" = none)
    std::string palette_result_;    // last call's pretty result JSON ("" = none)

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
    std::string selected_dataset_;  // dataset table id; re-resolved each frame
    std::string selected_model_;  // model id; re-resolved each frame
    std::string selected_job_;  // compute-job id; re-resolved each frame
    std::string selected_trace_;  // trace id (Traces view); re-resolved each frame
    std::string selected_span_;   // span id within the selected trace ("" = none)
    std::string selected_principal_;  // principal name; re-resolved each frame
    // Cluster panel: the node id of the box selected in the spatial canvas
    // ("" = none). Re-resolved against the live Cluster::nodes() each frame, so a
    // peer that leaves/fails simply drops out of the grid and the selection falls
    // back to self — the view tracks membership with no separate copy of it.
    std::string selected_cluster_node_;
    // Animated per-node gauge state (#584), keyed by node id. A thin renderer's
    // working copy of Node::telemetry_snapshot() (system.node_metrics gathered
    // cluster-wide over the signal plane): holds only the eased values + sparkline
    // history needed to animate the bars between the ~1 Hz samples. Updated each
    // frame in draw_cluster() and pruned of nodes that have left the cluster.
    std::map<std::string, NodeGaugeState> cluster_gauges_;

    // Distributed-algorithm-at-work overlay state (#590, epic #575 area 5): the
    // short-lived eased "flows" that animate partition placement / near-data loads /
    // scatter-gather collective rounds / rebalance moves over the node boxes. A thin
    // renderer's working copy of the live instrumentation read views
    // (system.dataset_partitions / system.pipeline_jobs / system.collective_rounds) —
    // diffed each frame in draw_cluster(); it owns no numbers, only the animation.
    ClusterPipelineState cluster_pipeline_;

    // Add nodes control (epic #575 area 1). `provisioners_` maps a CloudProvider
    // catalog id → the injected Provisioner the control drives for that target
    // (main wires "local" to the given-hardware LocalProvisioner; cloud
    // provisioners are injected by a configured deployment and stay consent-gated).
    // `selected_add_target_` is the catalog id chosen in the control;
    // `add_nodes_count_` how many to add; `add_nodes_status_` the last action's
    // result line (coloured amber via `add_nodes_status_warn_` when it came up
    // short). There is no persisted "granted" state — the cloud Allow button IS the
    // per-action consent, so a cloud add asks every time (deny-by-default).
    std::map<std::string, zetta::cloud::Provisioner*> provisioners_;
    std::string selected_add_target_{"local"};
    int add_nodes_count_ = 1;
    std::string add_nodes_status_;
    bool add_nodes_status_warn_ = false;
    // The in-flight async add-nodes op (null = idle). The provision runs off the
    // UI thread via runtime::spawn; the frame loop polls this + renders a progress
    // bar. `add_nodes_eta_seconds_` is a within-session baseline — the duration of
    // the last successful add for this target, seeding the progress bar's ETA on
    // the next one (0 = no baseline yet, so the bar shows elapsed-only).
    std::shared_ptr<AddNodesOp> add_nodes_op_;
    std::map<std::string, double> add_nodes_eta_seconds_;
    // Editable placement for a cloud add: region + VM size, applied to the injected
    // provisioner via Provisioner::set_placement before the add so the operator can
    // steer where a burst lands (complementing the backend's own SKU fallback).
    // Pre-filled from the detected system.provisioners config when the target
    // changes (tracked by add_nodes_placement_target_).
    std::string add_nodes_region_;
    std::string add_nodes_vm_size_;
    std::string add_nodes_placement_target_;
    // VM-size dropdown options for the current region + the async fetch that
    // populates them (az vm list-skus, off the UI thread — polled by poll_add_nodes).
    std::vector<std::string> add_nodes_size_options_;
    std::string add_nodes_sizes_region_;  // region add_nodes_size_options_ are for
    std::shared_ptr<VmSizesOp> add_nodes_sizes_op_;  // in-flight fetch (null = idle)
    // Per-region cache of fetched size lists — `az vm list-skus` is ~50s, so a
    // region is fetched at most once per session (re-selecting it is instant).
    std::map<std::string, std::vector<std::string>> add_nodes_sizes_cache_;

    // Cloud Inspector (permission-gated). `selected_cloud_provider_` is the
    // catalog id selected in the list. `cloud_consent_provider_` is the id the
    // user has *explicitly* consented to inspect right now — deny-by-default and
    // reset whenever the selection changes, so consent is never carried over to a
    // different provider or remembered across selections (CLAUDE.md hard rule).
    // Only after consent does run_cloud_inspect() run the provider's read-only
    // commands; the result is cached on the node (system.cloud_status).
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

    // Search panel (#641/#643): the query being composed and the last search's
    // outcome, held verbatim (an action-outcome snapshot, like the query
    // panel's last result — presentation state, not a synced engine copy). The
    // engine result is behind a unique_ptr so this header stays light.
    std::string search_target_;   // index id or table name (combo-selected)
    std::string search_query_;    // the free-text query being composed
    int search_top_k_ = 10;       // result cap the panel requests
    std::unique_ptr<zetta::index::SearchResult> search_result_;
    std::string search_error_;    // the engine's reason when the last search failed
    std::string search_selected_hit_;  // ranked-hit master-detail selection (row id)
    // The Search view's cluster strip: a completed search's scatter-gather
    // replayed as #590 flows over the answering nodes' boxes (#643). Only the
    // flows / node_activity members are used — pure animation state, anchored
    // to the last result's real per-node provenance.
    ClusterPipelineState search_flows_;
    zetta::observability::SpanRecorder* span_recorder_ = nullptr;  // may be null

    // Console panel — the governed Console/REPL screen (SCR-8 / #525), hosting the
    // reusable CMP-6 component (#524). The model (zetta-core, no ImGui) holds the
    // session state — selected frontend, input buffer, scrollback transcript — so a
    // future CLI can render the same model; the component is its thin ImGui
    // renderer. The run handler routes every statement through governed_console_
    // (zetta-core): capability-scoped against the acting key, simulated-Python
    // lowered to the IR + run through the correctness gate, and audited (FEAT-9).
    // `console_session_key_` is the key id the console acts as ("" = an
    // unauthenticated local session); it is set into governed_console_ each frame.
    // Held by unique_ptr with the types forward-declared, to keep their headers out
    // of app.hpp (like query_).
    std::unique_ptr<repl::ConsoleModel> console_model_;
    std::unique_ptr<ReplConsole> console_;
    std::unique_ptr<repl::GovernedConsole> governed_console_;
    std::string console_session_key_;  // acting key id ("" = unauthenticated local)

    // Correctness-gate panel (SCR-4 / CMP-2): the execution-graph IR in its
    // canonical JSON form to validate, an inline parse/validation error (a
    // reason, never a stack trace), and the last ValidationReport — null until
    // the gate is run. The graph JSON enters through ir::from_json, which
    // validates it against the IR schema at the boundary.
    std::string gate_graph_input_;
    std::string gate_error_;
    std::unique_ptr<zetta::pipeline::ValidationReport> gate_report_;

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

    // Models panel — the inline "Add model" form (draw_add_model_form). Register
    // or import a trained model via Node::create_model (+ optional set_weights).
    // `metrics` / `feature_schema` are pasted as JSON and re-validated against the
    // Model JSON Schema at this boundary before create. `weights_path_` is a file
    // chosen via the native dialog (staged through FileIntent::ModelWeights) whose
    // bytes are attached on Register; `weights_name_` is its display filename.
    // Nothing is persisted until Register, so the form is a staging area only.
    std::string add_model_name_;             // optional display label
    std::string add_model_kind_{"classifier"};  // required; default classifier
    std::string add_model_metrics_;          // metrics JSON (object of numbers), optional
    std::string add_model_features_;         // feature_schema JSON (object), optional
    std::string add_model_weights_path_;     // staged weights file ("" = none)
    std::string add_model_weights_name_;     // its display filename
    std::string add_model_error_;            // inline register error

    // Activity panel (#622) — presentation-only filters over the feed computed
    // fresh each frame (zetta::observability::activity_entries; no stored copy).
    int activity_window_ = 0;   // 0 = last hour, 1 = last 24 hours, 2 = all time
    int activity_surface_ = 0;  // 0 = all surfaces, 1 = GUI, 2 = console, 3 = MCP

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
    // Cluster-plan overlay (#591): when on, the canvas + a plan section project how
    // the open graph would run over the current cluster (near-data / fan-out /
    // owner nodes). On by default so opening a graph shows the projection; a
    // checkbox above the canvas hides it for a plain edit view.
    bool show_cluster_plan_ = true;
    // Live-run overlay (#626): when on, the newest run referencing the open
    // graph is projected onto the canvas (per-step status / active-step pulse /
    // pinned failure / materialized outputs) and a run strip renders below it.
    // Re-derived from the live run record every frame (the #590 pattern — no
    // GUI-owned copy of run state, bounded work); on by default so a run is
    // visible the moment one exists.
    bool show_run_overlay_ = true;

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
