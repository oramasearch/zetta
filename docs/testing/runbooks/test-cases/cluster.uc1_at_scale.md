# UC-1 at scale (TC-12) · `cluster.uc1_at_scale`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_uc1_scale.cpp` · **Engine doc:** [uc1-at-scale-e2e.md](../../../use-cases/uc1-at-scale-e2e.md)

The capstone scale test — the full **cost-capped burst lifecycle** (UC-4) around
the distributed UC-1 pipeline. One canonical sim-Python program drives the whole
flow over a real, **replicated**, manually-sized loopback cluster: **load the
saas-sales-conversations parquet as one partitioned table across the permanent
members → burst within the script's `cost_cap=` through the BurstApprovalGate
(an over-cap burst is blocked deny-by-default + audited) → partitions rebalance
onto the burst nodes (bodies follow) → mint short-lived burst identities →
featurize the engineered columns compute-near-data over the partitions → train a
calibrated GBDT on `outcome` over the resulting feature table → explain (per-row
TreeSHAP drivers) → rank (the at-risk list) → tear the burst down (partitions
drain back, identities revoked, the model stays live for serving)** — gated
(RULE-1) and governed (key-scoped access + an audit trail). It proves the
at-scale pipeline runs end to end across an elastically-grown cluster within its
cost cap and produces a real model identical on every node.

## What it does

1. **Compiles + gates the program.** The sim-Python script is the spec: the case
   compiles it with the [Python frontend](../../../pipeline/python-frontend.md), runs
   [`default_gate()`](../../../pipeline/gate.md) (RULE-1), and reads the data shape (the
   `load_dataset` column projection), the node count + **cost cap**
   (`cluster.add_nodes(10, provider='azure', cost_cap=12)`), and the label
   (`train(target=…)`) from the lowered graph.
