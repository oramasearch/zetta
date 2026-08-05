# Cross-entity navigation — the object graph made clickable (#633)

Every panel used to be a silo: a run's detail listed its model, jobs, graph,
and deployment as inert ids the user re-found by eye. This slice places the
`entity_link` widget throughout the object graph, so every cross-referenced
entity is one click from its owning panel. The mechanism itself (the
navigation plumbing) shipped earlier: `App::navigate_to(view, entity_id)` +
per-view pending selection + `take_pending_selection` (#627/#628, PR #655) —
see [architecture.md](architecture.md). This page records the wired edges and
the rules the links follow.

## The widgets

- `entity_link(label, view, id)` — the inline underline-on-hover link
  (`app_views.cpp`); clicking calls `navigate_to(view, id)`.
- `link_line(label, display, view, id)` — a detail pane's "label: ref" line
  where the ref is an `entity_link` (the quiet mono label + the clickable
  value). The standard form for `project:` / `graph:` / `run:` /
  `deploy:` reference lines.

**A link never fakes a destination.** Every caller re-resolves the reference
against the live node this frame (`find_run` / `find_model` /
`find_deployment` / `graph_is_stored` / `project_name_for`); a ref that
resolves to nothing renders as plain mono text. Two shared resolvers on `App`
back this: `project_name_for(ref)` maps a project uuid *or* name to the name
the Explorer selects by, and `graph_is_stored(ref)` checks whether any project
stores a graph under that id or saved name.

## The wired edges

| From | Link | To (view + selection) |
|---|---|---|
| Run detail | `project:` | Explorer, project selected |
| Run detail | `graph:` | Pipeline workspace, graph opened |
| Run detail | `deploy:` | Deploy, deployment row highlighted |
| Run detail | Compute jobs section (per job) | Jobs, job selected |
| Run detail | model output ref · "open in models" per trained-model card | Models, model selected |
| Model detail | `run:` (the producing run) | Runs, run selected |
| Model detail | `project:` | Explorer, project selected |
| Job detail | `run:` / `graph:` / `project:` | Runs / Graph / Explorer |
| Deployments table | graph name | Pipeline workspace, graph opened |
| Deployments table | latest run (newest `run.deployment == id`) | Runs, run selected |
| Dataset detail | partition owner nodes | Cluster, node box selected |
| Audit log | target, routed by action (job → Jobs, graph → Graph, deploy → Deploy) | the owning panel |
| Activity feed | targets (pre-existing #622, now carrying graph/deployment deep links) | the owning panel |
| Pipeline workspace | `project:` | Explorer, project selected |
| Pipeline workspace | "open in runs" / "open in models" (pre-existing #626) | Runs / Models |

## New pending-selection consumers

Master-detail panels already consumed their deep links through the scaffold.
This slice added the two non-scaffold consumers:

- **Pipeline workspace** (`draw_graph_workspace`): resolves the pending entity —
  a stored-graph id (a run's `graph`) or a saved name (a deployment's
  `graph_name`, an audit target) — across **every** project (the sender
  doesn't know the owner), selects that project, and opens the graph.
- **Deploy** (`draw_deploy`): the pending entity — a deployment id or a
  deployed graph's name — highlights its row in the deployments table with the
  accent "you are here" wash (`kAccentWash`, now a shared palette token in
  `app_internal.hpp`).

## Boundaries

- No new JSON format, schema, or engine surface — the links ride ids the
  (already schema-validated) entities carry; all resolution is per-frame reads
  of live `Node` state (the thin-renderer rule).
- The audit table's routing mirrors the activity feed's
  (`zetta::observability::entry_for`): key / data-access targets stay plain
  text because they already live on the Admin panel.
- Later slices: a deployment master-detail panel (today the table highlights,
  it has no detail pane), links for lineage source refs, and feed entries in
  Home reusing the same links (#622 follow-through).
