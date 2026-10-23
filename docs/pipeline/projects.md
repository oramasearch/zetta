# Projects

An **AI project** (`zetta::pipeline::Project`) is the central, user-managed artifact in
Orama Zetta. A project is instantiated from a **use-case type** (e.g. churn
intelligence) on a team's data, and — in later slices — owns that use case's
inputs, execution graph(s), and runs. Orama Zetta's job is to *manage projects
to success* (most AI projects fail), so the project is the root everything else
hangs off of. See [orama-zetta.md](../product/orama-zetta.md) for that framing.

This page documents the **data-model root**: the `Project` type and its
lifecycle (CRUD) on a `Node`. Ingestion, graph compilation, and the New Project
screen are separate work.

## The type

A `Project` (`include/zetta/pipeline/project.hpp`) is metadata plus the execution graphs
it owns (see [Execution graphs](#execution-graphs) below). The metadata:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `name` | `std::string` | Human name; unique within the node. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation (e.g. `2026-06-25T14:03:21.123Z`). |
| `updated` | `std::string` | ISO-8601 UTC stamp; bumped on rename. |
| `owner` | `std::string` | Owner **key** (for key-scoped access); empty by default. |

Stamps come from `zetta::util::now_iso8601()` (`include/zetta/util/timestamp.hpp`) — a
fixed-width UTC string, so the values also order chronologically as plain text.

A project owns its **inputs** (`zetta::pipeline::ProjectInput`) — raw intent/artifact
payloads captured with provenance; see [project-inputs.md](project-inputs.md) —
and its **execution graphs** ([below](#execution-graphs)), the same way a
database owns its tables. Its runs are **not** part of this type yet; they attach
in later slices (id-only references, keeping a project lightweight).

## CRUD on a Node

`Node` owns projects alongside databases, with the same shape as the database
CRUD (`shared_ptr` handles, empty/`false` on miss or clash):

| Method | Returns | Behavior |
|---|---|---|
| `create_project(name, owner = {})` | `shared_ptr<Project>` | Empty handle if `name` is empty or already taken. Stamps `created`/`updated`. |
| `find_project(name)` | `shared_ptr<Project>` | Empty handle on miss. |
| `drop_project(name)` | `bool` | `true` if one was removed. Removes the project **with its inputs and graphs** (their `input:`/`graph:` keys and the folder); runs and models are kept (see [Deleting a project](#deleting-a-project--delete_project-1009)). |
| `rename_project(from, to)` | `bool` | `false` if `to` is empty/taken or `from` is missing. Bumps `updated` and refreshes denormalized names on matching Runs, Models, and Studies. |
| `project_names()` | `vector<string>` | Sorted by name. |
| `project_count()` | `size_t` | — |

```cpp
auto churn = node.create_project("churn", "team:risk");   // owner key optional
node.rename_project("churn", "retention");                // keeps the id
node.drop_project("retention");
```

## Listing projects — `list_projects`

`list_projects` (REST `GET /v1/list_projects`, the MCP tool of the same name) takes
no input and answers the projects people see (internal projects are left out, see
[../ml/standalone-models.md](../ml/standalone-models.md)) in one call:

```json
{
  "projects": ["churn", "renewals"],
  "summaries": [
    { "id": "<uuid>", "name": "churn",
      "graphs": [ { "id": "<uuid>", "name": "Weekly Churn", "approval": "approved" } ] },
    { "id": "<uuid>", "name": "renewals", "graphs": [] }
  ]
}
```

- **`projects`** is the list of names, unchanged, for the callers that read it.
- **`summaries`** (#1185) has one entry per name, in the same order: the project's
  `id` and `name`, and each pipeline's `id`, `name` and `approval` (`proposed` or
  `approved`, #885), the same values `get_project` returns. It lets a list page (a
  project list, the pipelines an agent may be granted) render without one
  `get_project` per project. It is built from the projects already held in memory,
  so it costs no reads of disk or of other nodes.
- **Access.** `list_projects` and `get_project` both need `api:query`. A summary is
  part of `get_project`'s answer, so it is read with that tool's right: a user whose
  `mcp:call` is scoped to `list_projects` alone (#747) gets `projects` and no
  `summaries` field. AccessKeys and the local session are authorized on `api:query`
  for both tools, so for them the two always agree.
- Anything else about a pipeline (methodology, runnable, who approved it and when,
  the inputs) stays on `get_project`.

## Deleting a project — `delete_project` (#1009)

The control-surface entry is the **`delete_project`** action (REST
`POST /v1/delete_project`, the MCP tool of the same name), authorized like
`create_project` — its own right, `orchestration:delete_project`, in the
`pipelines:manage` bundle — and audited like every capability call.

- **Input:** `{ "name": "<project>" }` (required, the name `list_projects` shows).
- **Output:** `{ name, id, removed: { inputs, graphs }, kept: { runs, models } }`.
- **Missing project:** `not_found`.
- **A run of the project still `pending` or `running`:** refused (`engine_error`) —
  deleting the graph out from under an executing run would fail it half-way.

**What goes:** the project, its captured inputs (and their payload bodies) and its
graphs — removed from the replicated document (tombstones, so every peer drops
them on its next reconcile) and from disk. Before #1009, `drop_project` removed the
graphs but left each `input:` key behind naming a project that no longer existed;
the integrity check counted them as `dangling_inputs`.

**What stays, deliberately: runs and models.** A run is a record of what happened
and a model is what was trained; a model may be deployed or exposed and serving
traffic, and its lineage is what an audit reads. Neither is *part* of the project —
each carries the project's **id** as a plain label (`run.project`, `model.project`).
They stay listed in `list_runs` / `list_models` / `system.runs` / `system.models`,
still carrying that id, which now resolves to no project. The answer's `kept`
counts them so the caller is not surprised to still see them. Deployments and agents
that name the project are likewise untouched. A project later created with the same
name gets a new id, so old runs do not attach to it.

Not undoable: there is no trash or restore.

## Persistence & replication

Projects follow zetta's **"EVERYTHING is replicated state"** rule: the Automerge
CRDT document is the source of truth, and the in-memory list + on-disk folder are
a **projection** of it ([replication.md](../cluster/replication.md)).

- **Replicated state.** Each project is one flat key in the document:
  `project:<uuid> → { name, created, updated, owner }` (`src/core/catalog.hpp`).
  Local create/rename/drop capture into the document via `ReplicationManager`;
  on each `tick()` the node **reconciles** the projection to match the document,
  so a peer's project (or rename/drop) materializes locally. Versions converge
  conflict-free, so a project created on one node appears on every peer.
- **On-disk projection.** A project is a folder named by its UUID under the data
  dir, holding `project.json` (`{ id, name, created, updated, owner }`), an
  `inputs/` subfolder for its inputs (see [project-inputs.md](project-inputs.md)),
  and a `graphs/` subfolder for its execution graphs:

  ```
  data_dir/
    <project-uuid>/
      project.json
      inputs/
        <input-uuid>/...
      graphs/
        <graph-uuid>.json
  ```

  `Node::start()` rebuilds the in-memory list by scanning the data dir
  (`load_projects()`, which calls `Project::load()` to read each project's
  inputs + graphs) and — when replicated — reconciling from the CRDT document, so
  a project **survives restart** in both standalone and replicated nodes. The
  folder name *is* the id, so rename only rewrites `project.json`.

An **internal** project (lake#654) carries `internal: true` in both its replicated
value and `project.json`; every other project's value is unchanged. It holds one
standalone model, is never listed (`Node::project_names()` leaves it out, so
`list_projects`, the GUI and the web UI never show it), and is found by its id.
`Node::reveal_project(id, name)` (the `move_model_to_project` capability) turns it
into a project people see. See [../ml/standalone-models.md](../ml/standalone-models.md).
`rename_project` keeps a project's description (it used to clear it).

`project.json` is **validated at the boundary** on load: a malformed or
incomplete file (not an object, or missing a string `name`) is skipped rather
than allowed to corrupt the in-memory tree — matching how `database.json` /
`table.json` are read.

## Execution graphs

A project owns zero or more **execution graphs** (`zetta::pipeline::ProjectGraph`,
`include/zetta/pipeline/project_graph.hpp`) — named, persisted `ExecutionGraph` IRs (see
[execution-graph.md](execution-graph.md)). This mirrors how a `Database` owns
tables: the CRUD captures into the CRDT, and the `materialize_*` variants are the
reverse path used by reconciliation.

| Method (on `Project`) | Returns | Behavior |
|---|---|---|
| `create_graph(name, graph)` | `shared_ptr<ProjectGraph>` | New UUID; empty handle if `name` is empty or already taken. Captures `graph:<uuid>`. |
| `compile_intent_to_graph(input_id, name = {}, options = {}, error = nullptr, template_name = nullptr)` | `shared_ptr<ProjectGraph>` | Resolve a runnable [methodology](../ml/methodologies.md), compile the stored **intent** with that methodology's shape callback, and store it through `create_graph` (without a name, named from the intent's words and numbered on a collision, #1238). `options` may select a methodology or primary solution and carry hints. Empty handle on an invalid input, unsupported methodology, missing body, or an explicit name's collision; `error` explains compilation refusal and `template_name` receives the template the steps started from. |
| `update_graph(name, graph, new_name = {})` | `shared_ptr<ProjectGraph>` | Replace an existing graph's contents (found by current `name`), optionally renaming to `new_name`. Same id; rewrites the projection, refreshes denormalized names on matching Runs, Models, and Studies, and re-captures `graph:<uuid>`. **Re-proposes the plan** (resets approval → proposed, #885). Empty handle if no graph by `name` exists, the final name is empty, or `new_name` collides. The "persist an edit back to the IR" path. |
| `approve_graph(name, approver, at)` | `shared_ptr<ProjectGraph>` | Mark a proposed plan **approved** (#885, review-before-build): stamps the approver + ISO instant, re-captures `graph:<uuid>`, and audits `graph_approved`. Idempotent. Empty handle if no graph by `name`. Reached over the governed `approve_plan` capability. |
| `find_graph(name)` / `find_graph_by_id(id)` | `shared_ptr<ProjectGraph>` | Empty handle on miss. |
| `drop_graph(name)` | `bool` | `true` if one was removed (also removes the CRDT key). |
| `graph_names()` / `graph_count()` | `vector<string>` / `size_t` | Sorted by name / count. |

A `ProjectGraph` is `id` + `project_id` + `name` + the `ir::ExecutionGraph`. It is
**replicated state**, following the same flat-key + projection rule as everything
else:

- **Replicated state.** One flat key per graph,
  `graph:<uuid> → { project: <proj_uuid>, name, approval, approved_by,
  approved_at, graph: { nodes, edges } }` (`src/core/catalog.hpp`). The whole
  graph rides as a single value (a graph is authored and stored as a unit) — the
  serialized form is
  [`ir::to_json`](execution-graph.md#json-serialization). On each `tick()` the
  node reconciles, so a peer's graph (or an edit/drop) materializes locally and
  versions converge conflict-free.
- **On-disk projection.** `<project-uuid>/graphs/<graph-uuid>.json` holds
  `{ id, project, name, graph }`. Dropping a project removes its folder (and so
  its graphs) and the `graph:` keys for its graphs.
- **Boundary validation.** Both load paths — reading a `graphs/*.json` file and
  decoding a replicated `graph:` value — parse the graph through
  [`ir::from_json`](execution-graph.md#schema-validation-at-the-boundary) and
  **skip** anything that fails the schema/structure check, so a malformed payload
  is dropped rather than allowed to corrupt the projection. The integrity
  preflight counts a `graph:` whose project is gone as a **dangling graph**.

### Plan approval — review-before-build (#885)

A `ProjectGraph` carries a **plan-approval state** so the product's
review-before-build flow (the design's *Proposed Solution* screen) has engine
truth behind it. It is part of the replicated record (`approval` +
`approved_by` + `approved_at`), reconciles across peers, and survives restart on
both the disk and CRDT paths.

- **Proposed by default.** Every compiled/created plan starts `proposed`
  (`create_graph` / `compile_intent_to_graph`). Editing a plan (`update_graph`)
  **re-proposes** it — a changed Pipeline must be approved again.
- **Approved by `approve_plan`.** The governed, audited `approve_plan` capability
  (→ `Project::approve_graph`) is the human "yes, build this": it stamps the
  approver + instant and audits `graph_approved`.
- **gate and deploy require approval.** `handle_gate` and `handle_deploy` refuse a
  `proposed` plan with `not_approved` (approve first). This is **orthogonal to
  the correctness gate**: approval is the human sign-off; the gate is the machine
  verification — a plan can be approved *and* still be blocked by the gate (the
  gate-blocked journey approves the leaky plan, then the gate catches the leak).
  The normal Studio flow is **approve → gate → deploy → run**; `run` inherits
  approval by sequence (it follows a deploy that required it).
- **agent_run requires approval too** ([#913](https://github.com/oramasearch/zetta/issues/913)).
  `agent_run` stores the plan it compiles `proposed` and answers `not_approved`
  without running it; it never approves a plan itself. Given `project` +
  `graph_name` naming a stored plan, it runs the plan only if it is approved,
  and refuses a proposed one with `not_approved` before the agent's model loads.
- **Surfaced for the UI.** `get_project` (per graph), `get_graph`, and the
  compile/save outputs carry `approval`; the two read capabilities also carry
  `methodology` and `runnable`. Studio statuses (Draft / Needs approval / Ready /
  Running) therefore map onto engine state without inferring model support.

## GUI panel

In headed mode the explorer's **leftmost column is Projects** — a thin renderer
over the `Node` project CRUD, mirroring the Databases column. It lets you:

- **create** a project (name field → `create_project`; inline error on an empty
  or duplicate name),
- **select** one (re-resolved by name each frame, like the database selection,
  so a dropped/renamed project never leaves a dangling handle),
- **rename** the selected project (`rename_project`; bumps `updated`), and
- **delete** one (`drop_project`, the `x` button).

Selecting a project shows its read-only details (short id, owner, `created`, and
`updated` when it differs). The panel holds no project state of its own — it
reads `project_names()` / `find_project()` every frame and writes through the
Node, so a project created/replicated elsewhere appears here, and no SDL/ImGui
type leaks into `zetta-core` (`src/gui/app.cpp` → `App::draw_projects`). This is
the lifecycle panel only; the full **New Project** screen (ingestion + use-case
selection, SCR-2 #223) is separate work.

## Graph Workspace

The **Graph Workspace** (SCR-3 / CMP-1, the GUI slice of FEAT-2) renders the
selected project's `ProjectGraph` as a visible, editable **node/edge canvas**
rather than a black box, and persists edits back to the IR. It is a collapsing
header in the headed window (`src/gui/app.cpp` → `App::draw_graph_workspace`),
driven by the same `selected_project_` the Projects column sets.

- **Pick / create / delete a graph.** It lists the project's `graph_names()`;
  selecting one opens it, `New graph` creates an empty one via `create_graph`,
  and the `x` button drops one via `drop_graph`.
- **Canvas** (`App::draw_graph_canvas`). Nodes are boxes laid out in **columns by
  `StepKind`** (connect → clean → model → index → serve → act, the canonical
  order), with edges drawn between them — `data_flow` and `control` in different
  colours, each with an arrowhead at the `to` end so direction reads even when an
  edge points backward. Clicking a box selects that step.
- **Edit** (`App::draw_graph_editor`). Add a step (name + `StepKind`), add an
  edge (from/to pickers + `EdgeKind`), delete any edge, and rename or delete the
  selected step — driving the IR's `add_node` / `add_edge` / `remove_edge` /
  `remove_node` (see [execution-graph.md](execution-graph.md#editing-a-graph-remove)).
- **Working copy + Save/Revert.** Edits apply to an in-memory *working copy* of
  the graph, not the stored IR, so an edit is deliberate and atomic. **Save**
  writes it back through [`Project::update_graph`](#execution-graphs) (disk + the
  `graph:<uuid>` CRDT key, so the edit replicates), optionally renaming the graph;
  **Revert** reloads the stored copy. The amber `● unsaved edits` marker shows
  when the working copy differs from what's stored. Selecting a *different* graph
  (or switching projects) discards the working copy.
- **Cluster plan** (`App::draw_graph_plan`, #591). A toggle (on by default) that
  projects how the open graph would run over the **current** cluster — which steps
  run near their data, which fan out SPMD, and the owner nodes each lands on — as a
  canvas overlay (a placement-coloured stripe + badge per step), a current-cluster
  strip (the selected step's nodes highlighted), and a per-step placement table. A
  *projection* of zetta's SPMD + compute-near-data model, not a committed schedule.
  See [graph-plan.md](graph-plan.md).

The panel keeps zetta's GUI rule: it is a thin renderer over the core API and no
SDL/ImGui type leaks into `zetta-core` (the plan projection is the pure
zetta-core `project_cluster_plan`). Compile-from-intent (producing a graph to
edit here) and the correctness gate are separate slices.

## Tests

`tests/pipeline/test_project.cpp` covers create/find/rename/drop (+ owner + stamps; a drop leaves no `input:`/`graph:` key behind and keeps a labelled run, #1009),
survival across a restart (both disk-only and CRDT-only), and replication of a
project + a rename to a connected peer. `tests/pipeline/test_project_graph.cpp` does the
same for a project's execution graphs: create/find/drop, **`update_graph`**
(content edits + rename guards, and that an edit re-captures the `graph:` key and
survives restart via the CRDT), restart via disk and via the CRDT, and
replication of a graph to a peer (with a faithful round-trip checked each time).
Graph (de)serialization and the in-place `remove_node` / `remove_edge` edits are
covered in `tests/pipeline/test_execution_graph.cpp`. The `delete_project` capability's journey
(delete → read back, runs/models kept, missing → `not_found`, refused while a run is in
flight) is in `tests/api/test_registry.cpp`.
`list_projects`' summaries (#1185) are checked against `get_project` field by field
(`RegistryFixture.ListProjectsReturnsGraphSummariesMatchingGetProject`), and their
access rule in `tests/api/test_scoped_grants_api.cpp`
(`ScopedApiFixture.ListProjectsSummariesNeedGetProjectsRight`).
