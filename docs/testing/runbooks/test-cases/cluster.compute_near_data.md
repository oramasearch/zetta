# Compute near data (TC-11) · `cluster.compute_near_data`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_distributed.cpp` · **Engine doc:** [compute-near-data.md](../../../compute/compute-near-data.md)

A DDF group-by runs over the partitions each node **owns** locally (read off local
disk), with no full-data pull. The cluster-runnable acceptance of TC-11 (#501): the
near-data result equals the single-node oracle on every node, and only the partials
cross the wire, not the dataset.

## What it does

1. **Computes the single-node oracle.** The same group-by spec (sum, count, mean
   over `segment`) runs over the whole dataset on one CpuDevice.
2. **Brings up the cluster.** Each node is a [`Node`](../../../architecture/architecture.md) (its
   data + partitions) plus a [`Cluster`](../../../cluster/cluster.md) (the compute plane).
3. **Writes each node's shard into the partitions it owns.** A
   `RoundRobinPartitioner` plus [`write_partitioned`](../../../data/partitioned-write-path.md)
   places each node's round-robin shard onto `{self}`, so the
   [near-data loader](../../../compute/compute-near-data.md) reads the bodies off local disk
   (nothing is handed in out-of-band).
4. **Runs `run_near_data_group_by_job` SPMD.** A thread per node drives the job
   through a [`JobCoordinator`](../../../compute/job-coordinator.md) (a recorded `ddf`
   [`ComputeJob`](../../../compute/compute-jobs.md)): load the owned shard from the local
   [catalog](../../../data/dataset-partitions.md) + locally-held blob bodies, group it, and
   exchange only the per-node partial over the [collectives](../../../cluster/collectives.md).
   It is instrumented with `local_bytes_read` vs `wire_bytes_moved`
   (`Collectives::wire_bytes()`).
5. **Compares + totals.** Each node's result is checked against the oracle and the
   bytes are summed.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Compute near data (TC-11)**. Set the options in the
modal and click **Run**; the case runs on a worker thread and stands up a real
multi-node loopback cluster, so the UI stays responsive. The result pane shows
pass/fail, the metrics (bytes read near data vs moved over wire, the read/moved
ratio), and the log.

**Headless:** the case is exercised (at `nodes=2`, `rows=600`) by
`tests/testkit/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size: how many owners the partitions spread across |
| `rows` | 1200 | 1–1000000 | total rows in the dataset |
| `groups` | 5 | 1–512 | distinct group keys (`i % groups`) |
| `partitions` | 8 | 1–256 | partitions per node |

## What it asserts (pass criteria)

- Every node's near-data job **Succeeded** and read at least one owned partition.
- Each node's result **equals the single-node oracle** (single-node == multi-node).
- **Every row read near data exactly once** across the shards (total rows read ==
  dataset rows).
- The acceptance: on more than one node the bytes **moved over the wire** are far
  fewer than the bytes **read near data** (`wire · 4 < local`), the partials
  (≈ group count) and not the dataset (≈ row count); a single-node cluster moves
  nothing.

## Related

- Engine: [compute-near-data.md](../../../compute/compute-near-data.md) ·
  [dataset-partitions.md](../../../data/dataset-partitions.md) ·
  [ddf-groupby.md](../../../ddf/ddf-groupby.md)
- Sibling cases: [`cluster.group_by_consistency`](cluster.group_by_consistency.md) ·
  [`cluster.featurize_partitions`](cluster.featurize_partitions.md) ·
  [`cluster.partition_rebalance`](cluster.partition_rebalance.md)
