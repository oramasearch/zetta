# Distributed group-by == single-node · `cluster.group_by_consistency`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_distributed.cpp` · **Engine doc:** [ddf-groupby.md](../../../ddf/ddf-groupby.md)

Shard rows across a real cluster, run a distributed group-by, and assert every
node's result equals the single-node computation over the whole dataset. The heart
of a distributed op giving the same answer however the data is sharded.

## What it does

1. **Computes the single-node ground truth.** It builds the full dataset
   (`rows` rows, each keyed `i % groups` with `amount = i%9 + 1`) into a JsonStore,
   scans it to a [DataFrame](../../../ddf/ddf.md) on a CpuDevice, and runs
   [`ddf::group_by`](../../../ddf/ddf-groupby.md) with count, sum(amount), and mean(amount).
2. **Shards round-robin across the cluster.** It brings up the loopback cluster and
   places row `i` onto node `i % nodes`, scanning each node's shard into its own
   frame.
3. **Runs `distributed_group_by` SPMD.** A thread per node runs
   [`ddf::distributed_group_by`](../../../ddf/ddf-groupby.md) over its shard through its
   [`Collectives`](../../../cluster/collectives.md) (partial-per-node, gather to root, merge,
   broadcast back), so the merged result lands on every node.
4. **Compares.** Each node's keys and per-aggregate values are checked against the
   ground truth.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Distributed group-by == single-node**. Set the
options in the modal and click **Run**; the case runs on a worker thread and stands
up a real multi-node loopback cluster, so the UI stays responsive. The result pane
shows pass/fail, the metrics (nodes, rows, groups), and the log.

**Headless:** the case is exercised (at `nodes=2`, `rows=400`) by
`tests/testkit/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size: how many shards the rows are split across |
| `rows` | 2000 | 1–1000000 | total rows in the dataset |
| `groups` | 5 | 1–512 | distinct group keys (`i % groups`) |

## What it asserts (pass criteria)

- Every node's `distributed_group_by` **succeeded**.
- Every node's grouped result (keys plus count / sum / mean) **equals the
  single-node ground truth** over the whole dataset, so single-node == multi-node.

## Related

- Engine: [ddf-groupby.md](../../../ddf/ddf-groupby.md) ·
  [collectives.md](../../../cluster/collectives.md)
- Sibling cases: [`cluster.allreduce`](cluster.allreduce.md) ·
  [`cluster.compute_near_data`](cluster.compute_near_data.md) ·
  [`cluster.featurize_partitions`](cluster.featurize_partitions.md)
