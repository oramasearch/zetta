# Column order and typed columns (#1021)

A dataset lists its columns **in the order its source gave them**, each with a
type and the share of rows that hold null for it. `get_dataset` answers
`columns: [{name, type, nullable, null_fraction}]`; `list_datasets`,
`system.datasets` and `SELECT *` list the names in the same order.

## Why it needs recording

The engine stores every row as a JSON object, and a parsed object reads its keys
back **sorted**. So before #1021 a CSV whose header was `account_id, plan,
tenure_months, churned` came back from `get_dataset` and `SELECT *` as
`account_id, churned, plan, tenure_months`, and lake's dataset page showed it that
way (lake#812). The order is therefore recorded once, when the rows are loaded,
next to the table rather than in the rows.

## Where the order comes from

| Source | Order recorded |
| --- | --- |
| CSV (`add_dataset`, `upload_dataset`, `load_dataset_object`) | the header |
| Parquet (`add_dataset`) | the file's schema: its leaf columns, as the footer lists them |
| JSON Lines (the same three paths) | the keys of the first row, in the order the line writes them; a key first seen in a later row follows, in that row's order |
| JSON (an array of objects, or one object) | the same rule over the elements |
| A connector query (`create_dataset_from_query`, Postgres or Snowflake) | the query's SELECT list, as the source returned it |
| `CREATE TABLE t (a …, b …)` | the column list |
| `add_dataset_rows` (in-memory rows) | `AddDatasetOptions::columns` when the caller passes one; otherwise nothing (an in-memory object holds no order) |

A caller can override the decoder's order with `AddDatasetOptions::columns`; the
connector paths do, with the query's columns. CSV and JSON Lines are decoded a line
at a time by `DatasetLineDecoder`, which records the order as it goes; an object
load (`load_dataset_object`, #1302) creates its table with the order seen by the
first batch and sets it again from the whole object before partitioning, so a
JSON Lines key first written after the first batch is not lost.

## Where it is kept

- `Table::column_order()` — the recorded names. `set_column_order` captures them
  under the CRDT key **`cols:<table-id>`** (value `{columns: [name, …]}`) and
  writes `column_order` into `table.json`. One writer: the node that loaded the
  rows (the ingest initiator, for a co-launched Snowflake pull), the same rule as
  the [partition manifest](partition-manifest.md). It is its own key rather than a
  field of `tbl:` because the order is known only after the table exists, and
  every participant of a co-launched ingest writes `tbl:` when it adopts the table.
- Reconcile applies `cols:` like `mfst:` (materialize-only, cleared when the key
  is absent); a `tbl:`, `mfst:` or `cols:` change brings the other two into the
  batch. `migrate_to_replication` captures an order found in `table.json`.
  `Database::drop_table` removes the key with the table.

## How it is read

`zetta::data::order_columns(recorded, seen)` lists the recorded names first,
then any column a row carries that the order does not name (in first-seen
order). `Node::datasets()` (behind `get_dataset`, `list_datasets` and
`system.datasets`) and `SELECT *` over both replicated and partition-only tables
use it.

Every summary carries **`column_order`**: `"source"` when an order was
recorded, `"first_seen"` when none was.

## Datasets loaded before #1021 keep today's behaviour

A table loaded before this change has no recorded order, and nothing re-derives
one: its columns stay in **first-seen order**, which for stored rows is
alphabetical, and its summary says `column_order: "first_seen"`. Reload the file
(or re-run the connector query) to get the source order.

## Types and null shares

`zetta::data::ColumnProfile` (`include/zetta/data/dataset_columns.hpp`) types
each column by the JSON family its values share — the vocabulary a JSON Lines
partition body already records:

| `type` | meaning |
| --- | --- |
| `boolean`, `integer`, `number`, `string`, `object`, `array` | every non-null value is that family (an integer column that meets a real number widens to `number`) |
| `mixed` | values of several families |
| `null` | no value but null |
| `unknown` | only for a partition-only dataset whose partitions predate recorded types |

`nullable` is true when some row holds null for the column or omits it.
`null_fraction` is those rows over all rows.

**A column the caller may not read** (a column-scoped `sql:read`,
[access control](../governance/access-control.md)) keeps its name and answers
`type`, `nullable` and `null_fraction` as JSON `null`, the way SQL masks its
values: nothing about them reaches a caller who cannot read them. `list_datasets`
and `system.datasets` give names only.

- **A replicated table** is profiled from its rows, in the same pass that learns
  its columns.
- **A partition-only table** has no local copy of its rows on most nodes, so each
  partition records **`column_stats: [{name, type, nulls}]`** in its catalog
  stats when it is written (`write_partitioned`), and the profile sums them. A
  partition written before #1021 has only its body `schema` (types, and whether a
  column is optional); then the type comes from the schema and **`null_fraction`
  is `null`** (unknown), never guessed.

## Tests

- `tests/data/test_add_dataset.cpp` — `KeepsTheSourceColumnOrder` (CSV header,
  JSON Lines and JSON key order, a caller's order, no order for in-memory rows,
  the `cols:` key, a reconcile, drop, and a restart into a fresh document) and
  `AParquetFileKeepsItsSchemaOrder`; `ObjectLoad.KeepsTheSourceColumnOrder` (an
  object's CSV header, and a JSON Lines key that first appears after the first
  10,000-row batch).
- `tests/data/test_query.cpp` — `CreateTableColumnListIsTheSelectStarOrder`.
- `tests/query/test_distributed_select.cpp` —
  `APartitionOnlyDatasetKeepsItsSourceColumnOrder` (types and null share from the
  partition stats) and the two-node test (the order replicates to the node that
  did not load the dataset).
- `tests/api/test_registry.cpp` — `get_dataset`'s typed `columns` and
  `list_datasets`' names, with and without a recorded order;
  `GetDatasetMasksTheTypesOfColumnsTheCallerMayNotRead`.
