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
  headless-testable (`tests/data/test_add_dataset.cpp`).
- The headed affordance (`src/gui/app.cpp` + `src/gui/app_operations.cpp`) — a
  button + drag-drop + a File-menu item that call `add_dataset_file` on the UI
  thread.

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
(`AddDatasetOptions::partition_count` partitions, default 8; `replication_factor`
owners each, default 1) and writes it through
[`write_partitioned`](partitioned-write-path.md):

- the **record→partition** half shards rows across the `P` partitions (FNV-1a of
  the row), and
- the **partition→node** half is rendezvous / highest-random-weight placement, so
  each partition is owned by a deterministic subset of `AddDatasetOptions::members`
  (the cluster membership) — adding/removing a node moves only ~1/N of partitions.

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

`App::handle_incoming_files` handles `FileIntent::Dataset` by calling
`add_dataset_file(node_, path, {.members = node_.cluster_members()})`, then sets the
shared status line (`"Added dataset <name> (N row(s), M partition(s))"` or the
error), selects the new dataset, and switches to the Datasets view — where it
appears immediately, since the view recomputes from `Node::datasets()` every frame.
The add runs on the main thread, which owns the node, like every other ingestion
path. It follows the [Orama design system](../gui/design-system.md) (a charcoal primary
button, a muted drop hint, sentence case).

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
