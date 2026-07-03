# AI Solution Library & the Deploy panel

The **Deploy panel** is the headed UI for *one-button deploy* (FEAT-4 / screen
SCR-5 / component CMP-3): pick a ready-made AI solution, choose **where** to run
it on the placement dial, and deploy it to a target — seeing the result and the
recorded placements without leaving the screen. It is the GUI face of the
[deployment](deployments.md) slice.

Its source of deployable graphs is the **AI Solution Library** — a curated,
schema-validated catalog of ready-to-deploy [execution-graph](execution-graph.md)
blueprints. Orama Zetta's framing is *use-case-driven*: a use case (churn,
recommendations, semantic search, …) is a generic AI-project type a team
instantiates on its own data — "the use cases are the consumable meals." The
catalog is that menu, and the Deploy panel is where a user orders from it.

This slice ships a **built-in** catalog and a **Local-only** deploy. Loading
user-authored catalogs from disk, instantiating a solution into a
[Project](projects.md), and the non-local targets are later slices.

## The library — `zetta::pipeline::SolutionCatalog`

A `Solution` (`include/zetta/pipeline/solution_catalog.hpp`) is one AI-solution blueprint:
identity + presentation + the deployable graph.

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | Stable slug, unique within a catalog (e.g. `churn-prediction`). The Deploy panel selects by id. |
| `name` | `std::string` | Human name shown in the picker (e.g. "Customer Churn Prediction"). |
| `summary` | `std::string` | One-line description of the outcome it delivers. |
| `category` | `std::string` | Grouping label ("Customer Intelligence", "Search", …). |
| `graph` | `ir::ExecutionGraph` | The blueprint placed by `deploy_graph`. |

`SolutionCatalog` is an ordered, read-only library of solutions. `find(id)` hands
back a **non-owning** `const Solution*` (or `nullptr` on miss), valid for the
catalog's lifetime — the catalog owns its solutions by value.
`SolutionCatalog::builtin()` is a process-wide, immutable catalog parsed once from
embedded data.

### The built-in catalog

`builtin()` ships five representative blueprints, each a small but real pipeline
over the `connect→clean→model→index→serve→act` taxonomy:

| Solution | Category | Shape |
|---|---|---|
| Customer Churn Prediction | Customer Intelligence | connect×2 → clean → model → index → serve → act |
| Product Recommendations | Personalization | connect → clean → model → index → serve |
| Semantic Document Search | Search | connect → clean → index → serve |
| Sales Lead Scoring | Revenue | connect → clean → model → serve |
| Support Ticket Triage | Operations | connect → clean → model → act |

The built-in data is authored in the catalog's JSON form so it travels the **same
schema + IR boundary validation** as any external catalog would (a test asserts it
parses and every blueprint passes the gate). If the embedded data ever failed to
parse, `builtin()` logs to `stderr` and degrades to an empty library rather than
crashing the headed app.

