# Dataset view panel

The headed app's standalone **Datasets view** (`App::draw_datasets` +
`App::draw_dataset_placement`, `src/gui/app_datasets.cpp`) — a dedicated,
top-level screen reached from **Data → Datasets**. It is the GUI form of
[`system.datasets`](../data/system-datasets.md): one place to browse every dataset the
cluster holds, inspect its schema / row count / on-disk size / storage format, and
— the headline — **where its partitions physically sit across the cluster** (which
node owns which slice). It is the area-3 *dataset view* of the cluster-observability
epic [#575](https://github.com/oramasearch/zetta/issues/575), built on the
[`Node::datasets()`](../data/system-datasets.md) engine source from
[#585](https://github.com/oramasearch/zetta/issues/585).

A **dataset == a table together with the data it holds**. The
[Explorer](../data/query.md) already lets you walk databases → tables → rows; the Datasets
view adds the **data-first observability angle**: schema and size at a glance, the
[replication strategy](../data/replication-strategy.md), and the partition placement the
Explorer never surfaces.

## Thin renderer, no GUI-only state

The panel is a **thin renderer over the system tables** — it adds **no stored
state**. The dataset summaries are recomputed every frame from
`node_.datasets()` (the same source the `system.datasets` SQL view reads, so the
two can never diverge), and the cluster membership it overlays placement onto is
read live from `cluster_.nodes()` (the same source the [Cluster view](../cluster/cluster.md)
reads). The only UI state is `selected_dataset_` — the table id of the selected
row, re-resolved against the live datasets each frame (so a dropped table can't
leave a dangling selection), exactly like `selected_model_` / `selected_run_`.

## Add dataset

The panel also hosts the **"Add dataset" affordance** (#587): an **"Add dataset…"**
button at the top opens the native file picker filtered to
`parquet`/`jsonl`/`json`/`csv`, and a file **dropped onto this view** does the same
— both load the file as a **partitioned table** that joins the list with its
partitions placed across the cluster. The same action is reachable from
**File → Add dataset…**. The engine work is `zetta::data::AsyncDatasetAdd` (#624 —
decode on a worker, materialize in owner-thread slices): the panel shows live
progress (marquee + row count while decoding, a real fraction while adding
rows) with a **Cancel**, and the completion status line ("Added dataset … (N
rows, M partitions)") shows in place. See
[add-dataset.md](../data/add-dataset.md) and
[long-running-actions.md](long-running-actions.md).

## Layout

Rendered through the [master-detail scaffold](master-detail.md) (#634), like the
[Runs](../pipeline/runs.md#gui-panel), [Models](models-panel.md), and [Jobs](jobs-panel.md)
panels.

### Dataset list (left)

One row per `node_.datasets()` entry — a standard `MasterRow`: a muted dot (no
lifecycle), the dataset **name** (= the table name) as the title, and
`format · database` as the mono meta so same-named tables across databases stay
distinguishable. Selecting a row sets the active dataset (held by its table id).
A fresh node with no user tables shows "(no datasets)".

### Detail (right)

For the selected dataset:

- **Dataset** — name, database, and the full `table id` (a bare `tbl` uuid that
  joins to `system.dataset_partitions.table`).
- **Shape** — `format`, `replication` (the strategy name), `rows`, `size` (the
  on-disk bytes of the row chunks, via `format_bytes`), and the column count.
- **Schema** — the inferred field names, comma-joined in first-seen order (the
  engine is schemaless, so a dataset's schema is whatever its rows carry).
- **Partition placement across the cluster** — the overlay below.

## Partition placement across the cluster

`App::draw_dataset_placement` is the signature surface — it answers *which node
owns which partition* two ways:

- **A non-partitioned (`replicate_to_all`) dataset** has no partition-catalog
  entries — it lives in full on every node — so the panel says so plainly and
  shows nothing further.
- **A partitioned dataset** gets a **node-box overlay** reusing the
  [Cluster view](../cluster/cluster.md)'s spatial grammar (the same white-card / coral-accent
  box constants and short-id helper): one box per node in a wrapping grid. A node
  that **owns a partition of this dataset** is highlighted (coral wash + border,
  the owner accent); one that owns none is the plain card. Each box caption is the
  share that node holds — the partition count and the covered row total — so the
  placement reads at a glance. The **node universe** is the union of live cluster
  membership *and* every owner the catalog names, so a partition still owned by a
  node that has since left (tagged `(gone)`) or a foreign owner not yet connected
  still appears: the placement is the catalog's **global truth**, not just this
  node's local view (the catalog is replicate-to-all, so every node renders the
  same placement). The overlay is **read-only** — no hit-test, no selection.

Below the overlay, a **per-partition table** lists the literal catalog rows behind
it — each partition's short id, its `[start, end)` row range, and its owner nodes —
in row-range order. Each owner is an [entity link](cross-entity-links.md) (#633):
one click opens the Cluster view with that node's box selected. An unplaced
partition (empty `owner_nodes`) reads `(unplaced)`.

## Boundary validation

The panel introduces **no new JSON format** and **no new schema**. It reads the
already-validated in-memory handles `Node::datasets()` aggregates — the table rows
(stored by the storage engine) and the
[partition catalog](../data/dataset-partitions.md) (validated at the `DatasetPartition`
schema boundary when each entry entered from disk or the CRDT) — and only projects
them into the view. There is no untrusted payload entering here, so there is
nothing new to validate (the same reasoning as `system.datasets` itself).

## Where it fits

The Datasets view is **area 3 of #575**, the cluster-observability epic. It sits
alongside the read-only system-table viewers — [Models](models-panel.md),
[Compute jobs](jobs-panel.md), and the [Cluster view](../cluster/cluster.md) whose node-box
grammar it borrows for the placement overlay. Natural next slices: clicking a node
box to filter the partition table to that node's slices, surfacing
[per-node telemetry](../cluster/node-metrics.md) on the boxes, and a partition-detail drill-in
(the blob id / stats each catalog entry carries).

## Pointers

- `src/gui/app_datasets.cpp` — `App::draw_datasets` + `App::draw_dataset_placement`,
  reusing the shared canvas node-box palette (`kCanvasBox*`, `app_internal.hpp`)
  and `entity_short_id` from `app_operations_internal.hpp`, plus the
  "Add dataset…" button.
- `src/gui/app.cpp` — the `Data → Datasets` menu item and the `View::Datasets`
  switch arm; the `File → Add dataset…` item, the `FileIntent::Dataset` ingestion
  branch, and the drop-on-Datasets-view routing.
- [add-dataset.md](../data/add-dataset.md) — the `zetta::data::add_dataset_file` engine backend +
  the "Add dataset" affordance this panel hosts.
- `src/gui/app.hpp` — the `View::Datasets` enum value, the method declarations, and
  the `selected_dataset_` selection member.
- [system-datasets.md](../data/system-datasets.md) — the `Node::datasets()` engine source +
  the `system.datasets` SQL view the panel renders.
- [dataset-partitions.md](../data/dataset-partitions.md) — the `system.dataset_partitions`
  catalog the placement overlay joins.
- [cluster.md](../cluster/cluster.md) — the sibling spatial node-box view this overlay borrows.
- [cross-entity-links.md](cross-entity-links.md) — the partition owners are one-click entity links into the Cluster view (#633).
