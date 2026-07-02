# Fulltext index build + query (TC-13) · `cluster.fulltext_index`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_fulltext.cpp` · **Engine doc:** [distributed-fulltext.md](../../../indexing/distributed-fulltext.md)

Build a full-text index over a table's partitions via the index seam (ADR-34) on a
real multi-node cluster: each node builds one sub-index per partition it owns
(compute-near-data), then a scatter-gather query is asserted to match a single
consolidated-index oracle on every node. The cluster-runnable acceptance of TC-13
(#503). It uses the `oramacore_fulltext` BM25F backend when compiled in, else a
partition-independent reference scorer.

## What it does

1. **Picks the backend.** The real BM25F
   [`OramaCoreFulltextIndex`](../../../indexing/oramacore-fulltext-index.md) when
   `ZETTA_WITH_ORAMACORE` is compiled in, else a deterministic,
   partition-independent term-frequency reference scorer, so a CPU-only checkout
   that lacks the private submodule stays green. The corpus is tf-dominated, so the
   global ranking is unambiguous (exact for the global top-k) either way.
2. **Shards a corpus into owned partitions.** It brings up the loopback cluster;
   each node holds a disjoint round-robin shard of a "postgres" article corpus as
   its **own** partitions (`RoundRobinPartitioner` plus
   [`write_partitioned`](../../../data/partitioned-write-path.md)).
3. **Builds per-partition sub-indexes near data.** One
   [`DistributedFulltextIndex`](../../../indexing/distributed-fulltext.md) per node, with
   `build_local` building one sub-index per partition it owns (read off local disk).
4. **Computes the consolidated oracle.** One index over **every** row of every
   node's table, queried once, the single source of truth.
5. **Queries by scatter-gather.** Every node, acting as the query coordinator,
   scatters `"postgres"` to its peers over the `fulltext.query`
   [signal-plane](../../../cluster/signal-plane.md) RPC, gathers each node's ranked hits, and
   merges into one global ranking.
6. **Compares.** Each node's ranking is checked against the oracle.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Fulltext index build + query (TC-13)**. Set the
options in the modal and click **Run**; the case runs on a worker thread and stands
up a real multi-node loopback cluster, so the UI stays responsive. The result pane
shows pass/fail, the metrics (the backend used, partitions indexed, matched + ranked,
bytes read near data vs moved over wire), and the log.

**Headless:** the case is exercised (at `nodes=2`, `copies=3`) by
`tests/testkit/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size: how many shards the corpus spreads across |
| `copies` | 6 | 1–64 | corpus copies: repeats the article corpus so rows spread across more partitions per node |
| `partitions` | 4 | 1–64 | partitions per node |
| `top_k` | 0 | 0–1000 | result cap (`0` = all matching rows) |

## What it asserts (pass criteria)

- Every node's `build_local` **succeeded** and indexed at least one owned partition;
  all rows were indexed near data exactly once, and partition bodies were read near
  data (local bytes > 0).
- The consolidated oracle matched at least one row.
- Every node's scatter-gather ranking is **identical and equal to the consolidated
  oracle**; scores align with row ids and rank descending; every node heard from all
  N nodes.
- A multi-node query genuinely **moved hits over the wire** (scatter + gather); a
  single-node cluster answers locally and moves nothing.

## Related

- Engine: [distributed-fulltext.md](../../../indexing/distributed-fulltext.md) ·
  [oramacore-fulltext-index.md](../../../indexing/oramacore-fulltext-index.md) ·
  [index-backend.md](../../../indexing/index-backend.md) ·
  [compute-near-data.md](../../../compute/compute-near-data.md)
- Sibling cases: [`cluster.compute_near_data`](cluster.compute_near_data.md) ·
  [`cluster.partition_rebalance`](cluster.partition_rebalance.md) ·
  [`cluster.featurize_partitions`](cluster.featurize_partitions.md)
