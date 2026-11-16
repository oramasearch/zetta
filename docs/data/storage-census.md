# Storage census

What a node holds on its disk, by kind. `zetta::data::measure_storage(data_dir)`
(`include/zetta/data/storage_census.hpp`, `src/core/data/storage_census.cpp`) walks the node's data
directory and counts the bytes of every regular file once, in the kind its top-level entry is. The
`storage_usage` capability (`src/core/api/storage_capabilities.cpp`) answers it.

**Why.** A host that bills storage by kind needs the engine to say what it holds. Lake bills a
workspace's datasets, models, indexes, run artifacts, topics, files and buckets as storage
([lake#658](https://github.com/oramasearch/lake/issues/658)); before this, only datasets had a size
(`list_datasets` `size_bytes`) and a model's weights one (`system.models.weights_size`), and nothing
said how much an index, a topic or a run's artifacts took.

## Kinds

The kinds follow the data directory's layout, the same markers `Node::load_*` reads:

| Kind | What is counted |
|---|---|
| `datasets` | a database's directory (`database.json`) except its `large_objects/`, and dataset partitions (`partition.json`) |
| `files` | each database's `large_objects/`: files and blobs |
| `models` | a model's directory (`model.json`, its weights included) and `checkpoints/` (saved versions) |
| `indexes` | an index's directory (`index.json`) |
| `runs` | a run's or a study's directory (`run.json`, `study.json`) and `experiments/` |
| `buckets` | `objects/`, the object store's buckets; `buckets[]` gives each bucket's bytes |
| `streams` | `pubsub/`: the pub/sub topics' logs and spill |
| `records` | `records/`: the record log, records and labelled metrics |
| `catalogue` | every other entity's directory (projects, jobs, deployments, principals, roles, tokens, plans, monitors, workspace profiles) |
| `system` | everything else: `audit/`, `ledger/`, `security/`, `crashes/`, the replication state, the node id, and any directory without a known marker |

Symbolic links are not followed. An entry that cannot be read counts nothing; the walk never fails.
Every kind is present in an answer, 0 when the node holds none of it, and the kinds add up to
`total_bytes`.

## `storage_usage`

A read (`api:query`), input `{}` or `{"scope": "node"}`: the node that answers.

```json
{"node_id": "…", "scope": "node", "total_bytes": 123456,
 "kinds": {"datasets": 1000, "files": 0, "models": 4500, "indexes": 700, "runs": 100,
           "buckets": 890, "streams": 60, "records": 400, "catalogue": 30, "system": 50},
 "buckets": [{"bucket": "lake_store", "bytes": 800}, {"bucket": "photos", "bytes": 90}]}
```

It is a `blocks` entry: the registry authorizes and audits on the owner loop, then the handler
walks the disk on the blocking pool. The handler reads only the node's configuration (its data
directory and id), the filesystem and the telemetry cache (behind its own lock), never
main-thread Node state, so a large data directory never holds the tick.

**Physical bytes.** Each node measures its own disk: a replicated dataset or bucket counts on every
node that holds a copy.

### The whole cluster: `{"scope": "cluster"}`

A clustered node measures its own census on the blocking pool every
`Node::kStorageCensusInterval` (60 s), one walk at a time, and carries the latest in every
`node.metrics` telemetry event it broadcasts (`NodeTelemetryExchange`, beside
`storage_used_bytes`; [node metrics](../cluster/node-metrics.md)) as `storage_census`:
`measured_at` (by its clock), `age_seconds` (how old it was when the event was sent),
`total_bytes`, `kinds` and at most 256 `buckets`, the largest, with the rest counted in
`buckets_omitted` (their bytes stay in `kinds.buckets` and `total_bytes`). The payload is validated
against the wire schema (`schemas/node_metrics.schema.json`) before any field is read.

With `scope: "cluster"` the answering node measures its own disk now and folds in every peer's
census from its telemetry cache (`zetta::cluster::fold_storage_census`):

```json
{"node_id": "node-a", "scope": "cluster", "bytes": "physical", "total_bytes": 370368,
 "kinds": {"datasets": 3000, …}, "buckets": [{"bucket": "photos", "bytes": 270}],
 "nodes": [{"node_id": "node-a", "local": true, "measured_at": "2026-10-11T12:00:00Z",
            "age_seconds": 0, "total_bytes": 123456, "kinds": {…}, "buckets": […],
            "buckets_omitted": 0}, …],
 "missing": [{"node_id": "node-d", "reason": "stale"}]}
```

- `nodes[]`: each member with a fresh census, node-id ascending; `age_seconds` is its census's age
  now (its age when sent plus the event's age), by this node's clock, so clock skew does not matter.
- `kinds`, `buckets` (by name) and `total_bytes`: those members summed.
- `missing[]`: every other member of the cluster membership (the replicated node-list), never
  counted as zero: `not_reporting` (no telemetry within the 8 s freshness window: down,
  partitioned, or a version without the census), `not_measured` (reporting, census not measured
  yet: the first walk is still running) or `stale` (census older than
  `Node::kStorageCensusStaleAfter`, 180 s).
- `bytes: "physical"`: a replicated dataset or bucket is counted on every member that holds it.

A standalone node answers the cluster scope with itself alone. A member that reports a kind this
node does not know adds it to `total_bytes` but not to `kinds`.

**Mixed versions.** The telemetry wire schema is closed, so a node from before zetta#1259 rejects a
newer peer's `node.metrics` event (and its gauges) until it is upgraded; the newer nodes name the
older one `not_reporting`. Roll a cluster's members within a short window.

## Verify

- `StorageCensus.CountsEachFileOnceInItsKind` and `StorageCensus.AnEmptyOrMissingDirectoryHoldsNothing`
  (`tests/data/test_storage_census.cpp`): a data directory with one entry of every kind, each file
  counted once, each bucket on its own, and an empty or missing directory.
- `StorageCapabilities.ANodeAnswersItsDiskByKind` (`tests/api/test_storage_capabilities.cpp`): a
  started node answers through the registry with every kind, its id and `scope: node`; bytes added
  to a bucket show in `kinds.buckets`, in that bucket and in `total_bytes`.
- `StorageCapabilities.AClusterAnswersEveryMembersDiskSummed`: a three-node loopback cluster;
  `scope: cluster` on one node lists all three with their census, sums their kinds, shows a bucket
  written on another member's disk, says `bytes: physical`, and names none missing.
- `NodeTelemetryWire.CarriesTheStorageCensus`, `.CapsTheListedBucketsAndKeepsTheirBytes`,
  `.RejectsAMalformedCensus` (`tests/cluster/test_node_telemetry.cpp`): the census on the wire.
- `StorageCensusFold.SumsFreshMembersAndNamesTheRest`: of five members, two fresh are summed and the
  stale, unmeasured and silent ones are named with their reason.

## Change

A new kind of entity directory: give it a marker check in `entity_kind` and a row above; a new
top-level directory: a branch in `measure_storage`. Keep `storage_kinds()` the one list, so the
output schema and lake's mapping follow it.

## Who sees what (#1430)

`storage_usage` describes a node's whole disk, so only an administrator (`api:cluster_admin`) or
the local session gets all of it. Anyone else gets only the buckets it holds `objects:read` on, the
rule the bucket listing applies. Every kind is still listed, at 0, and `total_bytes` is the sum of
the buckets shown. The same holds for every member in `scope: "cluster"`, and for the cluster
total. Before #1430 any caller of the tool read every bucket's name and size.

