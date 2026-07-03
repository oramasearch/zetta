# Graph Workspace live-run overlay (#626)

The **Graph Workspace canvas shows a run moving through the open graph**: each
step wears its live status (Orama semantic green/amber/red), the step
executing right now pulses, a failed step is pinned red with its failing
reason from the run log, and the outputs a step materialized (dataset / model
/ index ids) appear on its box as the run produces them — with a click-through
to a produced model's panel. It is the Graph Workspace's counterpart to the
Cluster view's [distributed-algorithm animation](cluster-pipeline-animation.md)
(#590): the cluster canvas shows *where* work happens, this overlay shows
*where a run is inside the user's mental model of the pipeline*. Epic
[#614](https://github.com/oramasearch/zetta/issues/614) UX companion; builds on
the [cluster-plan overlay](../pipeline/graph-plan.md) (#591) sharing the same
canvas.

Like #590 it is a **thin renderer with no invented state**: everything drawn
is re-derived every frame from the live, already-schema-validated `Run` record
(`Node::runs()`), so a run started from the **console, the deploy panel, MCP,
or a replicating peer animates identically** to one started in the workspace —
the overlay reads the record, not the initiator. A peer's run arriving log
line by log line over the CRDT gossip progresses live on the canvas.

## The engine half: `pipeline::project_run_progress`

`include/zetta/pipeline/run_progress.hpp` (impl
`src/core/pipeline/run_progress.cpp`) projects a `RunValue` onto an
`ir::ExecutionGraph`, deriving per step:

- **status** (`StepRunStatus`: pending / running / succeeded / failed) from
  the run's log — the `GraphExecutor` writes `"<kind>: <label>"` as each step
  starts and `"<kind> step '<label>' failed: <reason>"` on failure, and runs
  steps strictly in dependency order fail-fast, so the last started step is
  the running one and every earlier start completed;
- the **failing reason**, verbatim from the log (plus a run-level
  `failure_reason` that falls back to the last error line for runs that failed
  before any step — an empty graph or a cycle);
- the **outputs** each step materialized, attributed by the default executor's
  logical `"<kind>:<step id>"` ref, falling back to the step label both
  executors stamp into `RunOutput::name` (the churn engine's model/index refs
  are real, resolvable entity ids).

The projection is a pure `O(logs + steps + outputs)` function — cheap enough
to recompute per frame (the #590 bounded-work constraint) — and lives beside
the executor whose log grammar it reads, so
`tests/pipeline/test_run_progress.cpp` drives both through the real
`GraphExecutor` and pins them to the same grammar. Steps a run's log never
mentions stay pending, so projecting an unrelated run shows nothing rather
than something wrong.

## The GUI half (`src/gui/app_graph_workspace.cpp`)

- **Which run**: `App::latest_run_for_open_graph()` scans the live
  `Node::runs()` each frame for the newest run referencing the open graph — by
  the stored graph **id** (`run.graph()`), the graph's saved **name**, or a
  **deployment** of that name (whose runs the deployment controller starts).
  No copy is kept; the "Live run" checkbox (on by default) hides the overlay.
- **Canvas** (`draw_graph_canvas`): per step a `tone_of(StepRunStatus)` status
  dot top-right (pending renders as a hollow ring), a time-eased amber pulse
  around the running step, a red border on a failed one (coral stays the
  selection accent — status never borrows it), and the first output's short id
  bottom-right in mono. Hovering a step shows status · reason · full outputs.
- **Run strip** (`draw_graph_run_status`, below the canvas): the run's
  identity + status + step tally, an `entity_link` into the Runs view, the
  pinned failure `banner` (the failing step + reason, verbatim), and the
  selected step's outputs — a `model` output whose ref resolves via
  `Node::find_model` links through to the Models panel. Unresolvable logical
  refs render as plain mono; the overlay never fakes a destination. (An
  Indexes panel is a later slice — index ids render as text.)
- **Run graph** (`App::run_open_graph`): executes the open graph's **saved**
  contents (disabled while the working copy is dirty) as a real Run labelled
  with the project + stored-graph id — the workspace's own run-initiating
  action, so the overlay has GUI-initiated runs to show. A churn-shaped graph
  routes through the real churn engine over the node's live `JobCoordinator`
  (the governed-console wiring, #580); anything else runs the structural
  `DefaultStepExecutor`. Synchronous on the UI thread that owns the node (the
  `run_churn_demo` per-click pattern).

Status colours come only from `tone_of(...)` — `StepRunStatus` adds one
overload in `src/gui/tones.hpp` (#636), deliberately identical to
`tone_of(RunStatus)` so a step reads exactly like the run it belongs to.

## No new JSON boundary

The overlay parses no JSON and adds no format: it consumes the `RunValue` that
was schema-validated at its own boundary (`parse_run_value`) and the typed IR
graph. Nothing is written except when the user explicitly runs the graph —
which goes through the existing, already-validated Run write path.

## Scope notes (deliberately later)

- Per-step **timings** (the observability spans) on the canvas — the run log
  carries per-line stamps; a duration read is a later slice
  ([#676](https://github.com/oramasearch/zetta/issues/676)).
- Animated **flows along the edges** (the #590 comet vocabulary) between steps
  ([#678](https://github.com/oramasearch/zetta/issues/678)).
- A dedicated run **picker** — the overlay follows the newest referencing run;
  older runs are read in the Runs view
  ([#677](https://github.com/oramasearch/zetta/issues/677)).
- An **Indexes panel** for the index-output click-through
  ([#675](https://github.com/oramasearch/zetta/issues/675)), and a durable
  run↔stored-graph id link across every initiator so the name/deployment
  matching heuristics shrink to fallbacks
  ([#679](https://github.com/oramasearch/zetta/issues/679)).
- `system.pipeline_jobs` job-level activity stays on the Cluster view's
  animation; the graph canvas shows run-record truth only.
