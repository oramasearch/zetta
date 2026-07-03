# Models panel

The headed app's standalone **Models view** (`App::draw_models`,
`src/gui/app_models.cpp`) — a dedicated, top-level screen for the
trained-[`Model`](../ml/models.md) registry, reached from **Operations → Models**. It is
the GUI form of [`system.models`](../data/query.md): one place to browse every model
the node holds, inspect its metrics / ranked drivers / weights fingerprint, and
**register or import** a new one.

Until this view, models had a GUI presence only **inside** a run — the
[Runs panel](../pipeline/runs.md#trained-models--metrics--ranked-drivers)'s "Trained models"
section, where a model is read alongside the run that produced it. That stays. The
Models view adds the **model-first** angle: models that arrive by replication, via
the API, or hand-registered have no single run to hang off, and an operator wants
to see *all* of them at once. The detail rendering is **shared** — both panels call
the same `render_model_card`, so the metrics / drivers / weights surface is
identical wherever a model appears.

## Layout

Rendered through the [master-detail scaffold](master-detail.md) (#634), like the
[Runs](../pipeline/runs.md#gui-panel) and [Jobs](jobs-panel.md) panels, with an inline
**Add a model** affordance above it.

### Model list (left)

One row per `node_.models()` entry — a standard `MasterRow`: a muted dot
(models have **no lifecycle status**, unlike a run or job), the model's **name**
— or the first segment of its UUID when unnamed — as the title, and the **kind**
(`classifier` / `regressor` / …) as the mono meta. Selecting a row
sets the active model (held by id and re-resolved each frame via `node_.find_model`,
like the rest of the UI, so a dropped/replaced model can't leave a dangling
handle). A fresh standalone node shows "(no models)" until something registers one.

### Detail (right)

For the selected model, `render_model_card` (reused from the Runs panel) renders:

- **identity + weights** — the model's name / kind / short id, and its **weights
  fingerprint** (`size`, flagged when the body is still in flight to this node, or
  "(not yet attached)" before any weights are set);
- **Metrics** — the evaluation scores (`metrics`, a flat `name → number` map) in a
  `Metric / Value` table; a value in `[0, 1]` is drawn as a bar so a score reads at
  a glance;
- **Ranked drivers** — the feature importances, **strongest |influence| first**,
  each a normalized bar with its signed value, read tolerantly from the model's
  free-form `feature_schema.drivers` via
  [`zetta::ml::ranked_drivers`](../ml/models.md#ranked-drivers-explainability).

Below the card, a quiet mono strip adds the `created` stamp and the model's lineage
refs (`project` / `run`) when set.

## Add a model

`App::draw_add_model_form` is the inline **register / import** affordance — a
collapsing header (closed by default, so the list stays the focus) that wires the
GUI to [`Node::create_model`](../ml/models.md#lifecycle-on-a-node) (+ optional
`Model::set_weights`). The fields:

1. **Name** (optional) — a display label.
2. **Kind** (required, default `classifier`) — e.g. `classifier` / `regressor`.
3. **Metrics** (optional JSON) — an object of `name → number`, e.g.
   `{"auc": 0.91, "f1": 0.85}`. Surfaces in the detail pane's Metrics table.
4. **Feature schema** (optional JSON) — a free-form object. A
   `"drivers": [{ "feature", "importance" }]` array inside it shows as the detail
   pane's ranked drivers.
5. **Weights** (optional file) — chosen via the native file dialog ("Choose weights
   file…") or dropped on the window (routed to `FileIntent::ModelWeights` and
   *staged*, not ingested). On **Register** the bytes are read and attached via
   `set_weights`, which writes the body to disk, fingerprints it (size + SHA-256),
   and replicates the metadata; the body then transfers peer-to-peer over the
   [large-object body seam](../data/large-objects.md). The bytes are opaque — no validation.

**Register model** runs the create atomically: it appears in the list immediately
(and in `system.models`), with `selected_model_` set to it. **Clear** resets
the form.

## Boundary validation

The panel introduces **no new JSON format**. The list/detail read **already-validated**
`Model` handles (`parse_model_value` ran when each entered from disk or the CRDT),
and the **Add model** form re-validates at *its* boundary: the pasted `metrics` /
`feature_schema` text is parsed (a parse failure or a non-object is rejected inline),
then the assembled `ModelValue` is checked against the **Model JSON Schema** via
[`validate_model_value`](../ml/models.md#boundary-validation-json-schema) — the same gate
the record must pass on the next reconcile — before `create_model` runs. So a
malformed metric (e.g. a non-numeric value, which the schema forbids) fails fast in
the form with a clear reason instead of being persisted and rejected on reload. This
reuses the existing Model schema; it adds no second schema.

## Where it fits

The standalone **Models view** is part of area 4 of #575. It sits alongside the
read-only Operations panels (Runs, Compute Jobs, Cluster) and complements — does not
replace — the Runs panel's "Trained models" section. The natural next slices are
**mutating** model actions the entity already supports but the view doesn't expose
yet: replacing weights / updating metrics on an existing model (`set_weights` /
`set_metrics`), and a delete affordance (`drop_model`).

## Pointers

- `src/gui/app_models.cpp` — `App::draw_models` + `App::draw_add_model_form` +
  `App::reset_add_model_form`, and the shared file-local `render_model_card` view
  helper (also used by `App::draw_runs`).
- `src/gui/app.cpp` — the `Operations → Models` menu item, the `View::Models`
  switch arm, the `on_open_model_weights` file-dialog callback, and the
  `FileIntent::ModelWeights` staging branch in `handle_incoming_files`.
- [models.md](../ml/models.md) — the `Model` entity (lifecycle, schema, weights body,
  `system.models` registry) the view renders.
- [runs.md](../pipeline/runs.md#trained-models--metrics--ranked-drivers) — the sibling surface:
  the same `render_model_card` inside a run's detail pane.
- [jobs-panel.md](jobs-panel.md) — the sibling read-only Operations panel this
  mirrors in shape.
