# Models

A **Model** (`zetta::ml::Model`) is a trained model registered on the node: its
metadata (kind, feature schema, evaluation metrics) plus a fingerprint of its
serialized **weights**. It realizes **ENT-14 / FEAT-6** and is part of the
*native distributed accelerated compute* pass for UC-1 churn (#264).

A model is split the same way a [large object](../data/large-objects.md) is, and for the
same reason: the **metadata replicates** in the CRDT document (flat key
`model:<uuid>`) so every node agrees on what models exist and what they are,
while the **serialized weights body** — a large binary that would bloat every
node's document — stays **off the CRDT**. It lives on disk and transfers
peer-to-peer over the existing large-object body seam, verified against the
size + SHA-256 recorded in the metadata.

This page documents the **storage + data model**: the `Model` type, its
lifecycle (create / attach weights / query / drop) on a `Node`, how it
replicates, and the `system.models` registry table.

## The type

A `Model` (`include/zetta/ml/model.hpp`) is a top-level entity with no parent,
modeled exactly like [`zetta::pipeline::Run`](../pipeline/runs.md) — keyed by id (models have no
human-name lookup; the registry is `Node::models()` + the system table). Its
value (`ModelValue`):

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `name` | `std::string` | Optional human display label. |
| `kind` | `std::string` | **Required, non-empty** — e.g. `classifier`, `regressor`. |
| `project` | `std::string` | Optional producing/owning project uuid (lineage). |
| `run` | `std::string` | Optional producing run uuid (lineage). |
| `feature_schema` | `nlohmann::json` | Free-form object: input features (field → type / profile). |
| `metrics` | `nlohmann::json` | Free-form object: evaluation scores (name → **number**, e.g. `{"auc": 0.91}`). |
| `created` | `std::string` | ISO-8601 UTC stamp at creation. |
| `weights` | `optional<ModelWeights>` | `{ size, sha256 }` — set once a weights body is attached. |
| `exposed` | `bool` | Governed serving state ([#648](https://github.com/oramasearch/zetta/issues/648), [../api/serving.md](../api/serving.md)): true publishes `/v1/models/{name}/predict` on every node. Default false; emitted in JSON only when set, so legacy records are unchanged. `Model::set_exposed` flips it (captured into the CRDT — the whole cluster converges). |

`ModelWeights` is *just the fingerprint* — `size` (bytes) + `sha256`
(lowercase-hex). The bytes themselves are never in this value; they live on disk
(see below). On a peer that has the model metadata but not yet its body,
`weights` is present but `has_weights_body()` is false until the transfer lands.

## Lifecycle (on a Node)

`Node` owns models like it owns [runs](../pipeline/runs.md):

```cpp
// Create: requires a non-empty `kind`; stamps `created`. The caller fills the
// rest of the ModelValue (name / feature_schema / metrics / project / run).
zetta::ml::ModelValue spec;
spec.kind = "classifier";
spec.name = "churn-v1";
spec.feature_schema = {{"tenure_months", "number"}, {"plan", "string"}};
spec.metrics = {{"auc", 0.91}};
auto model = node.create_model(spec);           // empty handle if kind is empty

// Attach (or replace) the serialized weights. The body is written to disk,
// fingerprinted (size + SHA-256), captured into the CRDT, and model.json rewritten.
model->set_weights(serialized_bytes);

// Update the evaluation metrics later (e.g. after an eval pass).
model->set_metrics({{"auc", 0.93}, {"f1", 0.85}});

auto found  = node.find_model(model->id());     // by id; empty handle on miss
auto all    = node.models();                    // the registry (created, then id)
bool gone   = node.drop_model(model->id());     // remove from CRDT + disk
```

The mutators (`set_weights`, `set_metrics`) capture `model:<id>` into the CRDT
(when replicated) then rewrite `model.json` — the CRDT is the source of truth,
disk is its projection. Reconciliation uses `apply_value()`, which updates the
metadata projection from the document with **no capture** and **without touching
the weights body** (the body arrives separately over the body seam).

## On disk

Each model is a folder named by its UUID directly under `data_dir` (a top-level
entity, like a run), holding the metadata projection and the raw weights:

```
data_dir/
  <model-uuid>/
    model.json   { id, name, kind, project, run, feature_schema, metrics,
                   created, weights: { size, sha256 } }
    weights      raw serialized bytes (NOT in the CRDT; like a large-object body)
```

`model.json` is a projection of the replicated `model:<uuid>` value (plus the
`id`); standalone nodes read/write it directly. The `weights` file is the body —
absent on a peer until body transfer completes.

## Replication

Following zetta's **"everything is replicated state"** rule, the model metadata
goes through `ReplicationManager` like every other entity:

- **Metadata** (`model:<uuid>` → the value above) rides in the Automerge CRDT and
  gossips between peers; `Node` captures local creates/edits and reconciles peer
  changes into the on-disk projection on each `tick()`.
- **Weights body** does **not** ride in the document. It moves over the same
  large-object body seam as `lo:` bodies: the cluster's `LOREQ`/`LORES` exchange
  through the `BlobStore`. `ReplicatedBlobStore` serves and stores model weights
  alongside large objects — it enumerates any `model:` entry carrying a `weights`
  descriptor whose body file is missing, requests it from a peer, and **verifies
  the received bytes against `weights.size` + `weights.sha256`** before writing
  them to `<data_dir>/<model-uuid>/weights`. A mismatch is dropped, never stored.

So a model created on one node converges everywhere: the metadata first (fast,
in the CRDT), then the weights body (peer-to-peer, verified). See
[replication.md](../cluster/replication.md) and [large-objects.md](../data/large-objects.md) for
the underlying machinery.

## Boundary validation (JSON Schema)

`Model` is validated against a formal **JSON Schema** (draft-07) at the storage
boundary — the same discipline as the [Run record](../pipeline/runs.md): `kind` is required +
non-empty, `metrics` values must be numbers, `weights` (when present) must carry
both `size` and `sha256`, and no stray fields are allowed
(`additionalProperties: false`). Every value entering from disk (`model.json`) or
the CRDT (`model:<id>`) passes `parse_model_value`, so a malformed record is
rejected with a clear reason (logged + skipped on load) instead of corrupting the
in-memory tree. The schema is exposed via `model_value_schema()`;
`validate_model_value` / `parse_model_value` are the entry points (via
`pboettch/json-schema-validator`, linked PRIVATE to `zetta-core`).

## Ranked drivers (explainability)

A model can carry an optional **explainability** annotation — its **ranked feature
drivers** (the SHAP-style "which features drove the prediction, and how strongly").
Because it is an optional, free-form signal an `Explain`
[compute job](../compute/compute-jobs.md) (or any producer) records, it lives **inside the
already-free-form `feature_schema`** rather than as a new top-level field — so it
adds **no new JSON boundary** to the Model record:

```json
"feature_schema": {
  "tenure_months": "number",
  "drivers": [
    { "feature": "support_tickets",  "importance": -0.41 },
    { "feature": "monthly_charges",  "importance":  0.27 }
  ]
}
```

`zetta::ml::ranked_drivers(const nlohmann::json& feature_schema)` (and the convenience
`Model::drivers()`) reads that optional `drivers` array **defensively** — keeping
only well-shaped `{ feature: string, importance: number }` entries, silently
skipping anything else — and returns them as `ModelDriver{ feature, importance }`
sorted by **|importance| descending** (strongest influence first; the sign is kept,
since a feature can push a prediction either way). It is **tolerant reading of
trusted data, not a second validation gate**: `feature_schema` was already
schema-validated as an object by `parse_model_value` when the model entered. With
no `drivers` key (the common case) it returns an empty list. This is what the
standalone [Models panel](../gui/models-panel.md) and the
[Runs panel](../pipeline/runs.md#trained-models--metrics--ranked-drivers)'s "Trained models"
section both render (via the shared `render_model_card`).

## Integrity (preflight)

`Node::check_integrity()` (run on `start()` unless `--no-preflight`) extends to
model weights, mirroring large-object bodies: it counts models, and for each
model that has weights metadata it checks whether the body is present and
re-hashes a present body against the recorded `sha256`. A model whose body hasn't
arrived yet is **informational** (`model_weights_missing` — awaiting transfer); a
present body that no longer matches its digest is **corruption**
(`corrupt_model_weights`, which fails `IntegrityReport::ok()`).

## The registry — `system.models`

The model registry is surfaced as a read-only [system table](../data/query.md) (like
`system.deployments`), computed at query time from `Node::models()`:

```sql
SELECT * FROM system.models;
SELECT model_id, kind FROM system.models WHERE kind = 'classifier';
```

Columns: `model_id`, `name`, `kind`, `project`, `run`, `feature_schema`,
`metrics`, `created`, `has_weights`, `weights_size`, `weights_sha256`,
`weights_present`, `exposed`, `endpoint`. The serialized body itself is not in
the registry (it's a large binary on the body seam); its fingerprint and local
presence are, so the registry shows what is trained and whether its weights
have landed on this node. `exposed` / `endpoint` surface the governed serving
state ([../api/serving.md](../api/serving.md), #648): whether the model's HTTP
predict route is published, and at which path (`endpoint` is null while
unexposed).

## What this slice is (and isn't)

It is **storage + provenance + replication + registry** for a trained model: a
first-class, replicated entity whose weights ride the existing body-transfer
path. It is **not** training or inference — producing the serialized weights (and
wiring a model into a [run's](../pipeline/runs.md) outputs or the
[graph executor](../pipeline/execution.md)) is a separate slice. `set_weights` takes the
bytes the caller already has.

## Pointers

- [`include/zetta/ml/model.hpp`](../../include/zetta/ml/model.hpp) — the `Model` type +
  `ModelValue` + schema/validation entry points.
- [`src/core/ml/model.cpp`](../../src/core/ml/model.cpp) — value, mutators, weights body
  I/O, JSON-Schema boundary validation.
- [models-panel.md](../gui/models-panel.md) — the headed standalone **Models view**
  (`App::draw_models`): list + detail over this registry, plus the inline
  register/import affordance.
- [runs.md](../pipeline/runs.md) — the sibling top-level entity this is modeled on.
- [large-objects.md](../data/large-objects.md) — the body-transfer seam the weights reuse.
- [replication.md](../cluster/replication.md) — the CRDT data plane.
- [query.md](../data/query.md) — the system-table mechanism behind `system.models`.
