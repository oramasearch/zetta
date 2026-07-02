# Graph-over-cluster plan view

The **graph-over-cluster plan view** (#591, part of [#575](https://github.com/oramasearch/zetta/issues/575)
area 6) extends the headed **Graph Workspace**
([`App::draw_graph_canvas`](../../src/gui/app_pipeline.cpp)) from a graph *editor*
into a cluster-aware *plan view*: opening a graph shows a **projection of how that
graph would run over the current cluster** — which steps run **near their data**,
which **fan out** across the cluster, and which **nodes** each lands on.

It builds on the spatial [Cluster topology view](../cluster/cluster.md) (#582) — the same
selectable node-box grammar, the same live `Cluster::nodes()` source — and reads
the [partition catalog](../data/dataset-partitions.md) (`system.dataset_partitions`) for
data placement. It is a **thin renderer** over the IR + those system tables and
adds **no new JSON boundary** (the IR is already validated by `ir::from_json`, the
owner sets come from the schema-validated partition catalog).

## It is a projection, not a schedule

zetta has **no general per-step scheduler yet** — R1 deploy places a graph onto
the local node, and the native compute pass runs **SPMD** (every node runs the
same graph against its shard, partials combine over [collectives](../cluster/collectives.md),
each node computing [near its owned partitions](../compute/compute-near-data.md)). So this
view does **not** report a committed placement; it **projects** zetta's documented
execution model onto the cluster *as it stands right now*. The UI says so, and the
projection is rebuilt every frame, so it tracks graph edits and cluster
joins/leaves live.

## The placement model

The projection is the pure, deterministic, non-throwing
`zetta::project_cluster_plan` (`include/zetta/pipeline/graph_plan.hpp`,
`src/core/pipeline/graph_plan.cpp`) — in **zetta-core**, so it is headless-testable
(`tests/pipeline/test_graph_plan.cpp`) and the GUI stays a thin renderer. It takes the
[ExecutionGraph IR](execution-graph.md), the cluster `members`, and `direct_owners`
(a node id → the owner nodes of the partitioned table it directly reads) and
returns a `ClusterPlan` — one `StepPlan` per graph node, in insertion order.

Each step is assigned one of four `StepPlacement` classes:

| Placement | Meaning | Targets |
|---|---|---|
| **near-data** | runs on the partition **owner nodes**, near the data it reads (compute-near-data; no full-dataset pull) | the table's owner nodes |
| **fan-out** | **SPMD** across every node; partials combine over collectives | all members |
| **all nodes** | a **load** whose result is replicated to every node | all members |
| **coordinator** | runs once on the **coordinator** (a single-node side effect) | the lexicographically-smallest node (the [collectives root](../cluster/collectives.md)) |

The rules, by [step kind](execution-graph.md):

- **connect** → **near-data** when it reads a partitioned source (its owners),
  else **all nodes** (a replicated load).
- **clean / model / index / serve** → **near-data** when a partitioned source
  flows in (its owners; partials still combine cluster-wide), else **fan-out**
  (SPMD across every node).
- **act** → **coordinator** (a single-node side effect — e.g. the churn
  retention-playbook step that notifies / writes back once).

**Near-data locality propagates forward along `DataFlow` edges.** A step inherits
the **union** of every partitioned source upstream of it, so a downstream compute
step runs on the owners of the data flowing into it (a `featurize` that joins two
partitioned sources runs on the union of both sources' owners). Control edges carry
no data, so locality does **not** propagate along them. A catalog owner that has
left the cluster is dropped — a stale owner never appears as a target, and if every
owner of a source is gone the step degrades to its no-locality default.

### How the GUI feeds it

`App::build_graph_plan` (`src/gui/app_pipeline.cpp`) resolves the inputs from the
live node each frame:

- **members** — `Cluster::nodes()` (the same source as `system.zetta_nodes` and the
  Cluster view).
- **owners** — `Node::dataset_partitions()` (the source of
  `system.dataset_partitions`), grouped by table id into the per-table owner-node
  union.
- **direct_owners** — for each step carrying a `"table"` attribute (a table name
  *or* a `tbl:` id), resolve it against the node's databases to a table id and, if
  that table is in the partition catalog, seed the step with its owners.

A step without a `"table"` attribute (or one naming a non-partitioned table) has no
direct locality, so it falls to its kind's default — which is why a fresh graph
over a cluster with no partitioned tables projects as *connect = all nodes, compute
= fan-out, act = coordinator*.

## The GUI

A **Cluster plan** checkbox sits above the canvas (on by default, so opening a
graph shows the projection; uncheck it for a plain edit canvas). When on:

- **Canvas overlay** — each step box gets a coloured **left stripe** in its
  placement colour and a mono **badge** (`near-data` / `fan-out` / `all nodes` /
  `coordinator`) under its name, so the graph reads as "coloured by where it runs".
- **Cluster strip** — the current cluster's node boxes (the Cluster view's
  grammar). Selecting a step (in the canvas or the table) gives the nodes it runs
  on the coral **"runs here"** treatment — the direct graph↔cluster link.
- **Per-step table** — `Step · Stage · Placement · Runs on`, one row per step.
  Clicking a row selects that step; hovering it shows the projection's rationale.

Placement colours follow the Orama [design system](../gui/design-system.md) — semantic,
never coral (coral stays the "you are here" / "runs here" accent): **green** for
near-data (compute where the data lives — the efficient case), **ink** for fan-out
(SPMD, distributed), **quiet** for an all-nodes load, **amber** for the coordinator
(a single-node funnel point).

## Scope

Storage + projection + rendering only. There is no per-step scheduler to *act* on
this plan (deploy is still local in R1; the compute pass is SPMD), so the view is
read-only and informational. A future scheduler that places arbitrary graph steps
onto specific nodes — and a `system.<table>` read view of a committed placement —
would replace this projection with the real thing behind the same shape.
