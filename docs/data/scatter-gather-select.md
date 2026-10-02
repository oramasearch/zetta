# Scatter-gather SELECT over a partition-only table

SQL is node-local: `Executor` reads `table->store()`. That is *complete* today
only because a partitioned table also replicates every row through the CRDT. A
[partition-only table](partition-only-tables.md) does not — its rows live in the
partition bodies their owners hold — so a node-local scan would answer from
whatever slice happened to be on the node that parsed the SQL. `SELECT count(*)`
would come back a fraction of the truth and look perfectly healthy.

So the executor routes a `SELECT` over such a table to
`zetta::query::DistributedSelect`, which fans out to the partition **owners**,
scans near the data, and merges.

## The shape

```
coordinator                              owner (any node, including the coordinator)
-----------                              -----------------------------------------
read the catalog (replicate-to-all)
assign every partition to ONE owner  ->  sql.scan { table, partitions, where,
                                                    columns | group_by+aggregates }
                                         read each body from LOCAL disk
                                         filter (WHERE) + project, or accumulate
                                         per-group PARTIALS
gather                               <-  { blocks: [...] } | { groups: [...] }
merge + finish
```

The RPC rides the existing signal plane as `sql.scan` — no new transport, no new
wire verb family — with both payloads validated against
[`schemas/partition_scan_request.schema.json`](../../schemas/partition_scan_request.schema.json)
and
[`schemas/partition_scan_response.schema.json`](../../schemas/partition_scan_response.schema.json)
at the boundary. This is the [distributed fulltext](../indexing/distributed-fulltext.md)
precedent, with two deliberate differences.

### 1. Each partition is scanned exactly once

Fulltext scatters to every connected peer and lets each filter down to what it
owns. That is safe while every partition has one owner; with replicas it would
count a partition once per copy — fine for a ranking that dedupes by row id,
fatal for `count(*)`.

Here the **coordinator** assigns each partition to one owner, preferring itself
(so a single-node cluster opens no connection), and sends each responder its
list. A responder asked for a partition it does not own fails rather than
answering short.

### 2. An unreachable owner fails the query

With RF=1 on connector data there is no replica to read instead. Answering from
four shards out of five — with no error, and a number that looks plausible — is
the one outcome that must never ship. The query fails with the partition id and
its recorded owners; the remedy is re-running the ingest, or
[promoting the dataset](promote-dataset.md) to a higher replication factor first
so a lost node has a fallback.

### 3. An incomplete catalog fails the query too (#837)

The guard above only fires for a partition the coordinator can *see*. A partition
whose `part:` entry has not reached this node is not unreachable — it is
**invisible**: it never enters the plan, so nothing notices, and the query comes
back a plausible fraction.

So before anything is scattered, the coordinator counts the `part:` entries it
holds against the dataset's [partition manifest](partition-manifest.md), the
table-level record of how many the finished write produced. Short ⇒ the query
fails with the same "the dataset is degraded" shape. Both entry points run it —
the clustered `DistributedSelect::execute` and the standalone
`select_over_partitions` — through the single rule
`zetta::data::catalog_shortfall`, so the planner and `system.datasets` can never
disagree. A table with **no** manifest is not checked (it predates them, or was
written by a path that produces none): failing every read of such a dataset would
be worse than the hole.

## Aggregates merge, they do not concatenate

A responder returns a **partial** per group per aggregate — `{value, count}` —
not a finished answer:

| Aggregate | partial `value` | merge | finish |
|---|---|---|---|
| `count` | rows (or non-null values) | add | the sum |
| `sum` | the sum of its shard | add | the sum (integer in, integer out) |
| `min` / `max` | the shard's extreme | reduce | the extreme, or NULL if nothing contributed |
| `avg` | the **sum**, with `count` beside it | add both | `Σvalue / Σcount` |

`avg` is the reason partials exist. A finished mean cannot be merged: averaging
the shards' averages weights a one-row shard exactly like a ten-thousand-row one.
`tests/query/test_distributed_select.cpp` pins that directly — two shards of 1
and 99 rows whose averages are 10 and 1, where the mean of the means (5.5) and
the true mean (1.09) are nowhere near each other.

