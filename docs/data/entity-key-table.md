# The text entity-key table

`zetta::data::store_entity_key_table` / `load_entity_key_mapping`
(`include/zetta/data/entity_key_table.hpp`, `src/core/data/entity_key_table.cpp`),
`Node::entity_key_reference` / `Node::resolve_entity_key_mapping`. Issue
[#1255](https://github.com/oramasearch/zetta/issues/1255), the follow-up to #1202.

## Why

A text entity column (customer ids such as `"cust-00042"`) is keyed for the numeric engine
by a derived id: a stable FNV-1a hash of the text, masked to 53 bits
([dataset-path.md](dataset-path.md)). Every output restores the business id, so a run or a
model keeps the reverse map, one entry per distinct id in the dataset.

That map used to be stored inline, in `RunValue::entity_key_mapping` and in the Model's
`feature_schema.entity_key_mapping`. Every replicated write of the record carried it, and so
did every peer apply, every `system.runs` / `system.models` row and every in-process
`metadata()` copy. A warehouse entity column with millions of ids made each record
megabytes. #1202 kept the API answers small; this keeps the records small.

## What is stored

The entries are rows of the engine-internal record-store collection `_entity_keys`
([record-store.md](record-store.md)), keyed by owner and row number:
`<owner>:<row, six digits>`, where the owner is `run:<id>` or `model:<id>`. A row holds up to
1,024 entries in derived-id order as `{"entries": [[derived_id, "original"], …]}`. The record
keeps a reference:

```json
{"column": "customer_id", "entry_count": 1500, "table": "_entity_keys",
 "owner": "model:2c1f…", "chunks": 2}
```

The rows live in the node-local record log, not in the replicated catalogue. They are written
durably on the writing node and sent to every live peer at once; a peer that was away gets
them by the record store's anti-entropy.

## Writers

Every writer still sets `entity_key_mapping` to the inline form. Two injection points turn it
into a reference, so the ten writers did not change:

- `Node::create_model` converts `feature_schema.entity_key_mapping` before the record is
  captured.
- `Run::set_entity_key_mapping` and `Model::set_feature_schema` call the entity-key store the
  node gives every run and model it materializes (`Run::set_entity_key_store`,
  `Model::set_entity_key_store`).

A reference to another owner's rows is copied under the new owner: a retrained model, or a
solution's master run taking a stage run's mapping, owns what it references. Deleting a run or
a model (`Node::drop_run`, `Node::drop_model`) deletes its own rows and no one else's.

A node with no record store (standalone, no replicated catalogue) keeps the inline form, as
does a write whose rows cannot be stored (the reason is logged).

## Readers

`Node::resolve_entity_key_mapping` reads either form:

- **inline** (`{column, entries}`): parsed as before. This is how records written before #1255
  keep loading: migration on read, no rewrite of existing records;
- **a reference**: the owner's rows read from this node's record log. If some have not reached
  this node yet, because a peer can hold the Model before its rows arrive, it fails and names
  what is missing (`1 of the 2 rows of model:…'s ids … are on this node; the rest have not
  reached it yet …`). It never scores with ids it cannot restore.

The readers are `predict` (`src/core/api/serving.cpp`), `score` with inline rows
(`ml::score_rows` takes the resolved mapping), and `get_run` / `get_model` with
`include_entity_keys: true`, which answer the entries inline exactly as before. Without
`include_entity_keys`, `get_run`, `get_model` and `list_models` answer `{column, entry_count}`
for both forms (`summarize_entity_key_mappings`).

Scoring a table (`score` with `dataset_path`) and `explain` restore ids from the dataset
contract resolved at call time, not from the model, and the distributed segmentation and
next-best-action launches carry the contract's mapping in their job payloads (signal-plane
messages, not replicated state). None of those changed.

## Who can read it

The API refuses every record collection name starting with `_`, so `get_record` and
`list_records` cannot reach `_entity_keys`, for the owner too. The rows are reached only
through the run or model that references them, after the caller passed that capability's
authorization: whoever can read the run or the model can read its ids, and no one else.

## Tests

| Test | Covers |
|---|---|
| `EntityKeyTable.StoresTheMappingAsRowsAndReadsItBack` | 2,500 ids in three rows, the reference's shape and schema, a read back equal to the mapping, after a restart too |
| `EntityKeyTable.AnInlineMappingStillLoads` | the inline form, with and without a record store |
| `EntityKeyTable.AReaderMissingRowsFailsWithAReason` | a missing row, no rows, no record store, a malformed reference |
| `EntityKeyTable.RewritingOrDroppingTouchesOnlyTheOwnersRows` | a smaller rewrite leaves no rows behind; a drop removes only its owner's |
| `EntityKeyTable.TheRecordCapabilitiesCannotReachTheTable` | `get_record` / `list_records` and the store's own `get` / `list` refuse `_entity_keys` |
| `EntityKeyTable.ExpandingReplacesEveryReference` | `include_entity_keys` expansion and the summary of a reference |
| `EntityKeyTableNode.NewRunsAndModelsReferenceTheTable` | `set_entity_key_mapping`, `create_model`, `set_feature_schema`, the replicated records, reads with and without the entries, a copied reference, a restart, deletes |
| `EntityKeyTableNode.RecordsWrittenBeforeTheTableKeepLoading` | an old Run and Model with the inline map, written straight to the catalogue, resolve and read |

## Later

- Mixed-version clusters: a node older than #1255 validates a Run against a schema that knows
  only the inline form, so it refuses a Run carrying a reference.
- A keyed read of one row by derived id (rows are in derived-id order) instead of loading the
  whole map, for a predict of a few ids against millions.
