# The cross-node connector ingest

One query's result pulled with the **cluster's aggregate bandwidth** and landed
where it was pulled (#833 sections 6 + 7, closing [#829](https://github.com/oramasearch/zetta/issues/829)).

Tiling a result set and then running every tile on the coordinating node is a
parallel loop, not a distributed algorithm: it spends one node's uplink and N-1
nodes' worth of nothing, and the rows cross the wire twice — once to the
coordinator, once to their eventual owner. This is the other half of
[connector-query.md](connector-query.md).

```
include/zetta/connect/distributed_ingest.hpp  DistributedIngestOptions/Result, the launch kind
src/core/connect/distributed_ingest.cpp       type dispatch; submit → launch → fetch → land → agree (Snowflake),
                                              run_single_shot_connector_ingest (Postgres and any untileable source)
include/zetta/connect/distributed_fetch.hpp   plan_fetch_assignments (the tiling the launch runs on)
include/zetta/data/fetch_affinity_placement.hpp  the placement that keeps a slice where it landed
schemas/connector_fetch_launch.schema.json    the generated launch-payload schema
tests/connect/test_distributed_ingest.cpp     the five properties, over a real loopback cluster
src/core/testkit/cases_connect.cpp           cluster.distributed_import — the same properties as an in-app, GUI-runnable case
```

## Which connectors take this path

**The four steps below are Snowflake's protocol.** They exist because
Snowflake's SQL API v2 hands back a result *manifest* — a statement handle plus
`partitionInfo[]` — which is what makes a result set tileable: the per-chunk row
counts are the tiling's weights, and each node can fetch its own chunks
directly from the source.

A connector whose source has no such manifest cannot be tiled. Postgres returns
one result set over its own wire protocol; there is nothing to weight a fan-out
by and no way for a second node to pull "chunk 4". `run_distributed_connector_ingest`
dispatches on `config.type` and sends those to
**`run_single_shot_connector_ingest`**: pull the rows through the connector's
own runner, then land them with the same `PartitionScheduler`, the same
`partition_count` / `replication_factor`, the same `partition_only` marker, and
the same fetch-affinity placement. **The dataset it produces is shaped exactly
like the tiled path's** — it just fans out over this node's cores and disks
rather than over the cluster's uplinks.

That is an honest limit of the source, not a shortcut: a source that hands back
one stream can only be read by one node. Distributing it means pushing a keyed
range split into the query itself so each node asks the source for a different
slice — a real slice, deliberately not built here.

> Before this dispatch existed, **every** config went through the Snowflake
> steps. A Postgres connection built a Snowflake REST request out of its empty
> `account` and failed with `snowflake: could not resolve
> .snowflakecomputing.com` — a vendor the caller never named. The bug was
> invisible for as long as the consent gate refused `create_dataset_from_query`
> before it reached the handler
> ([#994](https://github.com/oramasearch/zetta/issues/994),
> [#995](https://github.com/oramasearch/zetta/issues/995)). Pinned by
> `DistributedIngest.ANonSnowflakeConfigNeverTakesTheSnowflakeProtocol` and, end
> to end against a real database, by
> `PostgresConnector.DistributedIngestLandsARealPostgresResultSet`.

## The four steps (Snowflake)

```
1. SUBMIT   one node POSTs the statement  ->  statementHandle + partitionInfo[]
                                              (partition 0 arrives inline)
2. LAUNCH   carry (handle, roster, row counts, THE PARTITION PLAN) to every
            participant                       — a `run.launch` task, no credential
3. FETCH    each node takes its own tile from the SHIPPED plan and pulls ONLY
            that, several requests in flight (bounded)
4. LAND     each node writes its slice as partition-only partitions IT owns
            — no re-shuffle, rows never cross the wire twice
```

**SUBMIT.** `submit_connector_statement` runs once, on the initiator. It reads
back the statement handle, the column shape, and `partitionInfo[]` — the
per-chunk row counts that are the tiling's weights. Partition 0's rows come back
*inline with the submission*, so the initiator already holds them.

**LAUNCH.** The initiator snapshots the roster (the collective participants:
this node plus every connected peer, node-id ascending, identical on every
member), creates the target database + table, and fans out one `run.launch` task
per peer through the existing
[coordinated-launch](../compute/coordinated-launch.md) machinery — a registered
runner (`connector_fetch`), not a second synchronization path. The envelope
carries the handle, the row counts, the target ids, the **partition plan**, and a
**saved-connection name**. It does not carry a token, an account, or a URL.

**FETCH.** The plan comes from the coordinator's
[partition scheduler](partition-scheduler.md) — how many partitions the dataset
gets, what size each targets, and which node owns each one. Each participant
re-validates it at the boundary, tiles the result with
`plan_fetch_assignments(roster, partition_rows, the plan's per-node shares)`,
takes the slice at its own rank, and reads only that. Within a node the reads
overlap (`runtime::when_all` over a bounded wave); with no async client installed
the same slice is walked serially.

**The tiling used to be derived, not transmitted** — a pure function of (roster,
row counts), identical on every node, so nothing crossed the wire. That property
is **gone, deliberately**: capacity-aware placement reads live telemetry, which
differs per node by construction, so a plan each participant derived for itself
would not agree with any other's. The coordinator computes it once and ships it;
participants use what they received and nobody recomputes. `partition_count` and
`replication_factor` are no longer separate payload fields — both live in the
plan.

**Right-sized, so nothing is rebalanced.** A Snowflake result set is a
**flexible** source: its chunks are freely assignable, so weighting the tiling by
the plan's per-node shares makes each node pull exactly the rows it will own and
write exactly the partitions the plan gave it. The bodies land at the size the
scheduler targeted and there is no post-download shuffle. The automatic
rebalance only fires when the source genuinely forces a mismatch — a
**fixed**-unit source, which is a later slice. See
[partition-scheduler.md](partition-scheduler.md).

**LAND.** Each node adds its rows to the shared table and writes them out with
[`FetchAffinityPlacement`](fetch-affinity-placement.md), so every partition it
produces is owned by it. The table is
[partition-only](partition-only-tables.md), so the rows are never captured into
the CRDT and nothing re-replicates to the coordinator.

## Why partition 0 is never tiled

`plan_fetch_assignments` deliberately differs from
`plan_partition_assignments`: it tiles `[1, N)` and leaves partition 0 with the
initiator. Partition 0's bytes arrived with the submission response — tiling it
would either fetch the same bytes a second time or throw away bytes already
paid for. Everything else is split by the same row-balanced,
longest-processing-time-first rule over the whole roster, initiator included.

## No credential on the wire

[Saved connections](connector-query.md#saved-connections-the-credential-lives-on-every-node)
are replicate-to-all precisely so that each node can present the credential
**itself**. The launch names one; every participant resolves it from its own
copy of `zetta_connect.connections`. A participant with no such connection
declines — and, because a silent decline would leave the rest of the roster
waiting out the whole timeout, it declines *by reporting in with a zero*.

This is also why an **inline connector config degrades to the local path**: with
no saved connection there is nothing a peer could present, so the initiator
pulls the whole result itself.

## All or nothing

A node dying mid-fetch **fails the whole ingest**. The participants agree
through a single `allreduce` over the launch id: each contributes
`{landed ? 1 : 0, rows}`, and the ingest succeeds only when the first column
sums to the **full roster size**. One comparison covers every failure mode:

| What happened | How the agreement sees it |
| --- | --- |
| a node stopped / became unreachable | its contribution never arrives; the round times out |
| a node's fetch failed | it contributes 0; the sum is short |
| a node never received the launch | same as unreachable |
| membership shrank mid-ingest | the survivors' sum is short of the snapshotted roster |

On failure **every** node discards the ingest — every catalog entry for the
table it can see, the bodies those entries reference, and the table itself — so
the dataset either exists complete or does not exist. It is a *cache*:
re-running the ingest is the expected remedy. Orphaned tiles are deliberately
**not** reassigned; that would require tracking assignments rather than deriving
them, and is a later optimization.

Each node clears what *it* can see rather than only what it wrote, because a
removal converges most reliably when the node holding a key removes it itself.

### Residue

Cleanup is asserted on every surface a caller reads: after a failed ingest the
table is gone and nothing by that name appears in `system.datasets` on any node.

Orphaned `part:` / `lo:` keys used to be possible here. A removal could be undone
when a peer's gossip merge carried a copy of the document that still held the key,
which this path provokes by construction: the coordinator cleans up while
participants are still writing their own entries, so its removals are *necessarily*
concurrent with writes it has not seen.

That was a property of the replication layer, not of the ingest — it applied
equally to `drop_table`, `drop_job` and every other `remove` — and it is fixed
there: removal now replicates as a **tombstone** and **wins** over a concurrent
write, so a key removed here is gone on every node whatever order the merges
happen in. See [../cluster/replication.md](../cluster/replication.md) →
*Removal replicates as a tombstone*
([#838](https://github.com/oramasearch/zetta/issues/838)), covered by
`tests/cluster/test_drop_replication.cpp`.

## Adopting the initiator's table

Every participant must land into the **same** table, so the initiator mints it
and the launch carries `(database_id, database_name, table_id, table_name)`.
`Node::adopt_replicated_table` **captures** those keys locally under the
initiator's ids rather than only materializing them, for a reason worth
remembering: a launch is a signal and arrives at once, while `db:`/`tbl:` merge
on the next gossip round — and `reconcile_databases` **prunes a local database
or table the document does not have**, which would delete the directory, the
rows, and the partition bodies underneath it. Writing the identical value under
the identical key is idempotent; when the initiator's copy merges there is
nothing to reconcile. The same reasoning applies in reverse on failure: a peer
that captured `tbl:<id>` must also capture its removal, because the initiator's
`remove` is *concurrent with* the peer's `set`, not causally after it.

## Row ranges are per node

A partition's `row_range` is its cumulative slice **in the writing node's own
row space**, so ranges from different nodes overlap. Nothing depends on them
being globally unique: `system.datasets` sums range *sizes*, and the
[scatter-gather SELECT](scatter-gather-select.md) uses `row_range.start` only to
order the partitions it plans. An ingested result set is an unordered set of
rows.

## Fetch-affinity partitions never rebalance

The [between-jobs rebalance](partition-rebalance.md) recomputes rendezvous
placement on a membership change. That is right for a hash-placed partition and
wrong for a fetch-affinity one, whose owner is *the node holding the bytes*. So
`write_partitioned` records the placing partitioner in the catalog entry's
`stats.placement`, and `rebalance_partitions` skips `fetch_affinity` entries. At
RF=1 an owner leaving means "re-run the ingest", not "re-place the catalog
entry".

## Degrading

| Situation | What happens |
| --- | --- |
| a single-node cluster (or no attached cluster) | the local path: this node pulls it all and lands it as its own partitions |
| a single result partition | it arrived inline; there is nothing to fan out |
| an inline connector config instead of a saved connection | the local path (peers hold no credential) |
| a node with no `run.launch` listener | the local path |
| no async HTTP client installed | still co-launched; each node walks its own tile serially |

The landed dataset is the same shape in every case — partition-only,
fetch-affinity, RF=1 — so a one-node and an N-node ingest differ only in who did
the pulling.

## Observability

Every participant records a `ComputeJob` of kind **`ingest`**, so the work
surfaces in `system.jobs` beside the featurize/train/score jobs. Its detail
names how many result partitions that node pulled and how many rows it landed;
a failed ingest records the reason. The initiator's job id comes back in
`DistributedIngestResult::job`, and `create_dataset_from_query` returns
`coordinated` + `participants` so a caller can see the fan-out happened.

## What the tests prove

`tests/connect/test_distributed_ingest.cpp` runs a real three-member loopback
cluster with a fake warehouse installed as the **process-wide default**
transport (that is how a peer reaches a source — it never sees the initiator's
injected argument), and asserts:

1. **Exactly-once cluster-wide** — every source chunk read exactly once, chunk 0
   never re-read, and the reads disjoint across the three fetching threads.
2. **Land where you pulled** — each node holds exactly the rows of the chunks it
   fetched, and the catalog's owner for those rows is that node.
3. **Rows never cross the wire twice** — the replicated document holds no `row:`
   entry for the table, on any node.
4. **Readable and correct** — a scatter-gather `SELECT` returns all rows exactly
   once and merges its aggregates to the same totals as the single-node
   reference over the same input. The read is patient: the coordinator can only
   plan over partitions whose catalog entries have reached it, and a `SELECT`
   run before then answers **short without an error** — worth knowing, and the
   same replication-convergence dependency the *Residue* note above describes.
   (Convergence, not durability: a partition entry that has not arrived yet will,
   and one that was removed stays removed — see *Residue*.)
5. **A participant that stops mid-fetch fails the ingest** — with a
   "re-run the ingest" reason, no dataset reported, and no table or
   `system.datasets` entry left on any node, and the `part:` / `lo:` keys it had
   already published are gone from the document too (see *Residue* above).

The first four of those are also an **in-app test-suite case** —
[`cluster.distributed_import`](../testing/runbooks/test-cases/cluster.distributed_import.md),
runnable from Operations → Test suite and over the `testsuite_run` capability, so
the ingest can be demonstrated on a live cluster rather than only in CI. Property 5
has its own in-app case,
[`cluster.import_participant_loss`](../testing/runbooks/test-cases/cluster.import_participant_loss.md)
— and it needs no fault injection, because the case owns its own loopback nodes and
a node is made idle by not giving it a tick loop rather than by wedging it
([#840](https://github.com/oramasearch/zetta/issues/840)).

See also [connector-query.md](connector-query.md) (the connector + the credential
contract), [partition-only-tables.md](partition-only-tables.md) (why nothing
re-replicates), [scatter-gather-select.md](scatter-gather-select.md) (the read
path), and [../compute/coordinated-launch.md](../compute/coordinated-launch.md)
(the co-launch this rides on).
