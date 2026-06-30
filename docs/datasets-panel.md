# Dataset view panel

The headed app's standalone **Datasets view** (`App::draw_datasets` +
`App::draw_dataset_placement`, `src/gui/app_operations.cpp`) — a dedicated,
top-level screen reached from **Data → Datasets**. It is the GUI form of
[`system.datasets`](system-datasets.md): one place to browse every dataset the
cluster holds, inspect its schema / row count / on-disk size / storage format, and
— the headline — **where its partitions physically sit across the cluster** (which
node owns which slice). It is the area-3 *dataset view* of the cluster-observability
epic [#575](https://github.com/oramasearch/zetta/issues/575), built on the
[`Node::datasets()`](system-datasets.md) engine source from
[#585](https://github.com/oramasearch/zetta/issues/585).

A **dataset == a table together with the data it holds**. The
[Explorer](query.md) already lets you walk databases → tables → rows; the Datasets
view adds the **data-first observability angle**: schema and size at a glance, the
[replication strategy](replication-strategy.md), and the partition placement the
Explorer never surfaces.

## Thin renderer, no GUI-only state

The panel is a **thin renderer over the system tables** — it adds **no stored
state**. The dataset summaries are recomputed every frame from
`node_.datasets()` (the same source the `system.datasets` SQL view reads, so the
two can never diverge), and the cluster membership it overlays placement onto is
read live from `cluster_.nodes()` (the same source the [Cluster view](cluster.md)
reads). The only UI state is `selected_dataset_` — the table id of the selected
row, re-resolved against the live datasets each frame (so a dropped table can't
leave a dangling selection), exactly like `selected_model_` / `selected_run_`.

## Layout

A list + detail in the same "select on the left, inspect on the right" shape the
[Runs](runs.md#gui-panel), [Models](models-panel.md), and [Jobs](jobs-panel.md)
panels use.

### Dataset list (left)

One row per `node_.datasets()` entry. Each row is the on-disk **format** tag
(`jsonl` today) plus the dataset **name** (= the table name), with the owning
**database** as a muted sub-line so same-named tables across databases stay
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
  [Cluster view](cluster.md)'s spatial grammar (the same white-card / coral-accent
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
in row-range order. An unplaced partition (empty `owner_nodes`) reads `(unplaced)`.

## Boundary validation

The panel introduces **no new JSON format** and **no new schema**. It reads the
already-validated in-memory handles `Node::datasets()` aggregates — the table rows
(stored by the storage engine) and the
[partition catalog](dataset-partitions.md) (validated at the `DatasetPartition`
schema boundary when each entry entered from disk or the CRDT) — and only projects
them into the view. There is no untrusted payload entering here, so there is
nothing new to validate (the same reasoning as `system.datasets` itself).

## Where it fits

The Datasets view is **area 3 of #575**, the cluster-observability epic. It sits
alongside the read-only system-table viewers — [Models](models-panel.md),
[Compute jobs](jobs-panel.md), and the [Cluster view](cluster.md) whose node-box
grammar it borrows for the placement overlay. Natural next slices: clicking a node
box to filter the partition table to that node's slices, surfacing
[per-node telemetry](node-metrics.md) on the boxes, and a partition-detail drill-in
(the blob id / stats each catalog entry carries).

## Pointers

- `src/gui/app_operations.cpp` — `App::draw_datasets` + `App::draw_dataset_placement`,
  reusing the file-local cluster node-box constants (`kClusterBox*`) and
  `cluster_node_short_id` the [Cluster view](cluster.md) defines.
- `src/gui/app.cpp` — the `Data → Datasets` menu item and the `View::Datasets`
  switch arm.
- `src/gui/app.hpp` — the `View::Datasets` enum value, the method declarations, and
  the `selected_dataset_` selection member.
- [system-datasets.md](system-datasets.md) — the `Node::datasets()` engine source +
  the `system.datasets` SQL view the panel renders.
- [dataset-partitions.md](dataset-partitions.md) — the `system.dataset_partitions`
  catalog the placement overlay joins.
- [cluster.md](cluster.md) — the sibling spatial node-box view this overlay borrows.
