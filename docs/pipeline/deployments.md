# Deployments

A **deployment** places a validated [execution graph](execution-graph.md) onto a
**target** and records it. This is the first slice of *one-button deploy* (FEAT-4)
and the `DeploymentTarget` domain entity (ENT-6): the orchestrator's way of
saying *"this graph should run here"*.

This slice is **R1 local only** — a graph is placed onto the local node, and the
placement is recorded (replicated + on disk) and queryable. The reconcile/
self-heal loop that *acts* on a deployment — driving desired → actual and
restarting a killed/failed run — now exists as the **[deployment
controller](deployment-controller.md)**. Kubernetes / VPC / managed-cloud
targets and cross-node failover remain deliberately **later slices** (see
*What's deferred*). The headed **[Deploy panel](solution-catalog.md)** drives
this slice from the GUI: one-button deploy of an AI-solution blueprint to the
local target, gated by the [correctness gate](gate.md), with the recorded
placements shown back.

## The target — `DeploymentTarget`

A `DeploymentTarget` (`include/zetta/pipeline/deployment_target.hpp`) is the placement the
orchestrator deploys onto. It carries a **Cloud Interface binding** (which
concrete cluster/host/account) and a **credentials reference** (a key into stored
credentials — never the secret itself).

| Kind         | `DeploymentKind` | Status (R1)                                  |
|--------------|------------------|----------------------------------------------|
| `local`      | `Local`          | **functional** — this node / this machine    |
| `kubernetes` | `Kubernetes`     | declared, not built (planned)                |
| `vpc`        | `Vpc`            | declared, not built (planned)                |
| `cloud`      | `Cloud`          | declared, not built (planned)                |

Declaration order is the placement dial's local→cloud progression. The non-local
kinds are named so every layer refers to the same taxonomy (like `ClusterRole`,
which declares roles before they exist); only `Local` can actually be placed onto
in R1. For a `Local` target the `binding` and `credentials_ref` are empty — the
binding is implicitly this node and no credentials are needed.

```cpp
DeploymentTarget local = DeploymentTarget::local();   // kind = Local, no binding
local.is_local();                                     // true
```

`to_json()` / `from_json()` serialize the shape `{ kind, binding,
credentials_ref }`. `from_json` **validates at the boundary**: it returns
`std::nullopt` unless the value is an object whose `kind` is a known name, so a
malformed target never enters the in-memory tree.

## The record — `Deployment`

Deploying records a `Deployment` (`include/zetta/pipeline/deployment.hpp`) — metadata only,
mirroring [`Project`](projects.md):

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `graph_name` | `std::string` | Caller's label for the deployed graph. |
| `target` | `DeploymentTarget` | The placement (R1: always `local`). |
| `node_id` | `std::string` | The node the graph was placed on (its cluster id). |
| `node_count` | `std::size_t` | Graph summary captured at deploy time. |
| `edge_count` | `std::size_t` | Graph summary captured at deploy time. |
| `created` | `std::string` | ISO-8601 UTC stamp (`zetta::util::now_iso8601()`). |

The graph is recorded as a **summary** (its name + node/edge counts), **not** the
full graph: the ExecutionGraph IR has no serialized form yet (a later slice, see
[execution-graph.md](execution-graph.md)). When it does, a deployment will
reference the stored graph by id; the summary is the honest record until then. A
deployment is **immutable once recorded** in this slice (no rename/update path).

## CRUD on a Node

`Node` (`include/zetta/node.hpp`) owns deployments alongside databases and
projects:

| Method | Returns | Behavior |
|---|---|---|
| `deploy_graph(graph, target, graph_name)` | `shared_ptr<Deployment>` | Empty handle if the graph is empty or the target is non-local (R1). Stamps `created`, records the placement node. |
| `find_deployment(id)` | `shared_ptr<Deployment>` | Empty handle on miss (id is the UUID). |
| `deployments()` | `vector<shared_ptr<Deployment>>` | Sorted by `created` (then id). |
| `deployment_count()` | `size_t` | — |

```cpp
ExecutionGraph graph = /* a validated graph */;
auto deployment = node.deploy_graph(graph, DeploymentTarget::local(), "churn");
node.find_deployment(deployment->id());
```

`deploy_graph` guards the **basics** — a non-empty graph and a local target — and
then runs the [correctness gate](gate.md) (`default_gate()`) at the seam itself:
a graph that fails the gate is **not** placed and records a blocked
(`allowed=false`) deploy audit event (RULE-1 — no deploy without a passing gate,
enforced for a programmatic deploy, not just the GUI Deploy panel).

## Persistence & replication

Deployments follow zetta's **"EVERYTHING is replicated state"** rule
([replication.md](../cluster/replication.md)): the Automerge CRDT document is the source of
truth, and the in-memory list + on-disk folder are a **projection** of it.

- **Replicated state.** Each deployment is one flat key in the document
  (`src/core/catalog.hpp`):

  ```
  deployment:<uuid> → { graph_name, target: {kind, binding, credentials_ref},
                        node, node_count, edge_count, created }
  ```

  `deploy_graph` captures the key into the document; on each `tick()` the node
  **reconciles** the projection to match, so a deployment created on one node
  materializes on every peer (recorded with the node that placed it). Versions
  converge conflict-free.
- **On-disk projection.** A deployment is a folder named by its UUID under the
  data dir, holding `deployment.json` (`{ id, … }`):

  ```
  data_dir/
    <deployment-uuid>/
      deployment.json
  ```

  `Node::start()` rebuilds the in-memory list by scanning the data dir
  (`load_deployments()`) and — when replicated — reconciling from the document,
  so a deployment **survives restart** in both standalone and replicated nodes.

`deployment.json` is **validated at the boundary** on load: a malformed or
incomplete file (not an object, missing a string `graph_name`, or an unknown
`target.kind`) is skipped rather than trusted into the in-memory tree — matching
how `project.json` / `database.json` are read.

> **Which node is "local"?** Because a deployment replicates to every peer, the
> record stores the `node` it was placed on (the originating node's id). The
> [deployment controller](deployment-controller.md) turns "recorded" into
> "running" — but only on the node that placed it (`node == node_id()`); a peer
> sees the record but does not act on it. Cross-node failover (a survivor adopting
> a dead node's placements) is a later, leadership-gated task.

## Queryable

Deployments are queryable two ways:

- **C++ API** — `Node::deployments()` / `find_deployment()` (above).
- **SQL** — the virtual **`system.deployments`** table
  ([query.md](../data/query.md)), alongside `system.nodes`:

  ```sql
  SELECT * FROM system.deployments;
  SELECT graph_name, node_count FROM system.deployments
    WHERE target_kind = 'local';
  ```

  Columns: `deployment_id`, `graph_name`, `target_kind`, `binding`,
  `credentials_ref`, `node_id`, `node_count`, `edge_count`, `created`. The
  executor sources the rows from `Node::deployments()`; `SELECT *`, projection,
  and a single `WHERE` equality work exactly as for a regular table. (zetta has no
  MCP server today, so there is no MCP query surface — SQL + the C++ API are the
  queryable surfaces.)

## What's deferred (later slices)

- **Deeper gate checks** — the gate now runs at the deploy seam (RULE-1), but its
  semantic checks are still growing: acyclicity and legal stage-transition checks
  are not yet among the `default_gate()` set.
- **Cross-node failover** — the local reconcile/self-heal loop now exists (the
  [deployment controller](deployment-controller.md)), but a surviving node
  *adopting* a dead node's placements needs leadership/lease and is a later slice.
- **k8s / VPC / cloud targets** — the non-local `DeploymentKind`s + their Cloud
  Interface bindings and provisioning (ADR-7 / ADR-15).
- **Graph serialization** — once the IR has a serialized form, a deployment will
  reference the stored graph by id instead of recording only a summary.

Note: **executing** a deployed graph (running it step-by-step into a
[Run](runs.md)) is its own slice and exists today — see
[execution.md](execution.md). It takes the graph directly (a deployment records
only a summary); once a deployment references a stored graph by id, a run will be
launchable straight from a deployment record.

## Tests

`tests/pipeline/test_deployment.cpp` covers the `DeploymentTarget` JSON round-trip +
boundary validation, placing a graph onto a local target (and rejecting an empty
graph / non-local target), the `system.deployments` query surface, survival
across a restart (both disk-only and CRDT-only), and replication of a deployment
to a connected peer (recording the placing node).

See also ENT-6 (`#87`), FEAT-4 (`#135`), and [execution-graph.md](execution-graph.md).