2. **Stands up the replicated cluster.** `nodes` permanent members + `burst`
   standby nodes, each wired the way `main()` wires a live process
   (`ReplicationManager` + `Node` + `ReplicatedBlobStore` + `Cluster` — the
   [harness](../../test-suite.md)'s replicated mode). Only the permanent members
   announce themselves; the standby nodes are the hermetic stand-in for cloud
   capacity.
3. **Loads the saas parquet as ONE partitioned table.** Decodes the embedded
   `saas_sample.parquet` with the native [Parquet reader](../../../data/parquet-reader.md)
   (no Arrow), dropping the `embedding_*` columns, then scales it up with
   deterministic saas-shaped rows. The driver writes it once through the
   [partitioned write path](../../../data/partitioned-write-path.md) with
   [rendezvous placement](../../../data/partitioner.md) (P=64, rf=1) over the
   **permanent members**; every member pulls the bodies of the partitions it owns.
4. **Bursts, cost-capped.** Runs the lowered `cluster.add_nodes(…, cost_cap=…)`
   through the **Node-aware, gated** [`run_add_nodes_step`](../../../cloud/add-nodes.md)
   with an **injected** `LocalProvisioner` over the standby fleet (the `provider=`
   is provenance only, so the cloud burst stays consent-gated and the case is
   hermetic). The [`BurstApprovalGate`](../../../cloud/burst-approval-gate.md) wraps the
   provisioner: an **over-cap probe** (200 nodes vs the same cap) is blocked
   before the backend is touched and audited (`provision_requested`); the real
   within-cap burst provisions, joins the replicated membership, and the
   [between-jobs rebalance](../../../data/partition-rebalance.md) moves partitions onto
   the burst nodes — the case waits for their bodies to arrive.
5. **Governs.** A scoped, capability-bearing key authorizes the run (in-scope
   allowed, out-of-scope denied — [key-scoped access](../../../governance/access-control.md)),
   and each burst node gets a short-lived identity from the
   [`BurstIdentityManager`](../../../cloud/burst-identity.md) (`key_issued` audited).
6. **Featurizes + trains near data, SPMD.** Every node — permanent **and burst** —
   concurrently runs
   [`run_near_data_featurize_train_job`](../../../use-cases/uc1-featurize-train-over-partitions.md)
   (#530) over its owned partitions: featurize (the broadcast feature table,
   identical on every node) → train (+ calibrate) **over the feature table** →
   explain (TreeSHAP) → rank, each a recorded
   [`ComputeJob`](../../../compute/compute-jobs.md). The case thread pumps each node's
   [owner queue](../../../architecture/owner-gate.md) while the job threads run.
7. **Tears the burst down (UC-4).** Partitions drain back onto the permanent
   members, the burst identities are revoked (`key_revoked` audited), the burst
   nodes are released through the provisioner seam — and the model must still
   **serve from every permanent member**.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → UC-1 at scale (TC-12)**. Set the options in the
modal and click **Run**; the result pane shows pass/fail, the metrics (cost vs
cap, partitions on burst / drained back, top driver, AUC, bytes read near data vs
partials over wire, audit events), and the log.

**Headless:** the case is exercised (at `nodes=2`, `burst=1`, `rows=96`) by
`tests/testkit/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 2 | 1–4 | permanent members — the standing cluster |
| `burst` | 1 | 1–3 | burst nodes — the standby fleet the gated add provisions |
| `rows` | 96 | 12–200000 | conversations (the saas fixture, scaled up) |
| `calibrate` | true | bool | fit held-out Platt calibration on the model |

The script names **10** nodes against a `cost_cap=12` (what an operator bursts
for real); the loopback case is sized by `nodes` + `burst`, with
requested-vs-provisioned counts and cost-vs-cap in the result metrics.

## What it asserts (pass criteria)

- **The cost cap held** — the script's burst cost is within its own `cost_cap=`,
  the over-cap probe was blocked (no members, backend untouched, membership
  unchanged) and audited, and the metrics report cost vs cap.
- **Partitions rebalanced onto the burst nodes** with their bodies present locally
  before training, and every one drained back at teardown.
- The broadcast **feature table covers every conversation** with the script's
  columns, identical on every node, its `featurize` job recorded.
- A real calibrated [`Model`](../../../ml/models.md) registered on the root, **identical
  model + ranking on every node** (single-node == multi-node), AUC > 0.6.
- Per-row TreeSHAP drivers (in feature order, non-empty top driver) + a ranked
  at-risk list (every conversation once, risk in `[0,1]` descending).
- **Compute-near-data, on the burst too** — each node read its own shard off local
  disk at train time and, multi-node, read only its shard; every conversation
  read near data + explained once. Only the training partials cross the wire.
- **The model stays live for serving** — it resolves on every permanent member
  after the burst is gone.
- **Gated + governed, the whole lifecycle** — the gate passed, the scoped key
  authorized the run, and the [audit trail](../../../governance/audit-trail.md) captured the
  access decisions, each job's created/finished events, the blocked over-cap
  burst, and the burst identity lifecycle (`key_issued` / `key_revoked`).

## Related

- Engine: [uc1-at-scale-e2e.md](../../../use-cases/uc1-at-scale-e2e.md) ·
  [uc1-featurize-train-over-partitions.md](../../../use-cases/uc1-featurize-train-over-partitions.md) ·
  [uc1-train-over-partitions.md](../../../use-cases/uc1-train-over-partitions.md) ·
  [compute-near-data.md](../../../compute/compute-near-data.md) ·
  [add-nodes.md](../../../cloud/add-nodes.md) ·
  [burst-approval-gate.md](../../../cloud/burst-approval-gate.md) ·
  [burst-identity.md](../../../cloud/burst-identity.md) ·
  [partition-rebalance.md](../../../data/partition-rebalance.md)
- Sibling cases: [`cluster.featurize_partitions`](cluster.featurize_partitions.md) ·
  [`cluster.compute_near_data`](cluster.compute_near_data.md) ·
  [`cluster.partition_rebalance`](cluster.partition_rebalance.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md) ·
  [`examples.uc1_at_scale`](examples.uc1_at_scale.md)
