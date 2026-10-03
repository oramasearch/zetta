# The object capability

Refs [#1005](https://github.com/oramasearch/zetta/issues/1005) (the decided specification is the
issue's 2026-10-03 comment). Built for lake: liquid-bridge decision 54 places each lake workspace's
liquid store — its repositories, Drive, models, datasets and tenant OCI layers — in that
workspace's Zetta, and lake's liquid `store_backend` writes it through this capability (lake
`docs/architecture/workspace-storage-plan.md`, "The Zetta object capability").

`zetta::data::ObjectStore` (`include/zetta/data/object_store.hpp`,
`src/core/data/object_store.cpp`) keeps byte objects addressed by `(bucket, key)`. It maps one to
one onto liquid's `store_backend`: `put`, `put_range`, `get_range`, `remove`, `list` and `barrier`,
plus usage, a per-bucket quota and an owner lease whose fence every mutation carries.

The large objects of [large-objects.md](large-objects.md) do not fit this use: they are held whole
in memory, capped near 48 MiB, replicated to every node, have no range read and no append, and are
reached only through cookie-authenticated web routes.

## Model

- **Bucket.** Declared once with `create_bucket {bucket, quota_bytes?, rf?}`. Its definition is the
  replicated catalogue key `object_bucket:<name>`, like a record collection's. The call is
  idempotent for an identical definition and answers `409 conflict` for a different one. Names
  follow `^[a-z0-9][a-z0-9_.-]{0,95}$`.
- **Object.** Addressed by `(bucket, key)`, a key of 1 to 1024 bytes, `/` as separator, no control
  characters. Its bytes are node-local, in the bucket's log under `<data_dir>/objects/<bucket>/`;
  they never enter the replicated document.
- **Generation.** Every version of a key has a generation, `max(previous + 1, now in
  microseconds)`. A `put` makes a new one; appends extend the current one. Reads report it in the
  `Zetta-Generation` response header.

## Routes

The byte operations take and return raw bodies, never base64 JSON, and stream in both directions a
slice (at most 4 MiB) at a time; no request holds a whole object in memory. Every mutation takes
the bucket lease's fence as the `Zetta-Fence` header and `?durable=true` to fsync before answering.

| Operation | Route | Contract |
|---|---|---|
| put | `PUT /v1/objects/<bucket>/<key>` | Atomic whole replace: readers see the old bytes or the new, never a mix. A body of one slice is one frame; a larger one is staged on every owner and committed atomically. |
| put if absent | `PUT …?if_absent=true` | Conditional create, decided atomically by the key's primary owner; `409 conflict` if the key exists. Liquid's exclusive repository create (oramasearch/liquid#326). |
| put range | `PATCH /v1/objects/<bucket>/<key>?offset=<n>` | Create, or extend, at exactly `n`. `n` equal to the size appends; a range inside the object equal to the bytes already there succeeds and changes nothing (`"unchanged": true`), so an append can be retried. Any other range is `409 conflict` with `details.size`. A body larger than a slice is applied a slice at a time, each conditional on the last. |
| get range | `GET /v1/objects/<bucket>/<key>`, `Range: bytes=a-b` | Exactly that range, streamed (`206`, `Content-Range`); `bytes=a-` and `bytes=-n` too. `416` with `details.size` and `Content-Range: bytes */<size>` outside the object; `404` when absent. Without `Range`, the whole object (`200`). |
| remove | `DELETE /v1/objects/<bucket>/<key>` | Delete one object (the `delete_object` capability). `404` when absent. |
| list | `GET /v1/objects/<bucket>?prefix=&after=&limit=` | Keys, sizes and update times ordered by key, `limit` 1 to 1000, `next_after` while there is more (`list_objects`). |
| barrier | `POST /v1/objects/<bucket>:barrier` | Answers once every write acknowledged before the call is fsynced on every live node (`object_barrier`). |
| usage | `GET /v1/objects/<bucket>:usage` | `{objects, bytes, quota_bytes}` (`object_usage`). |
| owner lease | `POST /v1/objects/<bucket>:acquire` `{holder, ttl_ms, wait_ms}`, `:renew` `{holder, fence, ttl_ms}`, `:release` `{holder, fence}` | `{bucket, holder, fence, expires_ms}`, or `409 held` naming the holder once the wait ends (`acquire_object_lease`, `renew_object_lease`, `release_object_lease`). |

A write answers `{bucket, key, size, generation, updated_ms, owner, replicas, replicas_expected}`
in the REST envelope. Errors use the registry's envelope and codes: `invalid_input` 400,
`unauthorized` 403, `not_found` 404, `conflict` / `fenced` / `held` 409,
`range_not_satisfiable` 416, `quota_exceeded` 507, `busy` 503 (retry). A refused upload's
unread body is drained (up to 64 MiB) so the client's connection stays usable; past that the
connection closes.

### Registry capabilities

The bucket and lease operations are registry capabilities — a REST route at `/v1/<name>`, an MCP
tool and an OpenAPI operation each — and the bucket routes above are their aliases:
`create_bucket`, `list_objects`, `object_usage`, `object_barrier`, `delete_object`,
`acquire_object_lease`, `renew_object_lease`, `release_object_lease`
(`src/core/api/object_capabilities.cpp`). They are `blocks` entries: authorized and audited on the
owner loop, run on the blocking pool. The byte routes are REST only; their OpenAPI operations
(`objects_put`, `objects_put_range`, `objects_get`, and the aliases) are generated from
`object_route_paths()` beside the capabilities.

`acquire_object_lease` with `wait_ms` waits without holding a thread: its registry entry is an
`AwaitableRead` with `retry_within`, so REST and MCP make one governed call, wait on a lease-change
event or the holder's expiry, and call again, until it succeeds or `wait_ms` passes
([registry-design.md](../api/registry-design.md)).

## Fencing

- The **fence** rises every time a new holder acquires the lease (the same holder re-acquiring a
  lease it still holds keeps it). It is the catalogue key `object_lease:<bucket>` (`holder`,
  `fence`, `expires`, `version`), so a restarted node enforces it too.
- One node decides a bucket's lease (a rendezvous hash of the bucket over the live nodes); it
  records the decision in the catalogue, and `acquire` answers only after every live node has
  installed it (`object.lease_apply`). On each node a write holds the bucket's fence lock shared
  across its check and its append, and an install takes it exclusively, so once a new fence is in
  place no write checked against the old one can still land there.
- Once a bucket has had a holder, every mutation must carry the current fence. One without it, or
  with an older one, answers `409 fenced` with `details.fence`. A lake process that lost its lease
  (a lost renewal, a partition, a successor took over) can never write again. A bucket that never
  had a holder takes writes without a fence.
- `acquire` with `wait_ms` waits for a release or for the holder's expiry: the per-bucket form of
  lake#479's hand-off. `renew` extends the caller's lease; with a fence that is no longer current it
  is `409 fenced`. `release` leaves the fence in place until the next holder raises it.
- Reads need no fence. Liquid's side of the lease is oramasearch/liquid#327.

## Rights

`objects:read`, `objects:write` (the lease too) and `objects:manage` (creating buckets) are
object-scopable by bucket name prefix, like `ON COLLECTION`:

```sql
CREATE ROLE lake_service;
GRANT mcp:call TO ROLE lake_service;
GRANT objects:read, objects:write, objects:manage ON BUCKET lake_ TO ROLE lake_service;
```

Only `admin` holds them by default. Every call is authorized by the bearer and audited on
authorize, as every capability is: the byte routes record `api:get_object`, `api:put_object` and
`api:put_object_range` rows with the decision and the bucket, never the bytes. The routes are in
neither of lake's `workspace_query` nor `workspace_act` allow-lists.

## Durability

A write is acknowledged after `write(2)` on every owner that holds it. The **barrier** is the
durability point: it fsyncs every segment of the bucket written since the last barrier, on every
live node. `durable=true` makes one write fsync before its answer.

Crash safety comes from the log's format. A put is staged and then committed by one commit frame,
so after a kill the object is the old version or the new, never part of either. An append is one
checksummed frame: after a kill it is absent or whole, and everything acknowledged before a
barrier is there. Startup verifies the checksums of the last two segments (the only ones that can
hold bytes never fsynced; a rolled segment is fsynced by the next maintenance step and by any
barrier) and truncates at the first torn frame.

## Placement and replication

- **One-node cluster** (every workspace today): the volume is the only copy; backups are CSI
  snapshots (orama-operator#260).
- **Multi-node cluster:** each object has `rf` owners, the top of a rendezvous hash of
  `(bucket, key)` over this node and its live permanent peers, as
  [sharded-blob-placement.md](sharded-blob-placement.md) places partition bodies. A bucket's `rf`
  is set at `create_bucket`; the default is `min(3, nodes)`.
  - The first owner, the **primary**, orders the key's mutations: fence, quota and condition are
    decided there, under its log's lock. A mutation arriving elsewhere is forwarded to it
    (`object.write`).
  - The node that took the request then installs the decided version on the other owners
    (`object.apply`; an upload's parts go to every owner as they arrive, `object.stage`) and
    acknowledges once all `rf` owners hold it. Otherwise it answers `busy` naming how many hold it;
    the retry is safe (a retried append of the same bytes changes nothing).
  - Reads go to any owner: this node's copy when it is one, otherwise a peer's, pinned to the
    version's generation and read a slice per `object.read`.
  - **Anti-entropy**, every five seconds on the pool: each node asks each peer for the digests of
    the keys it owns (256 XOR buckets of `(key, generation, size, deleted)` per bucket,
    `object.digest`), lists the versions in differing buckets (`object.keys`) and pulls what it
    lacks — a missing tail of the same version as appends, a newer version staged and committed.
    A copy a node no longer owns is dropped once every owner reports that version or a newer one
    (`object.stat`). Membership changes therefore move objects to their new owners, and a lost
    node's objects are repaired to `rf` copies on the survivors.
- The signal payloads are one JSON header line (`object_signal` schema) and then the raw bytes, so
  bytes are base64ed once, by the signal envelope.

## Storage: a segmented log, not the replicated catalogue

[#1231](record-store.md#storage-a-node-local-log-not-the-replicated-catalogue) measured what a
steady write load does to the Automerge document. Objects therefore live in `ObjectLog`
(`src/core/data/object_log.hpp`), one per bucket, in the record log's design:

- **Frames.** `u32 header length, u32 header crc32c, header, payload`. The header holds the kind,
  the key, a generation or upload id, offsets, the write time and the payload's crc32c. A data
  frame carries at most 4 MiB. Segments roll at 64 MiB.
- **Kinds.** An *append* data frame carries bytes and establishes its generation; a *stage* data
  frame belongs to an upload and is visible only once a *commit* frame names the upload; a
  *remove* frame is a tombstone (kept 24 hours, then purged); a *purge* frame drops every version
  of a key at or below its generation (expired tombstones, copies a node no longer owns).
- **Writers.** One at a time per bucket, on a write mutex. The directory's lock is held exclusively
  only to decide a write and to publish it; the `write(2)` between runs without it (writers are
  excluded, so the decision stays valid), so a reader never waits for the disk.
- **Directory.** In memory: each key's generation, size, update time and extents (where each
  stretch of its bytes lies). A read resolves its extents under a shared lock and `pread`s outside
  it, holding the segments it reads from, so a version replaced or compacted meanwhile still
  reads whole.
- **Replay.** Startup collects every frame per key and keeps each key's highest generation that is
  whole (a commit whose staged bytes are not all there is skipped), so the order frames sit in
  does not matter.
- **Compaction.** Dead segments are deleted oldest-first (which keeps a purge frame until no older
  frame of its key remains). A mostly-dead oldest segment has its live frames moved forward; a step
  moves at most 8 MiB and sweeps as many segments as that allows, taking the log's lock once per
  batch rather than once per frame. Maintenance runs on the pool every 100 ms.

## Limits

- Bodies stream; nothing buffers a whole object. There is no per-object size cap beyond the
  volume and the bucket quota.
- **Quota.** A write that would take the bucket past `quota_bytes` answers `507 quota_exceeded`;
  an upload is checked against its `Content-Length` before any byte is staged and again as each
  part arrives, and replacing an object frees its old bytes. On one node the count is exact. On a
  multi-node cluster the primary estimates the bucket's bytes from what every node held at the
  last anti-entropy round divided by `rf`, so a burst of writes across nodes can pass the quota by
  up to five seconds of writes.

What it does not promise, stated so a caller can plan for it:

- **During a membership change** two nodes can disagree about a key's owners for a moment; a write
  forwarded to a node that no longer sees itself as the primary is `busy` (retry), as in the record
  store.
- **A replica that missed earlier bytes** (it was down) refuses the next append of that object
  until anti-entropy repairs it, so that append is `busy` for up to one round.
- **A lease decided while a write is in flight** on another node can see that write land on the
  write's other owners just after the acquire answers: the write was ordered before the takeover on
  its primary. Every write checked after the new fence is installed is refused.
- **A node away longer than the tombstone grace** (24 hours) can bring back an object removed
  meanwhile when it rejoins.
- The anti-entropy digest compares `(key, generation, size)`. A version's bytes never change
  within a generation (appends only add, and a retry must match), so no content hash is computed;
  every frame's payload is checksummed instead.

## What lake's liquid store backend calls

| `liquid::store_backend` | Call | Maps |
|---|---|---|
| `put(key, bytes)` | `PUT /v1/objects/<bucket>/<key>` with `Zetta-Fence` | `200` ok |
| `put_range(key, offset, bytes)` | `PATCH /v1/objects/<bucket>/<key>?offset=<offset>` with `Zetta-Fence` | `200` ok (a retry is `unchanged`); `409 conflict` names the size (see the note below) |
| `get_range(key, offset, size)` | `GET /v1/objects/<bucket>/<key>` with `Range: bytes=<offset>-<offset+size-1>` | `206`; `416` → `errc::corrupt`; `404` → `errc::not_found` |
| `remove(key)` | `DELETE /v1/objects/<bucket>/<key>` with `Zetta-Fence` | `200`; `404` → `errc::not_found` |
| `list(prefix)` | `GET /v1/objects/<bucket>?prefix=<prefix>&limit=1000`, then `&after=<next_after>` until absent | |
| `barrier()` | `POST /v1/objects/<bucket>:barrier` | |
| exclusive repository create (liquid#326) | `PUT …?if_absent=true` | `409 conflict` → exists |
| owner lock (liquid#327, lake#479) | `POST …:acquire {holder, ttl_ms, wait_ms}` at open; `:renew` well inside `ttl_ms`; `:release` on drain | `409 held` → `errc::locked` naming the holder; `409 fenced` on any write → the lease is lost: stop writing |
| storage measurement | `GET …:usage` | |

`401`/`403` map to `errc::unauthorized` and an unreachable node or `503 busy` to
`errc::unavailable` (retry). Lake creates the bucket once per workspace with `create_bucket
{bucket, quota_bytes}` and holds `objects:*` `ON BUCKET` its prefix.

**One liquid write the contract refuses.** Liquid's packs only ever append, and its index flush
puts a run's header and then appends the bloom filter and records. Its index *merge*
(`src/store/index.cpp`, the compaction of several runs) writes a placeholder header and bloom
filter, appends the records, and then rewrites the header at offset 0 and the bloom filter at
`run_header_size` with different bytes. The decided `put_range` contract answers that with
`409 conflict` (a range inside the object must equal the bytes already there). Before lake runs a
store on this backend, the merge has to write the run in order — for example, compute the header
and bloom filter first, or stage the records under a temporary key and `put` the finished run —
or this contract has to grow an in-place overwrite.

## Latency

Measured over loopback HTTP against a file-backed node, Apple M3 Max; each test fails above its
budget (`tests/api/test_object_routes.cpp`). The figures held, within the budgets, with the machine
also running other work at a load average of 12 to 25.

| Case | Budget | Measured |
|---|---|---|
| 1 MiB `PATCH` append, then `barrier`, one node | p99 under 20 ms | p50 1.3 ms, p99 1.6 ms |
| 64 KiB ranged `GET`, one node | p99 under 2 ms | p50 0.11 ms, p99 0.36 ms |
| 4 MiB ranged `GET`, one node | p99 under 15 ms | p50 2.3 ms, p99 3.4 ms |
| Stream a 256 MiB object | ≥ 200 MiB/s, flat RSS | up 1,104 MiB/s, down 1,651 MiB/s; resident memory +0 to 5 MiB |
| Push: 64 MiB as 64 × 1 MiB appends plus one barrier | under 1.5 s | 66 ms |
| Pull: 2,000 random 64 KiB reads by 8 concurrent readers | p99 under 5 ms | p50 0.37 ms, p99 1.1 ms |

A read of a range this node holds is resolved, and its first megabyte read, on the HTTP thread (a
directory lookup and a `pread` from the page cache, as the asset routes read small local files);
the rest of a larger range, and any read that needs a peer, runs on the blocking pool. Before
that change, each read cost two more thread hops and the pull p99 was 1.9 ms on an idle machine
and over 5 ms on a loaded one.

## Soak

`ObjectStoreSoak.DiskAndMemoryFollowTheLiveSetUnderSteadyChurn` (`tests/data/test_object_soak.cpp`)
runs a liquid-shaped load on a node: four writers append 4 MiB packs in 256 KiB appends, seal each
with a barrier, put an index run and a store marker, and remove their oldest packs so each keeps
six (a live set of about 100 MiB), while a reader reads random 64 KiB ranges and the owner loop
ticks. It fails if disk passes 640 MiB at any point or 320 MiB at the end, the directory 4 MiB,
the resident memory it adds 512 MiB, or the read p99 10 ms. `ZETTA_OBJECT_SOAK_BYTES` sets its length; the suite runs 2 GiB.
On an Apple M3 Max: 2 GiB appended at about 1 GiB/s, a live set of 96 MiB, peak disk 400 to
450 MiB (96 MiB at the end, in two segments), a directory of 190 KiB (mostly tombstones inside
their grace), peak RSS 38 MiB, 43,000 reads at p50 0.007 ms and p99 0.1 ms; the replicated
catalogue did not grow. Two changes the soak forced:

- **Compaction took the log's lock once per frame** and could not keep up: the load reached
  1.5 GiB on disk before the writers stopped. Batched, it holds disk near the live set.
- **`write(2)` ran under the log's exclusive lock**, and on a file another thread is fsyncing it
  sometimes stalls 3 to 14 ms, so reads waited too (p99 4 to 11 ms). Writers now serialize on
  their own mutex and hold the directory lock only to decide and to publish, never across the
  write; the read p99 fell to 0.1 ms and the write rate rose from about 700 MiB/s.

## Tests

| Test | Covers |
|---|---|
| `ObjectStoreFixture.BucketsAreIdempotentAndRefuseADifferentDefinition` | bucket definitions, names, key validation |
| `ObjectStoreFixture.PutReplacesTheWholeObjectAndReadsExactRanges` | put, ranges, `416`, `404`, empty objects, `if_absent` |
| `ObjectStoreFixture.PutRangeAppendsAtTheExactOffsetAndARetryChangesNothing` | the exact-offset contract, retried appends, conflicts with the size, ranges larger than a slice |
| `ObjectStoreFixture.ListRemoveUsageAndBarrier` | prefix and paged listing, remove, usage, barrier |
| `ObjectStoreFixture.QuotaRefusesWritesPastIt` | the quota for puts, appends and uploads; replacement frees bytes |
| `ObjectStoreFixture.ConditionalCreateUnderARaceHasExactlyOneWinner` | 8 threads, 20 rounds, one winner each |
| `ObjectStoreFixture.LargePutsAreAtomicForReaders` | a reader beside 10 MiB replacements never sees a mix; an aborted upload leaves the old object |
| `ObjectStoreFixture.FencingRefusesAStaleHolderAfterATakeover` | fences, held, release, renewal keeping the lease, expiry, the fence across a restart |
| `ObjectStoreFixture.ObjectsSurviveARestartAndStayOutOfTheCatalogue` | restart, appends continuing a version, nothing in the catalogue |
| `ObjectStoreFixture.ATornTailLeavesAppendsWholeUpToTheBarrierAndPutsNeverPartial` | 28 torn tails (some with garbage): appends absent or whole and never short of the barrier; a put old or new, never partial |
| `ObjectStoreFixture.CompactionKeepsDiskNearTheLiveSet` | segment retirement and moves, tombstone purge across a restart |
| `ObjectStoreCluster.RfTwoOnThreeNodesSurvivesALossAndAReplacement` | rf = 2 on three real nodes: exactly two copies, reads from any node, a node killed and repaired, a replacement joining with objects moved and old copies dropped, no acknowledged write lost; the fence across nodes |
| `ObjectStoreSoak.*` | the soak above |
| `ObjectRoutes.*` | the raw routes and statuses, the aliases, fencing and the waiting acquire over REST and MCP, the `ON BUCKET` grant and its audit rows, `507`, MCP and OpenAPI, and the six latency budgets |

## Later

- A `system.object_buckets` read view.
- A content hash of sealed objects, if a store ever needs end-to-end verification beyond the
  frame checksums.
- Exact multi-node quota accounting (today an estimate refreshed by anti-entropy).
- Inline repair of a replica that missed bytes, instead of waiting for the next anti-entropy round.