A shard that contributed nothing to an aggregate (all-null values) carries
`count = 0` and is skipped by the merge, so it cannot drag a `min` down to zero.

Result columns are named exactly as the local `GROUP BY` path names them —
`count`, `sum_x`, `min_x`, `max_x`, `mean_x` — so the same SQL reads the same
whichever path answers it. The owner folds its rows through
`zetta::query::GroupAccumulator`, the same accumulator the executor uses for a
replicated table's text keys and whole-table aggregates (#1218).

**A whole-table aggregate** (`SELECT count(*) FROM t`, no `GROUP BY`) is a scan
request with `aggregates` and no `group_by`: every row is one group with a
`null` key. When nothing survives anywhere, the merge still emits one row —
`count` 0, the rest `NULL` — rather than an empty result.

**Columns.** Each answer carries `columns_seen`, every column its scanned rows
carried before the `WHERE`. The coordinator unions them: a column the query names
that no owner saw is an error naming it, and `SELECT *` reports the union as its
`columns`. An answer without `columns_seen` (a responder that predates it)
switches the check off rather than refusing a real column.

## Where the work happens

- **WHERE and the projection are pushed to the owner** — the filter runs beside
  the data and only the wanted columns of the surviving rows cross the wire.
- **Column-level access masking stays with the coordinator**, which is the node
  that knows the caller.
- **Row-level security is refused** on a partition-only table (as `GROUP BY`
  already is on a policy-protected one): the policy predicate is not shipped to
  the owner, and evaluating it after the fact would mean pulling rows the caller
  may not read. The error says so rather than quietly returning everything.

## Threading — why there is a snapshot

`on_request` runs on the cluster's **network thread**, where the `Node` tree is
off limits (`src/core/CLAUDE.md`). So `DistributedSelect` keeps a small
snapshot — for each partition-only table, the partitions this node owns and the
**path** of each body — rebuilt by `refresh()` from `Node::tick()` and again
before every scatter, guarded by a fingerprint so the steady-state cost is a
string compare. The handler then reads body files directly: a body is
content-addressed and written once, so its bytes never change under a reader.

That is the same freshness the catalog itself has (both advance on tick), and
the same pattern `reconcile_search_indexes` uses for the search plane.

## Wiring

`Node::attach_cluster` constructs the plane chained over the search plane's
signal handler; `Node::stop_coordinator` tears it down first, restoring handlers
in reverse order. A node with **no cluster** has no plane, and
`select_over_partitions` answers from its own partitions — a partition placed
elsewhere fails with the same unreachable-owner reason a dead peer would give.

## Tests

`tests/query/test_distributed_select.cpp`:

- the merge rules asserted directly on hand-built partials (the weighted `avg`,
  and count/sum adding while min/max reduce and an empty shard is ignored);
- the wire round trip plus malformed messages rejected at the schema boundary,
  including a whole-table aggregate request and `columns_seen`;
- a whole-table `count(*)`, a text `GROUP BY` key, an unknown column and
  `SELECT *`'s columns over a partition-only table, on one node and across two;
- a single node answering a partition-only table from its 8 partitions — grouped
  aggregates against a hand-computed truth, a plain `SELECT` returning every row
  exactly once, and a pushed-down `WHERE` + projection;
- **two nodes** with the partitions split across them producing the identical
  answer from either coordinator, with the peer's `scans_served()` proving the
  scan ran where the data is;
- **RF=2**, where every partition has two owners and the answer is still exact;
- an **unreachable owner failing** the query rather than answering short;
- an **incomplete catalog failing** it as well
  (`tests/data/test_dataset_manifest.cpp` —
  `AnIncompleteCatalogFailsTheScatterGatherSelect`, constructed deterministically
  by dropping one `part:` entry while the manifest stands, plus
  `ACompleteCatalogStillAnswers` for the no-false-positive direction).

Related: [partition-manifest.md](partition-manifest.md),
[partition-only-tables.md](partition-only-tables.md),
[query.md](query.md), [dataset-partitions.md](dataset-partitions.md),
[../indexing/distributed-fulltext.md](../indexing/distributed-fulltext.md).
Issue: #833 section 5.
