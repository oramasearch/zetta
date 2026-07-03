# UC-1 at scale — the sim-Python E2E (TC-12)

The **capstone scale test** (issues **#531** + **#502**, realizes **TC-12**): one
canonical **sim-Python program** drives the whole UC-1 pipeline at scale — with
the **full cost-capped burst lifecycle** (UC-4) around it — over a **real,
replicated, manually-sized cluster** —

```
load the saas-sales-conversations parquet as ONE PARTITIONED table
across the permanent members
  → burst within the script's cost_cap= (over-cap blocked, deny-by-default, audited)
    → partitions REBALANCE onto the burst nodes (bodies follow over the blob seam)
      → mint short-lived burst identities
        → featurize the engineered columns, compute-near-data over the partitions
          → train a calibrated GBDT on `outcome` over the RESULTING feature table
            → explain (per-row TreeSHAP drivers) + rank (the at-risk list)
              → tear the burst down: partitions drain back, identities revoked,
                the model stays live for serving
```

— **gated** (RULE-1) and **governed** (key-scoped access + an audit trail). The
program doubles as the **canonical usage example**: it is the same script an
operator runs for real in the headed app (start a node, add cloud nodes behind the
[consent gate](../cloud/cloud-inspector.md), launch the case). It composes already-landed,
unit-tested pieces — it adds no new engine code beyond the
[cost-cap-gated add-nodes binding](../cloud/add-nodes.md), only the end-to-end
wiring + assertions.

```
src/core/testkit/cases_uc1_scale.cpp   the case + the shared canonical script
src/core/testkit/cases_examples.cpp    examples.uc1_at_scale (the example registration)
tests/testkit/test_testkit.cpp                 DistributedCasesRunOnARealCluster (headless gtest)
```

## The canonical program

```python
nodes = cluster.add_nodes(10, provider='azure', cost_cap=12)
conversations = load_dataset('saas-sales-conversations', columns=['messages',
    'avg_response_seconds', 'sentiment', 'questions_asked', 'outcome'])
model = train(conversations, target='outcome')
drivers = explain(model)
at_risk = rank(model)
```

It lowers through the [simulated-Python frontend](../pipeline/python-frontend.md) to the
[ExecutionGraph IR](../pipeline/execution-graph.md) over the
`connect → … → act` taxonomy: `cluster.add_nodes` → an `act` control step,
`load_dataset` → `connect`, `train` → `model`, `explain` → `serve`, `rank` →
`index`; the variable refs become the data-flow edges
(`load_dataset → train → explain`, `train → rank`). The `cost_cap=` keyword is the
burst's spend ceiling in relative compute units
([`BurstCap::max_cost`](../cloud/burst-approval-gate.md)), read back by
[`add_nodes_cost_cap`](../cloud/add-nodes.md).

This **one source string** backs two registered tests (so it stays a single source
of truth — `uc1_at_scale_script()`):

- **`examples.uc1_at_scale`** — a lightweight [sim-Python example](../testing/example-scripts.md)
  under `TestCategory::Examples`: the standard harness compiles it, runs it through
  the [gate](../pipeline/gate.md), and executes it **single-node** (the
  [`DefaultStepExecutor`](../pipeline/execution.md)), asserting the **lowered shape**
  (5 steps, 3 edges, each verb's `StepKind`) and that it runs to `Succeeded`
  producing `dataset` / `model` / `index` outputs. It proves the program is real,
  well-formed sim-Python.
- **`cluster.uc1_at_scale`** — the heavy, real-engine case below, under
  `TestCategory::Distributed`.

## What `cluster.uc1_at_scale` does

The script is the **spec**: the case reads the data shape, node count, and cost
cap **from** the lowered graph (the `load_dataset` step's column projection via
[`dataset_load_spec`](../pipeline/datasets-shim.md), the `cluster.add_nodes` count + cap via
[`add_nodes_count` / `add_nodes_cost_cap`](../cloud/add-nodes.md), the `train` step's
`target=` kwarg), then runs the real pipeline:

1. **Compile + gate.** Compile the program; run [`default_gate()`](../pipeline/gate.md)
   (RULE-1) — a blocked gate fails the case. A script with no `cost_cap=` fails
   the case up front: TC-12 *is* the cost-capped burst.
2. **Load the saas parquet.** Decode the embedded
   `saas_sample.parquet` fixture with the native
   [Parquet reader](../data/parquet-reader.md) (`read_parquet_rows` — no Apache Arrow),
   **projecting to the script's columns and dropping the `embedding_*` columns**.
   The real fixture seeds the dataset; it is scaled to the chosen size with
   deterministic, saas-shaped synthetic conversations (a planted-but-imperfect
   signal, ~9% label noise) so the at-scale train + calibration are meaningful —
   the way an operator's real run loads the full ~100k-row dataset.
3. **Stand up the REPLICATED cluster.** Bring up a real
   [`LoopbackCluster`](../testing/test-suite.md) of `nodes + burst` nodes in
   **replicated mode** — each wired the way `main()` wires a live process
   (`ReplicationManager` + `Node` + `ReplicatedBlobStore` + `Cluster`), so the
   catalog gossips and partition bodies follow ownership. Only the `nodes`
   permanent members announce themselves (`node:<id>`); the `burst` standby nodes
   are the hermetic stand-in for the cloud capacity a real burst provisions.
4. **Load as ONE partitioned table.** The driver creates `data.conversations`
   once, partitions it with [`RendezvousPlacement`](../data/partitioner.md) (P=64,
   rf=1), and drives the [partitioned write path](../data/partitioned-write-path.md)
   with placement over the **permanent members** — the
   [compute-near-data](../compute/compute-near-data.md) layout. The case waits until
   every member holds the bodies of the partitions it owns (pulled over the
   [blob seam](../data/partition-rebalance.md)).
5. **Burst, cost-capped.** Execute the lowered `cluster.add_nodes(10,
   provider='azure', cost_cap=12)` step through the **Node-aware, gated**
   [`run_add_nodes_step`](../cloud/add-nodes.md): the injected provisioner is wrapped
   in the [`BurstApprovalGate`](../cloud/burst-approval-gate.md) (a transparent
   unit-per-node `CostEstimator`), the provisioned members join the replicated
   membership, and the [between-jobs rebalance](../data/partition-rebalance.md)
   spreads partitions onto them — the case waits until the burst nodes hold their
   bodies. First, a **deny-by-default probe**: an over-cap burst (200 nodes vs the
   same cap) through the same path is **blocked** before the backend is touched
   and recorded as a `provision_requested` audit event. The `provider=` is
   **provenance only** — the `Provisioner` is **injected**, so the cloud stays
   consent-gated; here a [`LocalProvisioner`](../cloud/provisioner.md) over the standby
   fleet stands in for the Azure burst, keeping the case hermetic and headless.
6. **Govern.** On the driver node, a `uc1-pipeline` service
   [principal](../governance/access-control.md) holds a key scoped to the `uc1` project + `data`
   database with `orchestration:start_run` + `data:read` capabilities. The case
   authorizes against it (in-scope allowed; out-of-scope denied — deny-by-default).
   Each burst node gets a **short-lived identity** from the
   [`BurstIdentityManager`](../cloud/burst-identity.md) (`key_issued` audited).
7. **Featurize + train near data, SPMD.** Every node — permanent **and burst** —
   concurrently runs
   [`run_near_data_featurize_train_job`](uc1-featurize-train-over-partitions.md)
   (#530) with a static [`FeaturizeSpec`](../ddf/ddf-featurize.md) built from the
   script (the projected columns as `account_features`, the `target=` kwarg as the
   label): load this node's owned partitions once, featurize (the broadcast feature
   table, identical on every node), then train (+ calibrate) → explain (per-row
   TreeSHAP) → rank (the at-risk list) **over the feature table** — not the raw
   engineered columns — each a recorded [`ComputeJob`](../compute/compute-jobs.md).
   Only the per-node partials cross the wire. While the per-node job threads run,
   the case thread pumps each node's
   [owner queue](../architecture/owner-gate.md) (`drain_owner_work`), exactly the
   role a live tick loop plays.
8. **Tear the burst down (UC-4).** Drain the partitions back onto the permanent
   members ([`rebalance_partitions`](../data/partition-rebalance.md) to the permanent
   membership; bodies follow), **revoke** the burst identities (`key_revoked`
   audited), and release the burst nodes through the same provisioner seam. The
   trained model must still **serve from every permanent member** afterwards —
   replicated state, not a copy stranded on a temporary node.

## What it asserts

- **The cost cap held** — the script's own burst cost (10 nodes × 1 unit) is
  within its `cost_cap=12`; the over-cap probe was **blocked** (no members, the
  inner backend untouched, membership unchanged) and **audited**
  (`provision_requested`).
- **Partitions genuinely moved** — the rebalance put a non-zero share of the
  partitions onto the burst nodes with their bodies present locally before
  training, and the teardown drained every one of them back.
- **A real feature table + model + drivers + ranking on every node** — the pipeline
  `Succeeded`, the broadcast feature table covers every conversation with the
  script's columns and is identical on every node, its `featurize` job is recorded,
  a calibrated [`Model`](../ml/models.md) is registered (`find_model`), the per-row
  TreeSHAP drivers are in `feature_columns` order with a non-empty top driver, and
  the at-risk list ranks every conversation once by risk descending, each risk in
  `[0, 1]`. The model learned the signal (AUC > 0.6).
- **Single-node == multi-node** — every node converges on the **identical** model +
  ranking (the trees grow off globally-allreduced histograms, the result is
  broadcast). The rigorous single-node-vs-N-node oracle equivalence is proven in
  [`tests/ml/test_gbdt_train_partitions.cpp`](uc1-train-over-partitions.md).
- **Compute-near-data, on the burst too** — each node (including every burst node)
  read its own shard off local disk and, multi-node, read only its shard — not the
  whole dataset; every conversation was read near data and explained exactly once.
  Only the training partials (gradient histograms, SHAP, scores) cross the wire at
  train time — for iterative GBDT training those partials can exceed the tiny
  local shard, so the near-data guarantee is "the raw data stays local at train
  time", not "fewer bytes move".
- **The model stays live for serving** — after the burst teardown, the model id
  resolves on **every permanent member**.
- **Gated + governed, the whole lifecycle** — the gate passed, the scoped key
  authorized the run, and the immutable [audit trail](../governance/audit-trail.md) captured
  the access decisions, each job's `created` + `finished` events, the blocked
  over-cap burst (`provision_requested`), and the burst identity lifecycle
  (`key_issued` / `key_revoked`).

## Options (manually-sized)

| option | default | meaning |
|---|---|---|
| `nodes` | 2 (1–4) | permanent members — the standing cluster |
| `burst` | 1 (1–3) | burst nodes — the standby fleet the gated add provisions |
| `rows` | 96 | conversations (the saas fixture, scaled up) |
| `calibrate` | true | fit held-out Platt calibration on the model |

The script names **10** nodes (what an operator bursts for real) against a
`cost_cap=12`; the loopback case is sized by the `nodes` + `burst` options
(best-effort on given hardware), with the requested-vs-provisioned counts and the
cost-vs-cap numbers reported in the result metrics.

## Run it

```bash
# headless gtest (nodes=2, burst=1, rows=96)
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
# the lightweight example
ctest --test-dir build -R 'ExampleScripts' --output-on-failure
```

Or run `cluster.uc1_at_scale` interactively from the headed **Operations → Test
suite** panel (pick the node counts, watch it run over a real loopback cluster).

## Relation

- The **pipeline step** it runs is the
  [featurize → train composition](uc1-featurize-train-over-partitions.md) (#530),
  which composes [featurize engineered columns over partitions](../ddf/ddf-featurize.md)
  (#532) with [train GBDT(outcome) over partitions](uc1-train-over-partitions.md)
  (#533); all ride the [compute-near-data](../compute/compute-near-data.md) loader (#516).
- The **burst lifecycle** composes the
  [`BurstApprovalGate`](../cloud/burst-approval-gate.md) (APR-3 / RSK-9), the
  [Node-aware `add_nodes`](../cloud/add-nodes.md) (#518/#519), the
  [between-jobs partition rebalance](../data/partition-rebalance.md) (#517), and the
  [`BurstIdentityManager`](../cloud/burst-identity.md) — this case is where they run
  end to end (verifying **NFR-3** #494 and **QAS-3** #495 in their shipped,
  between-jobs form; mid-job elastic rebalance stays deferred).
- It is the at-scale sibling of the single-node [UC-1 end-to-end](uc1-end-to-end.md)
  (#399), and the capstone of **UC-4** (#267, burst-train on temporary nodes):
  provision → train → tear down → the model still serves.
- **Deliberately later:** running over the real HF-hub dataset (a Hub-resolver +
  scale exercise) and a **real Azure burst** — the cross-process cloud run needs the
  [provisioned-VM cluster join](../plans/cloud-provisioning.md) (cloud-init +
  reachable seed), and the cloud path stays consent-gated per the hard rule.