The churn blueprint's shape (connect×2 → clean → model → **index** → serve → act)
**matches the real churn pipeline** the engine runs, so the same graph that deploys
also runs end-to-end on the real engine — see *[Run on the real
engine](#run-on-the-real-engine-churn)* below and
[churn-solution.md](../use-cases/churn-solution.md). The `index` step is the
[score / rank-at-risk](../ml/gbdt-score.md) stage; without it there would be no ranked
list for the `act` (retention playbook) step to act on.

## Boundary validation (schema + IR)

The catalog is **data-driven and schema-validated at the boundary**, matching the
[Run record](runs.md)'s layered approach:

`parse_solution_catalog(document, &error)` is **non-throwing** (returns
`std::optional`, matching zetta's no-exceptions convention) and validates in
layers, failing fast with a human-readable, path-qualified reason:

1. **Document shape** — the document is checked against the catalog **JSON
   Schema** (draft-07) via `pboettch/json-schema-validator`: a `solutions` array
   of `{ id, name, summary, category, graph }`, with the step/edge-kind enums and
   `additionalProperties: false`. The schema is the single declarative source of
   truth, embedded in `src/core/pipeline/solution_catalog.cpp` and mirrored by
   [`schemas/solution_catalog.schema.json`](../../schemas/solution_catalog.schema.json)
   — keep the two in sync.
2. **Graph structure** — each blueprint's `graph` is then parsed through
   [`zetta::ir::from_json`](execution-graph.md), the IR's own trust boundary
   (unique node ids, no dangling/duplicate edges). A structurally invalid graph is
   rejected with the IR's diagnostic, path-qualified as
   `solutions[i].graph: …`.
3. **Id uniqueness** — duplicate solution ids are rejected rather than silently
   shadowed, since `find()` and the panel both resolve by id.

A malformed catalog is rejected with a reason instead of corrupting state.

## The Deploy panel

`App::draw_deploy()` (`src/gui/app.cpp`) is a collapsing panel in the headed UI,
between Query and Cluster. Its flow realizes one-button deploy:

1. **Pick a solution** — a combo over `SolutionCatalog::builtin()`; hovering a row
   shows its category + summary. The selection is held by id
   (`selected_solution_`), and the panel shows the chosen blueprint's category and
   step/edge counts.
2. **Choose the placement** — the **placement dial** combo over `DeploymentKind`
   in its declared `local → kubernetes → vpc → cloud` order. **R1 is Local only**:
   the non-local kinds appear (labelled "… (planned)") but are **disabled**, so
   the dial communicates the roadmap while only Local can be chosen — the same
   "declare before you build" pattern as `ClusterRole`.
3. **Deploy** — the one **Deploy** button (disabled until a solution is picked):
   - **Runs the correctness [gate](gate.md) first** — `default_gate().run(graph)`.
     If it blocks (RULE-1: *no deploy without a passing gate*), the panel shows the
     failing check's reason in red and **stops** — nothing is placed.
   - Otherwise calls `Node::deploy_graph(graph, target, name)`. On success it
     shows a green confirmation with the new deployment's short id; an empty handle
     surfaces as an error (e.g. a non-local target, which R1 refuses).
4. **See the status** — a **Deployments** table lists every recorded placement
   (`Node::deployments()`): graph name, target, step/edge counts, and created
   stamp. Because deployments are replicated, placements made on a peer show up
   here too. This is the acceptance criterion — *deploy a validated graph to the
   local target and see its status*.

The panel adds **no new persisted state of its own** — it composes the existing
catalog, gate, and `deploy_graph` pieces. The only new state is UI selection
(`selected_solution_`, `deploy_placement_`, `deploy_status_`, `deploy_failed_`)
and the run result line (`churn_run_status_`, `churn_run_failed_`).

### Run on the real engine (churn)

`deploy_graph` records a *placement*; it does not train anything (the
deploy → reconcile → run control plane records run shells with the stubbed
`DefaultStepExecutor`). For the churn blueprint the panel also offers a **"Run over
the cluster"** action (`App::run_churn_demo`) that runs the actual
featurize → train → explain → rank → playbook pipeline over a built-in demo dataset
— the real [`ChurnStepExecutor`](../use-cases/churn-step-executor.md), not the stub. On success
it shows the registered model's short id, the number of accounts ranked, and the
top churn driver; the run also registers a real [Model](../ml/models.md) + recorded
[ComputeJobs](../compute/compute-jobs.md) on the node, so the [Runs](runs.md) and
[Jobs](../gui/jobs-panel.md) panels reflect it.

The action is offered **only for the churn solution** (the only blueprint wired to a
real pipeline today) and runs **synchronously on the UI thread** that owns the node
— the same deliberate per-click pattern as the [Cloud Inspector](../cloud/cloud-inspector.md).
It runs over the node's **live `JobCoordinator`** — the long-lived
[#579 compute spine](../compute/job-coordinator.md) over the real cluster
(`run_churn_solution_on_node`, #580/#637), not an isolated loopback — so the
produced Model + Run land on the live node (the headed app always attaches a
cluster; if none is attached the runner's clear failure reason is shown as the run
status). The wiring it calls — `zetta::usecases::churn::run_churn_solution_on_node`,
the ONE public churn runner — is a reusable core surface; see
[churn-solution.md](../use-cases/churn-solution.md#one-runner-over-the-live-cluster-580-637).

## What's deferred (later slices)

- **User-authored catalogs** — loading a catalog document from disk (the parser +
  schema already support it; only `builtin()` is wired today).
- **Instantiating a solution into a Project** — today deploy records a placement
  from the blueprint directly; binding a solution to a project's inputs/data is a
  later slice.
- **The non-local dial** — k8s / VPC / cloud targets and their Cloud Interface
  bindings (see [deployments.md](deployments.md) → *What's deferred*).
- **Richer status** — run/reconcile state per deployment once the
  reconcile/self-heal loop and [Runs](runs.md) execution land.

## Tests

`tests/pipeline/test_solution_catalog.cpp` covers the built-in library (non-empty, unique
ids, every blueprint passes the gate), `find()` by id and on miss, and the
boundary parser: it accepts a valid document and rejects a missing required field,
an unknown step kind, an unknown top-level key, a dangling edge (caught by the IR
boundary, path-qualified), and duplicate ids.

See also FEAT-4 (`#135`), SCR-5 (`#226`), CMP-3 (`#69`), [gate.md](gate.md), and
[deployments.md](deployments.md).
