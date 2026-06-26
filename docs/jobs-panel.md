# Compute Jobs panel

The headed app's **compute view** (`App::draw_jobs`, `src/gui/app.cpp`) — the GUI
slice of the native distributed accelerated-compute pass for UC-1 churn (FEAT-5 /
SCR). It surfaces two things an operator wants while a job runs, neither of which
had a GUI before:

- **live compute jobs** — the GUI form of the
  [`system.zetta_jobs`](query.md) registry: every
  [`ComputeJob`](compute-jobs.md) the node holds, with its lifecycle status
  (queued → running → succeeded | failed);
- **per-job objective/scaling controls** — set the reward objective + bounds and
  add/remove nodes, scored live against the built
  [`ScalingPolicy::evaluate()`](scaling-policy.md).

The third strand of the same FEAT-5 slice — the trained-**Model** + metrics +
**ranked drivers** — lands in the
[Runs panel](runs.md#trained-models--metrics--ranked-drivers) instead, where a
model is naturally read alongside the run that produced it.

Like the [Runs](runs.md#gui-panel) and [Admin](admin-panel.md) panels, the job
list is **read-only**: it renders whatever jobs the node holds (created via the
API / tests, or replicated from a peer), so a fresh standalone node shows
"(no jobs)" until something creates one. Creating / scheduling a job — the
coordinator, scheduler, and live autoscaler — are later slices.

## Layout

One collapsing header, **Compute Jobs**, in the list + detail shape the Runs panel
uses ("select on the left, inspect on the right").

### Job list (left)

One row per `node_.jobs()` entry. Jobs have **no human name**, so each row is a
**color-coded status badge** (queued neutral, running amber, succeeded green,
failed red — the same palette as the cluster + runs panels) plus the job **kind**
(`featurize` / `train` / `explain` / `score` / `ddf`) and the first segment of the
job's UUID. Selecting a row sets the active job (held by id and re-resolved each
frame via `node_.find_job`, like the rest of the UI, so a dropped job can't leave a
dangling handle).

### Detail (right)

For the selected job:

- **identity + status** — the full id, the color-coded status, the `kind`,
  `created` / `updated` stamps, and the `detail` note (a failure reason surfaces
  here);
- **references** — the job's place in the pipeline as id refs, each shown only when
  set: `project` / `graph` / `run` (the produced run) / `scaling_policy` (the reward
  model id);
- **objective & scaling controls** (below).

## Objective & scaling controls (preview)

`App::draw_job_scaling_controls` is the per-job **objective/scaling** surface. It is
a **live preview**, not a write path: a [`ScalingPolicy`](scaling-policy.md) is not
persisted yet and the autoscaler is unbuilt (both later slices), so the controls
score an editable policy + sample against the **real, pure**
`ScalingPolicy::evaluate()` and show the decision. Nothing here mutates the job —
it is a "what-if" calculator that exercises the shipped reward logic.

The controls, top to bottom:

1. **Reward objective** — a combo over the three
   [`ScalingObjective`](scaling-policy.md)s (`minimize_cost` / `runtime_target` /
   `data_shape`). This is "set the reward".
2. **Bounds** — the hard envelope: `min_nodes` (≥ 1), `max_nodes` (≥ min), and a
   `cost_cap` (0 = unlimited). `target_seconds` appears only for the
   `runtime_target` objective (a runtime target is meaningless without one).
3. **Sample** — the live signals the autoscaler scores: **allocated nodes** with
   `- node` / `+ node` buttons (this is "add/remove nodes"), plus `progress`
   (slider, [0, 1]), `elapsed seconds`, and `accrued cost`.
4. **Decision** — the output of `policy.evaluate(sample)`:
   - **recommended nodes** + the **scale up / hold / scale down** action
     (color-coded: up green, hold neutral, down amber), relative to the sample's
     current allocation;
   - **reward** (higher is better — the autoscaler maximizes it);
   - a **cost-cap breached** flag (red) when accrued cost exceeds a set cap (the
     hard constraint sheds the fleet to `min_nodes`);
   - the human-readable **reason**.

The controls reset to sensible defaults whenever the selected job changes
(`scaling_loaded_job_` tracks which job the in-flight edits belong to), so they
never leak one job's experiment onto another.

## Boundary validation

The panel reads **already-validated** in-memory handles — `ComputeJob` (validated by
`parse_compute_job_value` when it entered from disk or the CRDT) and `Model` — and
builds a `ScalingPolicy` in memory whose own JSON boundary
(`ScalingPolicy::from_json`, mirroring `schemas/scaling_policy.schema.json`) already
exists. So this slice introduces **no new JSON format and no new boundary**, the
same way the [Runs](runs.md#gui-panel) and [Admin](admin-panel.md#boundary-validation)
panels reuse their entities' schemas.

## Where it fits

This is the **compute view** slice of FEAT-5's GUI. It sits alongside the existing
read-only panels (Cluster, Runs, Admin) and depends on two shipped subsystems: the
[ComputeJob registry](compute-jobs.md) (`system.zetta_jobs`) and the
[ScalingPolicy reward model](scaling-policy.md). The natural next slices are the
**mutating** compute actions — creating + scheduling a job (the coordinator), and
persisting a `ScalingPolicy` so the controls drive a live autoscaler instead of
previewing it.

## Pointers

- `src/gui/app.cpp` — `App::draw_jobs` + `App::draw_job_scaling_controls` +
  `App::reset_scaling_controls`, and the file-local `job_status_color` /
  `scaling_action_color` view helpers.
- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` entity + `system.zetta_jobs`
  registry the job list renders.
- [scaling-policy.md](scaling-policy.md) — the `ScalingPolicy` reward model + the
  `evaluate()` the controls preview.
- [runs.md](runs.md#trained-models--metrics--ranked-drivers) — the sibling FEAT-5
  slice: the trained Model + metrics + ranked drivers in the Runs panel.
