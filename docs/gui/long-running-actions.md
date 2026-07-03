# Long-running actions off the UI thread — progress + cancel

**Issue:** [`#624`](https://github.com/oramasearch/zetta/issues/624) ·
**Code:** `App::poll_background_ops` (`src/gui/app.cpp`), `CloudInspectOp` +
`App::run_cloud_inspect` / `poll_cloud_inspect` (`src/gui/app_cloud.cpp`),
`App::run_churn_demo` + `draw_churn_run_progress` (`src/gui/app_deploy.cpp`),
`App::run_open_graph` (`src/gui/app_graph_workspace.cpp`), the dataset add in
`App::handle_incoming_files` (`src/gui/app.cpp`) + the progress block in
`src/gui/app_datasets.cpp` · engine side:
[../architecture/owner-gate.md](../architecture/owner-gate.md)

The rule this page enforces: **the UI thread never blocks on engine work.** A
long action runs off the frame loop, its owning panel shows a progress
affordance, cancel exists where the engine supports it, and completion surfaces
in the activity feed. `App::poll_background_ops()` runs once per frame from
`run()` (beside `poll_add_nodes`) regardless of the active view, so background
work completes even when the user navigates away.

## The three actions

| Action | Off-thread how | Progress | Cancel |
|---|---|---|---|
| Cloud inspect (Cloud panel) | `runtime::spawn` coroutine runs the provider CLI (the `AddNodesOp` pattern); the poll applies `record_cloud_inspection` on the UI thread | "Inspecting · Ns" in the consent gate; Allow / Re-inspect disabled while in flight | No (a CLI subprocess has no kill seam — later slice) |
| Churn run (Deploy panel + Graph Workspace "Run graph") | one shared `usecases::churn::AsyncChurnRun` — a core worker whose node writes gate through `Node::run_on_owner` | live from the run's OWN records: headline + elapsed, the Run's latest log line, an entity link to the live Run; the #626 run overlay animates the workspace run as it executes | Yes — cooperative, between graph steps ("cancelling after the current step…") |
| Dataset add (Datasets panel / file drop) | `data::AsyncDatasetAdd` — decode on a runtime worker, materialize pumped in bounded owner-thread slices per frame | phase line + row counts, indeterminate marquee while decoding, a real fraction while adding rows | Yes — mid-decode touches nothing; mid-materialize drops the partial table |

## The load-bearing points

- **Consent is unchanged.** The cloud inspect still runs ONLY from the consent
  gate's explicit Allow / Re-inspect; going async moved the *work*, not the
  permission boundary (deny-by-default, per-action, never remembered).
- **One progress source for GUI + MCP.** The churn panel renders the live Run +
  jobs records — the same `system.runs` / `system.jobs` rows an MCP caller
  polls through `sql`. No panel-owned copy of engine progress exists
  (thin-renderer rule); the only GUI state is the op handles themselves.
- **One churn run at a time.** The Deploy panel, the workspace, and the
  guided demo ([onboarding.md](onboarding.md), #623) share `churn_run_`; every
  launch button disables while it is set. `churn_run_from_deploy_` /
  `churn_run_from_onboarding_` decide who owns the completion status line (the
  workspace's feedback is the live-run overlay itself).
- **Completion → activity feed.** A finished dataset add records
  `dataset_added` (deep-links to the Datasets view), an inspect records
  `cloud_inspected` (links to the Cloud panel); a churn run already surfaces
  through its Run entries. The feed renders them like every other action.
- **Teardown order matters.** `~App` resets the background handles FIRST —
  `~AsyncChurnRun` cancels + joins its worker while pumping the node's owner
  gate (joining without draining would deadlock); the inspect + dataset
  workers own their state via `shared_ptr`, so dropping the handles is safe.

## Testing

Headless coverage lives with the engine pieces: the owner gate + inspect audit
(`tests/test_node.cpp`), executor cancel (`tests/pipeline/test_executor.cpp`),
the async churn run + cancel over a live coordinator
(`tests/usecases/churn/test_churn_solution.cpp`), the async dataset add /
cancel / audit (`tests/data/test_add_dataset.cpp`), and the feed entries
(`tests/observability/test_activity.cpp`).
