# Partition rebalance on node join (TC-10) · `cluster.partition_rebalance`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_distributed.cpp` · **Engine doc:** [partition-rebalance.md](../../../data/partition-rebalance.md)

Partition a table across the cluster; when a node joins, the rendezvous rebalance
re-places ~1/N of the partitions and the new placement reflects in
`system.dataset_partitions` on every node, with the replication factor preserved
and every row still readable exactly once. The cluster-level acceptance of TC-10
(#500).

## What it does

1. **Stands up two replicated nodes.** A writer (A) and a peer (B), each with its
   own `ReplicationManager` + scratch data dir, preflight off. Convergence is driven
   by merging A's CRDT document into B and reconciling on `tick()`, the
   deterministic form of the gossip path (no TCP).
2. **Partitions a table on the writer.** The writer partitions a table over
   `write_members` using [`RendezvousPlacement`](../../../data/partitioner.md)
   (`partition_count=64`, `rf`) and [`write_partitioned`](../../../data/partitioned-write-path.md),
   then the peer converges (merge A's save into B, then `tick()`).
3. **Confirms the initial placement converged.** Both nodes' placements are read
   from [`system.dataset_partitions`](../../../data/dataset-partitions.md) via the query
   service and asserted identical.
4. **Joins a node + rebalances.** A new node joins; the writer runs
   [`rebalance_partitions(grown, members)`](../../../data/partition-rebalance.md) and the
   peer reconciles the new placement.
5. **Compares the rebalanced placement.** Owners, partition count, the joiner's
   share, the replication factor, and the tiling of the row ranges are all checked.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Partition rebalance on node join (TC-10)**. Set the
options in the modal and click **Run**; the case runs on a worker thread and stands
up a real multi-node loopback cluster, so the UI stays responsive. The result pane
shows pass/fail, the metrics (partitions, moved to joiner, replication factor),
and the log.

**Headless:** the case is exercised (at `nodes=3`, `rf=2`, `rows=120`, with
replicas to exercise the rf-preserved path) by
`tests/testkit/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | write members: the membership the table is partitioned over |
| `rf` | 1 | 1–3 | replication factor: copies (owners) per partition |
| `rows` | 120 | 1–1000000 | rows partitioned over the members |

## What it asserts (pass criteria)

- The **initial placement is identical** on every node.
- The join **moved a nonzero number of partitions**; the rebalanced catalog is
  identical on every node and neither lost nor invented a partition.
- The joiner took **some but not all** partitions (~1/N, not a full rehash), and
  its share agrees across nodes.
- The **replication factor is preserved** for every partition.
- The row ranges still **tile `[0, total)`** on every node (every row readable
  exactly once).

## Related

- Engine: [partition-rebalance.md](../../../data/partition-rebalance.md) ·
  [dataset-partitions.md](../../../data/dataset-partitions.md) ·
  [partitioner.md](../../../data/partitioner.md)
- gtest counterpart: `tests/data/test_partition_rebalance.cpp::CatalogConvergesOnEveryNodeAfterJoin`
- Sibling cases: [`cluster.compute_near_data`](cluster.compute_near_data.md) ·
  [`cluster.featurize_partitions`](cluster.featurize_partitions.md) ·
  [`cluster.fulltext_index`](cluster.fulltext_index.md)
