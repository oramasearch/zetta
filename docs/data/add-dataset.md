# Add a dataset from a file

The **"Add dataset" affordance** loads a local `parquet` / `jsonl` / `json` / `csv`
file as a **partitioned table**, so the new dataset shows up in the
[Datasets view](../gui/datasets-panel.md) / [`system.datasets`](system-datasets.md) with
its partitions **placed across the cluster**. It is area-3 work of the
cluster-observability epic
[#575](https://github.com/oramasearch/zetta/issues/575) ([#587]), the *write*
counterpart to the read-only Datasets view (#586): the view shows the datasets a
cluster holds; this is how you put one there from the GUI.

There are two layers, the repo's usual **engine-in-zetta-core / GUI-as-thin-renderer**
split:

- **`zetta::data::add_dataset_file`** (`include/zetta/data/add_dataset.hpp`,
  `src/core/data/add_dataset.cpp`) — the reusable engine backend. No SDL, so it is
  headless-testable (`tests/data/test_add_dataset.cpp`). Its in-memory sibling
  **`add_dataset_rows`** takes already-decoded rows instead of a file — the landing
  path a [connector query](#add_dataset_rows--the-in-memory-sibling-the-connector-query-landing-path)
  uses.
- The headed affordance (`src/gui/app.cpp` + `src/gui/app_datasets.cpp`) — a
  button + drag-drop + a File-menu item that start an **`AsyncDatasetAdd`**
  off the UI thread (#624; see "The headed affordance" below).

## `add_dataset_file`

```cpp
AddDatasetResult add_dataset_file(Node& node,
                                  const std::filesystem::path& path,
                                  const AddDatasetOptions& options = {});
```

It reads `path`, **decodes it by extension** into rows, creates a uniquely named
table in the target database (default `datasets`), adds the rows, and — when
partitioned and non-empty — writes the table through the
[partitioned write path](partitioned-write-path.md) so its partitions are placed
across the cluster. **Non-throwing**: any failure (an unsupported extension, an
unreadable / empty / malformed file, a partition-write error) returns an
`AddDatasetResult` whose `error` says why, instead of throwing across the
zetta-core boundary.

### Formats (`dataset_file::Format`)

`dataset_file::format_from_path` classifies the file by its lowercased extension:

| Extension          | Format    | Decoder                                                        |
| ------------------ | --------- | ------------------------------------------------------------- |
| `.parquet`, `.pq`  | `Parquet` | the native [Parquet reader](parquet-reader.md) (`read_parquet_rows`, no Apache Arrow / parquet-cpp) |
| `.jsonl`, `.ndjson`| `Jsonl`   | one JSON document per line (blank lines skipped)              |
| `.json`            | `Json`    | a JSON array (one row per element) or a single value (one row)|
| `.csv`             | `Csv`     | header row → object keys, cells type-inferred (below)         |
| anything else      | `Unknown` | rejected with a clear reason                                  |

`dataset_file::dialog_extensions()` returns the matching native-dialog filter
string (`"parquet;pq;jsonl;ndjson;json;csv"`).

The JSON/JSONL/CSV decoders are **all-or-nothing**, like
[`import_records`](large-objects.md): a malformed line / document fails the whole
load (nothing half-imported). Parquet bytes are bounds-checked at the binary
boundary by the native reader.

### CSV type inference

CSV has no types, so each cell is inferred into a JSON scalar:

- an **empty** cell → JSON `null`;
- `true` / `false` (case-insensitive) → a boolean;
- a clean **integer** (optional sign, digits, fits in `int64`, **no leading-zero
  ambiguity** — so a zip code like `01234` stays a string, not `1234`) → a number;
- a clean **float** that consumes the whole cell → a number;
- otherwise the **string** verbatim.

Quoted fields are honored (a doubled `""` inside quotes is a literal quote; a comma
inside quotes is literal). The header is the first non-empty line; blank lines
carry no row.

**A CSV that would not load row for row is refused, naming its 1-based line
(#1221)** — it used to be padded with nulls or cut short without a word. The
decoder (`DatasetLineDecoder`, shared by the file, async, upload and object-load
paths) and the pure check `zetta::data::csv_problem(content)` refuse, in the same
words lake uses (lake PR #459):

| Problem | Error |
|---|---|
| a row with more or fewer fields than the header | `line 3: 1 fields, but the header has 2 columns` |
| a header column with no name | `line 1: header column 2 has no name` |
| a header naming a column twice | `line 1: the header names column 'a' twice` |
| a quoted field still open at the end of its line | `line 2: a quoted field is not closed (a row must stay on one line)` |
| no non-blank line at all | `the file has no header row` |

### Source format (#1221)

`AddDatasetResult::source_format`, recorded on the table (`Table::source_format()`,
in `table.json` and in the replicated `tbl:` value), says what the rows were loaded
from: `csv` / `jsonl` / `json` / `parquet` for a file or an upload, the connector type
(`postgres`, `snowflake`) for a connector pull, `rows` for `add_dataset_rows` with no
`AddDatasetOptions::source_format`. It is fixed at create time and surfaces as
`source_format` on `get_dataset`, `list_datasets` and
[`system.datasets`](system-datasets.md); `format` there stays the storage format
(`jsonl`). The `dataset_added` audit note names the source format, so a connector pull
no longer reads as `jsonl`. A table made any other way (SQL `CREATE TABLE`, an engine
result) has none.

**Column order (#1021).** The load also records the source's column order on the
table (`Table::column_order()`, `cols:<table-id>`, `table.json`): the CSV header,
the Parquet schema, the order a JSON or JSON Lines source first writes each key, or
`AddDatasetOptions::columns` when the caller passes one (a connector passes its
query's SELECT list). `get_dataset`, `list_datasets`, `system.datasets` and `SELECT *`
list the columns that way. See [column-order.md](column-order.md).

### Partitioning (placed across the cluster)

The whole point of "as a partitioned table" is that the dataset's slices land on
**specific owner nodes** spread across the cluster — what the Datasets view's
placement overlay and `system.dataset_partitions` then surface. So
`add_dataset_file` configures the table with a
[`RendezvousPlacement`](partitioner.md) partitioner
(`replication_factor` owners each, **default 3, clamped to the node count**) and
writes it through [`write_partitioned`](partitioned-write-path.md):

- the **record→partition** half shards rows across the `P` partitions (FNV-1a of
  the row), and
- the **partition→node** half is rendezvous / highest-random-weight placement, so
  each partition is owned by a deterministic subset of `AddDatasetOptions::members`
  (the cluster membership) — adding/removing a node moves only ~1/N of partitions.

**How many partitions comes from the [partition
scheduler](partition-scheduler.md), not from a constant.** It used to be
`AddDatasetOptions::partition_count = 8` — eight because eight, which is wrong in
both directions at once (eight footers and eight replicated catalog entries for a
4 KB dataset; 25 GB blobs for a 200 GB one). The scheduler targets a partition
*size*, with a floor of partitions per node so every node gets work, and weights
placement by live node telemetry. `partition_count` is now
`std::optional<std::uint32_t>`: absent means auto, a value is a manual override
honoured exactly. The plan it emitted — count, size targets, owners, and its
**rationale** — comes back on `AddDatasetResult::plan`.

### Why the replication factor defaults to 3

It defaulted to **1** until #833. That was harmless only because a partitioned
table *also* replicated every row through the CRDT — belt and braces, and the
belt was doing the work. The moment rows stop being captured
([partition-only tables](partition-only-tables.md)), the factor is the only thing
standing between a dataset and "one copy, no backup", so the durable default is
**3, clamped to the roster** (`min(rf, members)`, and `members` falls back to
this node alone) — a one- or two-node cluster still places successfully instead
of recording owners that do not exist.

The deliberate exception is a **connector result set**: a cache, re-pullable from
its source, which [`create_dataset_from_query`](connector-query.md) passes at
**RF=1** explicitly. Promoting one to the durable factor is a separate,
deliberate act ([promote-dataset.md](promote-dataset.md)).

`RendezvousPlacement` is chosen over `RoundRobinPartitioner` / `HashPartitioner`
precisely because *their* `place_partition` is replicate-to-all (every node owns
every partition) — which is even balance but **not** a spread across the cluster.
On a single-node cluster every partition is owned by that one node (the placement
is still recorded, just not spread — exactly what you'd expect).

`members` defaults (via `write_partitioned`) to this node alone when empty; the GUI
passes `Node::cluster_members()` (the replicated `node:<id>` membership). Set
`AddDatasetOptions::partitioned = false` to load a plain (replicate-to-all) table
instead.

### Naming

The table name is derived from the file's basename (extension stripped, lowercased,
runs of non-alphanumerics folded to `_`, fallback `dataset`) and made unique within
the database (`accounts`, then `accounts_2`, …) so loading the same file twice does
not collide. `AddDatasetOptions::table_name` overrides it.

## `add_dataset_rows` — the in-memory sibling (the connector-query landing path)

Not every dataset starts as a file. `add_dataset_rows` takes **already-decoded
rows** (JSON documents — exactly what `add_dataset_file` decodes a file into) and
materializes them through the *same* create-table → add-rows → `write_partitioned`
→ audit path:

```cpp
#include "zetta/data/add_dataset.hpp"

AddDatasetResult add_dataset_rows(Node& node,
                                  std::vector<nlohmann::json> rows,
                                  const AddDatasetOptions& options = {});
```

This is the landing path a **connector query** uses
([#827](https://github.com/oramasearch/zetta/issues/827)): the row producer's only
job is to yield rows — Zetta shards and places them across the cluster. One
logical dataset in, a distributed table out. Everything downstream is identical to
the file path: the rows land in a uniquely named table, the partitions are
HRW-placed via [`RendezvousPlacement`](partitioned-write-path.md), and the dataset
shows up in `system.datasets` / `system.dataset_partitions` with a
`dataset_added` audit event.

Two small differences from the file path:

- **Naming** — there is no filename to derive from, so `options.table_name` is used
  directly (falling back to `dataset`), still made unique within the database.
- **Format** — the result reports `Format::Jsonl`, since in-memory rows are JSON
  documents (the same shape a `.jsonl` file decodes to). Only the audit note reads
  it; there is no file to classify.

## `add_dataset_content` + `upload_dataset` — a file's text over the API (#1008)

`add_dataset` takes `path`, a file staged in the **node's** import directory and named
relative to it ([import-root.md](import-root.md), #1391: no absolute path, no `..`, no
symbolic link), which a remote caller cannot write. `add_dataset_content` takes the file's **text** instead and
runs it through the very same decoders (`decode_csv` / `decode_jsonl` /
`decode_json`) and the same create-table → add-rows → `write_partitioned` → audit
path:

```cpp
inline constexpr std::size_t kMaxDatasetContentBytes = 16 * 1024 * 1024;  // 16 MiB

AddDatasetResult add_dataset_content(Node& node, std::string_view content,
                                     dataset_file::Format format,
                                     const AddDatasetOptions& options = {});
```

Its control-surface entry is the **`upload_dataset`** action (REST
`POST /v1/upload_dataset`, the MCP tool of the same name):

| Input | | |
|---|---|---|
| `table_name` | string, required | the new table's name: a letter or `_`, then letters, digits and `_`, at most 63 characters, not a SQL keyword |
| `if_exists` | `"fail"` (default) \| `"rename"` | a taken `table_name` is refused with `conflict` (409); `rename` saves under the next free name (`_2`, `_3`, …) and the output's `renamed_from` names the one asked for |
| `format` | `"csv"` \| `"jsonl"`, required | how `content` is laid out (csv: a header row, then rows with as many fields as the header, cells type-inferred) |
| `content` | string, required | the file's text, at most **16 MiB** (16 777 216 bytes) |
| `database` | string | target database, created if absent (default `datasets`); named by the same rule as `table_name` |
| `replication_factor` | integer ≥ 1 | owners per partition (default 3, clamped to the node count) |

The output is `add_dataset`'s: `{database, table, format, rows, partitions[]}`, plus
`renamed_from` when `if_exists: "rename"` moved it to a free name. It is
**authorized exactly like `add_dataset`** (`orchestration:add_dataset`: whoever may add
a dataset from a file may add the same rows from a request) and audited as
`dataset_added`; the table then appears in `list_datasets` / `system.datasets` and can
be given to `run` as `dataset_path: "<database>.<table>"` ([dataset-path.md](dataset-path.md)).

**The cap is 16 MiB of content, and it is a refusal, not a truncation.** Over it the
call answers `invalid_input` (400) naming the cap and pointing at the path built for
bigger files: upload them to a bucket a chunk at a time and load them with
[`load_dataset_object`](#load-from-a-stored-object-1302) (below). A file on the node
(`add_dataset`) or a source connection (`create_dataset_from_query`) also lands larger
data. The
whole text is held in memory and decoded on the owner loop, so the cap bounds both the
request and the stall. To let a capped upload through at all, the HTTP server's
request-body limit was raised from the parser's 64 KiB default to
`2 × 16 MiB + 1 MiB` (`kMaxRequestBodyBytes` in `http_server.cpp`: JSON escaping can
double a CSV's size); a body over that answers **413**. Refusals — oversize, a format
other than csv/jsonl (schema), malformed jsonl (the line is named), a csv with a header
and no rows — create nothing. Parquet is not accepted: it is binary and has no text
form.

**The rules lake applies in front of the workspace, in lake's words (#1221; lake PR
#459)**, so a direct caller gets the same answer from either layer. Each creates
nothing:

- `table_name` / `database` must be a name SQL reads unquoted
  (`zetta::data::dataset_name_problem`): `invalid_input` at `/table_name` or
  `/database`, e.g. `table_name: 'my-table' can hold only letters, digits and
  underscores (not '-'), so SQL can query it`, `… must start with a letter or an
  underscore`, `… is a SQL keyword; choose another name`, `a name is at most 63
  characters`.
- A CSV is checked with `csv_problem` before it decodes: `invalid_input` at
  `/content`, `the CSV file is malformed: line 3: 1 fields, but the header has 2
  columns` (the table above).
- A taken name is **refused, not silently renamed**: `conflict` (409) at
  `/table_name`, `a dataset named 'accounts' already exists in datasets; choose
  another name, or pass if_exists: "rename" to save it under the next free name`.
  This changed the default for every caller; `if_exists: "rename"` keeps the old
  `_2` behaviour on request. `add_dataset` (a file on the node) and the GUI still take
  the next free name.

Tests: `tests/api/test_registry.cpp` (`UploadDataset*`: csv + jsonl land and list,
type inference and quoted cells match the file path, the audit, the conflict and the
`if_exists: "rename"` opt-in, unsafe names, ragged CSV, and every other refusal;
`UploadedDatasetKeepsItsSourceFormat`), `tests/data/test_add_dataset.cpp`
(`RefusesRaggedCsvNamingTheLine`, `DatasetNameProblemMatchesLake`,
`RecordsTheSourceFormatOnTheTable`) and `tests/api/test_rest_server.cpp`
(`UploadDatasetOverRestAcceptsALargeBody`: ~1 MiB over loopback HTTP).

## Load from a stored object (#1302)

`upload_dataset` carries the file in the request, so it stops at 16 MiB. A larger file
goes in two steps: the caller uploads its bytes to a bucket of the
[object capability](object-store.md) a chunk at a time, with
`PATCH /v1/objects/<bucket>/<key>?offset=<n>` (resumable: a resent chunk is accepted
unchanged), then calls **`load_dataset_object`** with the bucket and key. No other
append API exists for datasets: the object store already orders appends at exact
offsets and is replicated, so a chunk may land on any node behind one endpoint.

The object must be a plain csv or jsonl file. Lake seals its drive files with a key
Zetta never holds, so it copies the plaintext into a staging object first
([lake#846](https://github.com/oramasearch/lake/issues/846)).

`load_dataset_object` is a **job** (REST `POST /v1/load_dataset_object`, the MCP tool of
the same name): it answers at once with a run, and the load runs in the background.

| Input | | |
|---|---|---|
| `bucket` | string, required | the bucket holding the object (`valid_bucket_name`) |
| `key` | string, required | the object's key (`valid_object_key`) |
| `format` | `"csv"` \| `"jsonl"`, required | how the object is laid out, as for `upload_dataset` |
| `table_name` | string, required | the new table's name, by `upload_dataset`'s rule |
| `if_exists` | `"fail"` (default) \| `"rename"` | as for `upload_dataset`; with `rename` the run's final log entry names the name asked for |
| `database` | string | default `datasets`, by the same rule |
| `replication_factor` | integer ≥ 1 | owners per partition (default 3, clamped to the node count) |
| `remove_object` | boolean | default false; when true the job removes the object once the run has finished, whether it succeeded or failed. Needs `objects:write` on the bucket |

```json
{"run_id": "9d1c…", "status": "running",
 "poll": "get_run until status is succeeded or failed; the dataset is the run's output"}
```

It is authorized like `upload_dataset` (`orchestration:add_dataset`), and the handler
also asks for **`objects:read` on the bucket** (`authorize_object_route`, the check
`GET /v1/objects/<bucket>/<key>` makes), so a load cannot read a bucket its caller
could not read directly. With `remove_object` it also asks for **`objects:write` on
the bucket**, the right `delete_object` needs, because the job deletes the object; a
key that can only read the bucket is refused before anything starts. These refusals
answer at once and create nothing:

| Case | Code | Path |
|---|---|---|
| a field fails its rule | `invalid_input` | `/<field>` |
| the caller lacks `objects:read` on the bucket | `unauthorized` (`no objects:read grant for bucket '<bucket>'` for a user) | — |
| `remove_object` and the caller lacks `objects:write` on the bucket | `unauthorized` (`no objects:write grant for bucket '<bucket>'` for a user) | — |
| no such bucket | `not_found` | `/bucket` |
| `table_name` taken and `if_exists` is `fail` | `conflict` (`upload_dataset`'s words) | `/table_name` |
| no background action owner in this embedding | `engine_error` | — |

Whether the object exists, and its size, are checked in the job: opening a read can
wait on a peer, and the handler runs on the owner loop.

### The run

The engine entry point is `AsyncObjectDatasetLoad` (`add_dataset.hpp`), modelled on
`AsyncDatasetAdd`'s worker/owner split. `start()` creates the run (project label
`datasets`, graph label `load_dataset_object`) and sets it `Running`; poll it with
`get_run {run_id}`. The run joins the caller's trace, and its `metrics.params` record
the request and the node running it:
`{node, bucket, key, format, database, table_name, rename_if_exists, remove_object}`.

1. **Read and decode, on a runtime worker** (`runtime::spawn`, adopted by the API's
   `BackgroundActions`). `ObjectStore::open_read` resolves the whole object, its size
   is checked, and `ObjectReader::read` reads it `ObjectStore::kSliceBytes` (4 MiB) at
   a time. Lines are cut on `\n`, the partial last line of each slice carried into the
   next, and each line goes to a `DatasetLineDecoder`.
2. **Materialize, on the owner thread.** Every 10,000 rows, and at the end, the worker
   hands the batch over with `Node::run_on_owner` and waits for it to land before it
   reads further. The first batch creates the table and adds it to the run's `reads`
   (zetta#1258), so `delete_dataset` refuses the table while the load runs. Every later
   batch first checks that the database still maps the table's name to the id the
   first batch created; a SQL `DROP TABLE` does not consult `reads`, and a table can be
   dropped and another created under the same name.
3. **Partition and finish, on the owner thread:** the same id check,
   `partition_target_table` (planned by the object's size,
   `AddDatasetOptions::approx_bytes`), the release of the loading node's copy of the
   rows, the `dataset_added` audit event, and the run's output and status.

Owner-thread work never lets an exception out: anything a batch or the finish throws
fails the run (`the load failed: <what>`) instead of unwinding out of the owner loop's
tick, which would have taken the node down or left the worker waiting forever.

**Memory.** Until the partitions are written, every decoded row is held on the loading
node in the table's local store: in memory, and appended to the table's JSONL chunk
files. Once the partitions are written, `Table::release_local_rows` frees both, so a
finished load leaves the loading node holding what every other node holds: the
partition catalog, plus the bodies of the partitions it owns. Nothing reads the local
copy of a partition-only table (a `SELECT`, `system.datasets` and the `dataset_path`
resolver all go to the partitions). Beyond the table the worker holds one slice, one
carry-over line (at most 16 MiB, checked before it grows) and one batch. Every
object-store call (`open_read`, `read`, `remove`) runs on the worker, so the owner loop
never waits on the store.

Paths that still read a table's local store do the same on the loading node as they
already did on every other node: `create_index` and the index step see no rows, and a
search hit carries no row body. SQL `UPDATE`, `DELETE` and `TRUNCATE` act on the local
store only and never reached the partitions.

**The table is partition-only** (#833, `Table::partition_only`): its rows are never
captured into the replicated document; they live in the partition bodies on their
owners (`replication_factor` copies), and a `SELECT` over the table is scatter-gather.
Capturing them row by row, as `upload_dataset` does, made a 67 MiB CSV (1.4 million
rows, one node) take 126 s, add 5.8 GB of node memory and stall the owner loop for
8.5 s; partition-only took 41 s, 1.7 GB and 2.7 s.

The log gets one `info` entry when the read starts
(`loading <bucket>/<key>: <size> bytes as <format>`), one for each further 64 MiB read
(`read 128 MiB of 512 MiB, 1,204,331 rows`) and none more often, because runs are
replicated.

- **Success:** status `succeeded`, one output
  `{kind: "dataset", name: "<table>", ref: "<database>.<table>"}`, and a final entry
  `loaded <rows> rows into <database>.<table> in <n> partitions`, with
  ` (asked for <table_name>; that name was taken)` appended after a rename.
- **Failure:** status `failed`, an `error` entry with the reason, and nothing left
  behind: the table's partition catalog entries and their bodies are removed (as
  `delete_dataset` does), then the table itself, found by its id, so a table someone
  else created under the same name is never dropped. The reasons:

| Reason | Error |
|---|---|
| no object at the key | `no object <key> in bucket <bucket>` |
| over the size limit | `the object is <n> bytes; a dataset loads from at most 1073741824 bytes` |
| a zero-byte object | `the object is empty` |
| a CSV refusal | as the table above, with its line: `line 3: 3 fields, but the header has 2 columns`, … |
| a line that is not well-formed UTF-8 (either format) | `line <n>: not valid UTF-8` |
| a JSONL line that does not parse | `invalid JSON on line <n>` |
| a line over the line limit | `line <n> is longer than 16 MiB` |
| a csv header and no rows, or a jsonl object of blank lines | `the object decoded to no rows` |
| a slice read failing (I/O, or a peer that lost the version) | `reading the object failed at byte <offset>` |
| the partition write | `partition: …`, as `partition_target_table` reports it |
| the name taken while the object was read (`if_exists: "fail"`) | `upload_dataset`'s conflict message |
| the table dropped, or replaced under its name, while the load ran | `the table <database>.<table> was removed while the load ran` |
| something owner-thread work threw | `the load failed: <what>` |
| the API server stopped before the load finished | `the load was stopped before it finished` |
| the node was killed mid-load (set at its next start) | `the node stopped before the load finished` |

With `remove_object`, `ObjectStore::remove` runs after the run reaches either terminal
status, the call `delete_object` makes. It is recorded in the audit trail as that
capability's call (`DataAccess`, target `api:delete_object`, the caller as actor,
detail `bucket <bucket>: <key> removed after load_dataset_object run <run id>`). A
failed removal (a fenced bucket, say) adds a `warn` entry and leaves the status alone.

**UTF-8.** `DatasetLineDecoder` refuses a line that is not well-formed UTF-8 before it
decodes it, so `upload_dataset` and `add_dataset_file` refuse it in the same words. A
row holding such bytes used to throw from the store when it was written, inside the
owner loop.

### A node killed mid-load

A load runs on the node that accepted it. If that node is killed, the run stays
`Running` and the table it was loading stays behind, possibly with partitions already
written. At its next start the node calls `recover_object_dataset_loads`: every
`load_dataset_object` run still `Running` whose `params.node` is this node's id (read
from `data_dir/node_id`, so it survives the restart) is set `failed` with `the node
stopped before the load finished`, and the table in its `reads` is removed with its
partition catalog entries and bodies, as a failed load's is. Runs another node is
loading are left alone. The object is left in place, even with `remove_object`: the
recovery runs on the owner loop and never calls the object store.

A run whose node never comes back stays `Running`, and its table stays; while it does,
`delete_dataset` refuses the table (the run reads it). `delete_run` it, then delete the
dataset.

### Observability

The job opens a span `load_dataset_object` in the caller's trace, with three beneath
it: `load_dataset_object:read` (opening, reading and decoding, wall clock, with the
bytes, slices and rows), `load_dataset_object:materialize` (the owner-thread time spent
landing batches, summed, since landing is interleaved with reading) and
`load_dataset_object:partition` (rows and partitions). They show in `system.spans` and
`get_trace` under the run's `trace_id`.

### Limits

```cpp
inline constexpr std::uint64_t kMaxDatasetObjectBytes = std::uint64_t{1} << 30;  // 1 GiB
inline constexpr std::size_t kMaxDatasetLineBytes = kMaxDatasetContentBytes;     // 16 MiB
```

Every decoded row is held on the loading node (in memory and in the table's chunk
files) until the partitions are written, so the object limit bounds that node's memory
and disk during the load; a partition-at-a-time write would lift it. The line limit
stops a file with no newline from growing the carry-over without bound. For tests,
`ObjectDatasetSource` lowers the object limit (`max_bytes`) and the progress cadence
(`progress_bytes`), and takes two injection points: `wrap_reader` (hold or fail a
read) and `after_partition_write` (fail once the partitions are written).

### Example

```sh
# A bucket for staged loads.
curl -X POST "$ZETTA/v1/create_bucket" -H "Authorization: Bearer $TOKEN" \
  -d '{"bucket":"lake_dataset_loads"}'

# The CSV in two chunks, each at the offset where the previous one ended.
split -b 8m accounts.csv part.
curl -X PATCH "$ZETTA/v1/objects/lake_dataset_loads/l/accounts?offset=0" \
  -H "Authorization: Bearer $TOKEN" --data-binary @part.aa
curl -X PATCH "$ZETTA/v1/objects/lake_dataset_loads/l/accounts?offset=$(wc -c < part.aa | tr -d ' ')" \
  -H "Authorization: Bearer $TOKEN" --data-binary @part.ab

# Load it, removing the staged object afterwards.
curl -X POST "$ZETTA/v1/load_dataset_object" -H "Authorization: Bearer $TOKEN" \
  -d '{"bucket":"lake_dataset_loads","key":"l/accounts","format":"csv",
       "table_name":"accounts","remove_object":true}'
# → {"value":{"run_id":"9d1c…","status":"running","poll":"get_run until …"}}

# Poll until status is succeeded (or failed, with the reason in logs).
curl "$ZETTA/v1/get_run?run_id=9d1c…" -H "Authorization: Bearer $TOKEN"
# → {"value":{"status":"succeeded",
#             "outputs":[{"kind":"dataset","name":"accounts","ref":"datasets.accounts"}],
#             "logs":[…,{"level":"info","message":"loaded 1405770 rows into datasets.accounts in 2 partitions"}]}}
```

Tests: `tests/data/test_add_dataset.cpp` (`DatasetLineDecoder.*`: the decoder gives the
whole-text decoders' rows and refusals, and refuses invalid UTF-8 on every path;
`ObjectLoad.*`: a 9 MiB CSV whose lines straddle slice boundaries, with its progress
entries, spans and released local copy, CRLF and blank lines and an unterminated last
line, the line limit after the table exists, a ragged last row after a landed batch,
invalid UTF-8 after a landed batch, a failure after the partitions were written, the
table dropped or replaced mid-load, a stop mid-load, a read failing at a byte, the
object limit, every other refusal, the rename, and a restart after a kill mid-load;
`ObjectLoadCluster.*`: on three nodes with `replication_factor: 2`, a node that did not
load the dataset lists, counts and selects it) and `tests/api/test_object_routes.cpp`
(`LoadDatasetObject*`: a 20 MiB CSV PATCHed in 8 MiB chunks loads end to end over REST
and `list_datasets` and `SELECT count(*)` agree, the synchronous refusals,
`if_exists: "rename"`, `remove_object` after success and failure, the `objects:read`
check, and `remove_object` refused without `objects:write` and audited as
`delete_object` with it).

## The headed affordance

The GUI reaches `add_dataset_file` three ways, all routed through the existing
[file-ingestion plumbing](large-objects.md) (the `FileIntent` queue drained on the
main thread):

1. **Datasets view button** — `App::draw_datasets` shows an **"Add dataset…"**
   primary button that opens the native file dialog filtered to the dataset
   extensions; its callback enqueues a `FileIntent::Dataset`.
2. **Drag-and-drop** — a file dropped while the **Datasets view is active** is
   ingested as a dataset (`SDL_EVENT_DROP_FILE` routing in `App::run`); a drop
   elsewhere keeps its existing meaning (rows / large object, or the New Project
   n8n artifact while that modal is open).
3. **File → Add dataset…** — the menu item switches to the Datasets view and opens
   the same picker.

`App::handle_incoming_files` handles `FileIntent::Dataset` by starting an
**`AsyncDatasetAdd`** (#624): the decode runs on a runtime worker and
`App::poll_background_ops()` pumps the owner-thread materialize in bounded
slices, so a large file never freezes the window. The Datasets view (navigated
to at launch) shows the live progress — a marquee + row count while decoding, a
real fraction while adding rows — with a **Cancel** that stops the add and
drops the partial table. Completion sets the shared status line
(`"Added dataset <name> (N row(s), M partition(s))"` or the error), selects the
new dataset when the user is still on the view, and records a `dataset_added`
audit event (the activity feed's completion entry). One add runs at a time; a
second drop waits in the ingestion queue. The synchronous `add_dataset_file`
remains the engine entry point for non-interactive callers, sharing the same
decode + materialize + audit phases. It follows the
[Orama design system](../gui/design-system.md) (a charcoal primary button, a
muted drop hint, sentence case).

## No new JSON format / boundary

This path introduces **no new JSON contract** to schema-validate:

- Parquet is decoded by the native reader (bounds-checked binary boundary);
- JSON/JSONL are parsed non-throwing into free-form, schemaless table rows (exactly
  like `import_records` — datasets are schemaless by design);
- CSV is decoded into the same free-form rows;
- placement rides the already-validated [`DatasetPartition`](dataset-partitions.md)
  catalog boundary via `write_partitioned`.

There is no untrusted JSON payload entering as a typed contract here, so there is
nothing new to validate — malformed *files* fail fast at decode with a clear reason
(the boundary equivalent), and the only state written is free-form rows + validated
catalog entries.

## Where it fits

- [datasets-panel.md](../gui/datasets-panel.md) — the Datasets view this affordance feeds
  (and hosts the button).
- [system-datasets.md](system-datasets.md) — the `Node::datasets()` source / SQL
  view the new dataset surfaces in.
- [partitioned-write-path.md](partitioned-write-path.md) /
  [partitioner.md](partitioner.md) — the `write_partitioned` + `RendezvousPlacement`
  this builds on.
- [parquet-reader.md](parquet-reader.md) — the native Parquet decoder.
- [datasets-shim.md](../pipeline/datasets-shim.md) — the sim-Python `load_dataset(...)` sibling
  (Parquet-only, IR-driven); this is the GUI, file-picker, all-formats counterpart.

Natural next slices: a partition-count / replication-factor control in the UI,
streaming a file on the node the way `load_dataset_object` streams an object, a
partition-at-a-time write that lifts the 1 GiB object limit, a Hub-name resolver behind the
`datasets`-shim extension point. The original import format is now retained as
`source_format` (#1221).
