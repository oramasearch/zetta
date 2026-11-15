# Deployment controller (reconcile + self-heal)

The **deployment controller** (`include/zetta/pipeline/deployment_controller.hpp`) is the
**control plane** for local deployments: a reconcile loop that drives a node from
*desired* state toward *actual* state and **self-heals** when a placed run is
killed or fails. This is an atomic slice of *one-button deploy* (FEAT-4, `#135`)
and realizes test case TC-3 (`#250` — deploy + self-heal). **R1 local only.**

A [`Deployment`](deployments.md) records *desired* state — *"this graph should
run on this node."* A [`Run`](runs.md) is the *actual* execution of it. Until this
slice, a deployment was an inert record; nothing turned "recorded" into
"running." The controller closes that gap: for every deployment placed on **this**
node it ensures there is a healthy run, and starts a replacement when one
disappears.

## What it does

`DeploymentController::reconcile()` is one pass over this node's local
deployments. For each one it looks at the runs tied to it (`run.deployment() ==
deployment.id()`) and decides:

| Actual state of the deployment's runs | Verdict | Action |
|---|---|---|
| At least one run is **pending / running / succeeded** | satisfied | nothing — read-only scan |
| **No** runs, or **all** runs **failed** | desired > actual | start a fresh `running` run, recorded against the deployment, with the deployment's `reads` (the tables its graph reads, zetta#1272) |

A run **satisfies** its deployment unless it has terminated in **failure**:

- **pending / running** — the placed graph is (still) executing; desired state is
  met.
- **succeeded** — terminal success. The placed graph *completed*; the controller
  does **not** keep restarting a finished run.
- **failed** — terminal failure. The deployment is unsatisfied, so the next pass
  starts a replacement. The failed run is **kept** (run history / diagnostics);
  the new run is a sibling, not an overwrite.

"Killing" a run — `Node::drop_run(id)` — removes it from coverage entirely, so the
deployment falls to *no runs* and the next pass restores it. That is the
**self-heal** path the acceptance test exercises: kill the placed run, reconcile,
and desired state is back.

Every (re)start writes a log line onto the new run — either *"started run for
deployment …"* or *"self-heal — restarted deployment … after N failed run(s)"* —
so the heal is observable and diagnosable from the run's own logs. The same
line goes on the audit trail as `data_changed` on `run:<id>`, with actor
`node:<node id>` and surface `internal`: the start is the node's own work, not
the work of whoever deployed the graph (#1327,
[audit trail](../governance/audit-trail.md#work-the-node-starts-itself-1327)).

## How it runs — driven by `tick()`

The controller is owned by the `Node` (`controller_{*this}`) and driven from
`Node::tick()`, the same heartbeat that reconciles the on-disk projection. Each
tick now does three things in order:

1. **Pull** — if the replicated document advanced (a local capture or a peer's
   gossip merged in), materialize the on-disk projection from it.
2. **Control plane** — `controller_.reconcile()`: drive each local deployment
   desired → actual, starting or self-healing its run.
3. **Persist** — if the document moved (a peer merge in step 1 *or* a run started
   in step 2), write it to disk and remember the version now reflected.

Because the controller runs inside `tick()`, it operates automatically in
**headless**, **headed**, and **tests** — no separate thread or scheduler. In
**steady state** (every local deployment already satisfied) a pass is a
**read-only scan** that captures nothing into the CRDT, so it adds **no
replication churn** and doesn't move the document version.

```cpp
auto deployment = node.deploy_graph(graph, DeploymentTarget::local(), "churn");
node.tick();                          // controller starts the placed run
node.drop_run(/* the placed run id */);
node.tick();                          // controller self-heals — a fresh run
```

Tests and the UI can also introspect or drive it directly:

```cpp
const ReconcileReport report = node.deployment_controller().reconcile();
node.deployment_controller().total_runs_started();   // cumulative counter
node.deployment_controller().passes();
```

## `ReconcileReport`

Each pass returns a cheap `ReconcileReport` — what the control plane saw and did:

| Field | Meaning |
|---|---|
| `local_deployments` | deployments placed on **this** node (the R1 scope) |
| `satisfied` | already had a healthy run |
| `runs_started` | runs (re)started this pass |
| `healed` | the deployment ids a run was started for |

The controller also keeps cumulative counters across all passes
(`total_runs_started()`, `passes()`) for observability.

## The `deployment` reference on a run

To tie an *actual* run back to the *desired* deployment it serves, `Run` gains an
optional `deployment` field (`run.deployment()`), set at creation via
`Node::create_run(project, graph, deployment)`. It is part of the
[Run JSON Schema](runs.md) (`"deployment": { "type": "string" }`) and is
**schema-validated at the storage boundary** like every other run field — a
malformed value is rejected by `parse_run_value`, never trusted into the tree.
Like `project` / `graph` it is fixed at creation and immutable thereafter. A run
created by hand (not by the controller) simply leaves it empty.

## R1 scope — local only; cross-node failover is deferred

A node reconciles **only the deployments it placed itself** — `target` is `local`
**and** `deployment.node_id() == node.node_id()`. A deployment placed on a *peer*
replicates onto this node (everything is replicated state), and the peer's run
record replicates too, but **this** node does not act on it: that is the peer's
responsibility.

**Cross-node failover** — a surviving node *adopting* a dead node's placements and
restarting them elsewhere — is intentionally a **later slice**. It requires
exclusive ownership (only one survivor should adopt a given placement), which is
the **leadership / lease** mechanism scoped to exclusive structures (see
[replication.md](../cluster/replication.md) → *Consistency model*). Like `ClusterRole` and
the non-local `DeploymentKind`s, the boundary is declared before it is built. R1
self-heal is **within a node**: restart/replace the placed run on the node that
owns it.

## Tests

`tests/pipeline/test_deployment_controller.cpp` covers:

- **desired → actual** — a freshly placed local deployment gets a running run;
  the report counts it.
- **steady state** — a satisfied deployment (running *or* succeeded) is left
  alone; a second pass starts nothing.
- **self-heal (killed)** — drop the placed run and the next pass restores a fresh
  run with a new id (TC-3).
- **self-heal (failed)** — a failed run is replaced by a sibling running run; the
  failed run is retained.
- **tick-driven** — a bare `Node::tick()` starts and later self-heals the run,
  and the cumulative counters advance.
- **R1 scope** — a deployment placed on a peer converges onto this node, but this
  node's controller never starts a run for it.

See also [deployments.md](deployments.md), [runs.md](runs.md), FEAT-4 (`#135`),
TC-3 (`#250`).
