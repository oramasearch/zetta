# Featurize over partitions == single-node · `cluster.featurize_partitions`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_distributed.cpp` · **Engine doc:** [ddf-featurize.md](../../ddf-featurize.md)

Point-in-time featurize engineered numeric columns (the saas-sales-conversations
shape) compute-near-data over a partitioned table: each node owns a shard,
featurizes it leakage-free (`event_ts <= as_of`), and the global feature table is
asserted identical to the single-node oracle on every node (#532).

## What it does

1. **Computes the single-node featurize oracle.** It builds `rows` conversations
   (engineered columns + an `event_ts` + a binary `outcome`, with ~1 in 7 sitting
   **after** the `as_of` cutoff, the leakage trap), loads them to a
   [DataFrame](../../ddf.md), and runs
   [`point_in_time_featurize`](../../ddf-featurize.md) with a static spec (the
   engineered columns carried through, `outcome` the label, the row universe filtered
   to `event_ts <= as_of`). It confirms the trap fired (fewer feature rows than input
   rows).
2. **Holds each node's shard as its own partitions.** A `RoundRobinPartitioner` plus
   [`write_partitioned`](../../partitioned-write-path.md) places each node's shard
   onto itself (every conversation on one node).
3. **Runs `run_near_data_featurize_job` SPMD.** A thread per node drives the job
   through a [`JobCoordinator`](../../job-coordinator.md) (a recorded `featurize`
   [`ComputeJob`](../../compute-jobs.md)) over the locally-owned shard.
4. **Compares + totals.** Each node's global feature table is checked against the
   oracle and the rows read are summed.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Featurize over partitions == single-node**. Set the
options in the modal and click **Run**; the case runs on a worker thread and stands
up a real multi-node loopback cluster, so the UI stays responsive. The result pane
shows pass/fail, the metrics (conversations, feature rows, excluded post-cutoff,
features), and the log.

**Headless:** the case is exercised (at `nodes=2`, `rows=140`) by
`tests/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size: how many shards the conversations split across |
| `rows` | 300 | 1–1000000 | conversations (entities) to featurize |

## What it asserts (pass criteria)

- The leakage trap fired (the oracle has **fewer feature rows than input rows**, so
  post-cutoff conversations were excluded).
- Every node's featurize job **Succeeded**.
- Each node's global feature table **equals the single-node oracle** (entities,
  feature names, label, values within `1e-6`): leakage-free, single-node ==
  multi-node.
- **Every conversation read near data exactly once.**

## Related

- Engine: [ddf-featurize.md](../../ddf-featurize.md) ·
  [compute-near-data.md](../../compute-near-data.md) ·
  [dataset-partitions.md](../../dataset-partitions.md)
- Sibling cases: [`cluster.compute_near_data`](cluster.compute_near_data.md) ·
  [`cluster.group_by_consistency`](cluster.group_by_consistency.md) ·
  [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)
