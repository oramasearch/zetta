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

A read (`api:query`), input `{}`:

```json
{"node_id": "…", "scope": "node", "total_bytes": 123456,
 "kinds": {"datasets": 1000, "files": 0, "models": 4500, "indexes": 700, "runs": 100,
           "buckets": 890, "streams": 60, "records": 400, "catalogue": 30, "system": 50},
 "buckets": [{"bucket": "lake_store", "bytes": 800}, {"bucket": "photos", "bytes": 90}]}
```

It is a `blocks` entry: the registry authorizes and audits on the owner loop, then the handler
walks the disk on the blocking pool. The handler reads only the node's configuration (its data
directory and id) and the filesystem, never main-thread Node state, so a large data directory never
holds the tick.

**Scope: the node that answered.** Each node measures its own disk, so the bytes are physical: a
replicated dataset or bucket counts on every node that holds a copy. A cluster's total is the sum of
each member's answer; lake's workspace engines are single-node, so one call is the workspace. Folding
every member's census into one answer through the telemetry exchange (`NodeTelemetryExchange`, which
already carries each member's disk use) is the follow-up for multi-node engines.

## Verify

- `StorageCensus.CountsEachFileOnceInItsKind` and `StorageCensus.AnEmptyOrMissingDirectoryHoldsNothing`
  (`tests/data/test_storage_census.cpp`): a data directory with one entry of every kind, each file
  counted once, each bucket on its own, and an empty or missing directory.
- `StorageCapabilities.ANodeAnswersItsDiskByKind` (`tests/api/test_storage_capabilities.cpp`): a
  started node answers through the registry with every kind, its id and `scope: node`; bytes added
  to a bucket show in `kinds.buckets`, in that bucket and in `total_bytes`.

## Change

A new kind of entity directory: give it a marker check in `entity_kind` and a row above; a new
top-level directory: a branch in `measure_storage`. Keep `storage_kinds()` the one list, so the
output schema and lake's mapping follow it.
