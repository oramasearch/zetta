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
zetta-core seam.

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
inside quotes is literal). The header is the first non-empty line; unnamed header
columns are skipped; a row with fewer cells than the header leaves the missing
columns `null`.

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

`add_dataset` takes `path`, a file on the **node's** filesystem, which a remote
caller cannot write. `add_dataset_content` takes the file's **text** instead and
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
| `table_name` | string, required | the new table's name; a clash gets `_2`, `_3`, … like a re-added file |
| `format` | `"csv"` \| `"jsonl"`, required | how `content` is laid out (csv: a header row, cells type-inferred) |
| `content` | string, required | the file's text, at most **16 MiB** (16 777 216 bytes) |
| `database` | string | target database, created if absent (default `datasets`) |
| `replication_factor` | integer ≥ 1 | owners per partition (default 3, clamped to the node count) |

The output is `add_dataset`'s: `{database, table, format, rows, partitions[]}`. It is
**authorized exactly like `add_dataset`** (`orchestration:add_dataset`: whoever may add
a dataset from a file may add the same rows from a request) and audited as
`dataset_added`; the table then appears in `list_datasets` / `system.datasets` and can
be given to `run` as `dataset_path: "<database>.<table>"` ([dataset-path.md](dataset-path.md)).

**The cap is 16 MiB of content, and it is a refusal, not a truncation.** Over it the
call answers `invalid_input` (400) naming the cap and pointing at the paths built for
bigger data — a file on the node (`add_dataset`) or a source connection
(`create_dataset_from_query`), both of which land data partition by partition. The
whole text is held in memory and decoded on the owner loop, so the cap bounds both the
request and the stall. To let a capped upload through at all, the HTTP server's
request-body limit was raised from the parser's 64 KiB default to
`2 × 16 MiB + 1 MiB` (`kMaxRequestBodyBytes` in `http_server.cpp`: JSON escaping can
double a CSV's size); a body over that answers **413**. Refusals — oversize, a format
other than csv/jsonl (schema), malformed jsonl (the line is named), a csv with a header
and no rows — create nothing. Parquet is not accepted: it is binary and has no text
form.

Tests: `tests/api/test_registry.cpp` (`UploadDataset*`: csv + jsonl land and list,
type inference and quoted cells match the file path, the audit, the name suffix, and
every refusal) and `tests/api/test_rest_server.cpp`
(`UploadDatasetOverRestAcceptsALargeBody`: ~1 MiB over loopback HTTP).

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
streaming large files rather than reading them whole, a Hub-name resolver behind the
`datasets`-shim seam, and retaining the original import format on the dataset
summary (today the summary reports the live on-disk format, `jsonl`).
