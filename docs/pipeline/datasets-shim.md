# Simulated `datasets` library shim → IR load step → a (partitioned) table

A sim-Python script can pull data in the HuggingFace way:

```python
from datasets import load_dataset
ds = load_dataset("saas_sample.parquet", columns=["messages", "outcome"])
```

The [simulated-Python frontend](python-frontend.md) lowers `load_dataset(...)` to a
**connect / load** step in the [ExecutionGraph IR](execution-graph.md) (the
`load_dataset` verb maps to `StepKind::Connect`), carrying the **parquet path** and
the optional **column projection** in the node's `python` provenance bag. This page
covers the matching **execution binding** (#521): take that lowered IR node and
reproduce the *meaning* of `load_dataset` with zetta's own primitives — decode the
referenced Parquet bytes with the **native Parquet reader** and populate a
(partitioned) table.

**HuggingFace `datasets` is the fidelity reference only — never run.** zetta does
not import the Python library, fetch from the Hub, or touch the network in the
engine / build / CI (the [new-input principle](../../.claude/skills/new-input/SKILL.md):
re-implement, don't run). The binding lives in `zetta::input`
(`include/zetta/input/datasets_bindings.hpp`), the same shape as the cluster control
binding [`run_add_nodes_step`](python-frontend.md#engine-bindings) and the
[`IndexStepExecutor`](../indexing/index-step-executor.md).

## Acceptance (#521)

The call yields an IR load node the executor runs via the native Parquet reader,
producing a (partitioned) table. Concretely:

```cpp
zetta::input::FrontendResult lowered =
    zetta::input::PythonFrontend{}.compile(
        "from datasets import load_dataset\n"
        "ds = load_dataset('saas_sample.parquet', columns=['messages','outcome'])\n");
const auto* step = lowered.graph.find_node("load_dataset").get();

zetta::input::DatasetLoadResult loaded =
    zetta::input::run_load_dataset_step(*step, node);
// loaded.table  -> the populated table (only the projected columns)
// loaded.rows   -> the row count read from the Parquet file
```

## How a `load_dataset(...)` node lowers

The frontend already maps the verb and captures the arguments — no new lowering is
needed. The node carries:

```json
{ "python": { "call": "load_dataset", "assigned_to": "ds",
              "args": ["saas_sample.parquet"],
              "kwargs": { "columns": ["messages", "outcome"] } } }
```

`dataset_load_spec(step)` reads this into a `DatasetLoadSpec`:

| Field | Read from |
|---|---|
| `source` (path-or-name) | the first positional string argument |
| | …unless it names a builder (`"parquet"` / `"csv"` / `"json"` / `"arrow"` / `"text"`) and a `data_files` / `data_dir` / `path` keyword is present (the HF builder-with-data shape) — then the keyword wins |
| `columns` (projection) | a `columns=` keyword: a list of strings (or a single string); empty = every column |
| `split` | a `split=` keyword (recorded for naming + lineage) |

It returns `std::nullopt` when no usable source is present, so a caller tells
"nothing to load" from "malformed load step".

## How the step executes

`run_load_dataset_step(step, node, resolver, options)`:

1. **Read the spec** (`dataset_load_spec`).
2. **Resolve the source to Parquet bytes** through the **`DatasetResolver`** IO seam.
3. **Decode** the bytes with the native [`parquet::read_parquet_rows`](../data/parquet-reader.md)
   (no Apache Arrow / parquet-cpp) — one JSON object per row.
4. **Project** to the requested columns (the table holds only the named columns; a
   column not present in the dataset fails with a clear reason).
5. **Materialize** into a table named for the dataset, in the `database` from
   `options` (default `"datasets"`), created if absent.
6. **Partition** (optional): when `options.partitioner` is set, the populated table
   is written through the [partitioned write path](../data/partitioned-write-path.md)
   (`write_partitioned`) so it is a *partitioned* table — one
   [`DatasetPartition`](../data/dataset-partitions.md) catalog entry per non-empty
   partition (visible in `system.dataset_partitions`).

The whole binding is **non-throwing** (zetta-core convention): any failure — no
source, an unresolvable source, a malformed Parquet file, a projection naming an
absent column — returns a `DatasetLoadResult` whose `error` says why and writes
nothing further.

### The `DatasetResolver` IO seam

File / network IO lives behind an injectable callable so the engine / build / CI
never reach the network or a Python runtime:

```cpp
using DatasetResolver =
    std::function<DatasetResolveResult(const std::string& source)>;
```

`default_dataset_resolver()` treats `source` as a **local filesystem path** and
reads it. A bare HuggingFace *name* (or any non-file path) fails with a clear reason
— the offline default cannot fetch it. Resolving a Hub name to bytes is a future
resolver a consenting host can inject; the default build stays offline. This mirrors
the cluster binding's injected `Provisioner&` (the host owns any external access).

### The `DatasetStepExecutor`

For running a *graph* that contains a `load_dataset(...)` node, the
**`DatasetStepExecutor`** is a [`StepExecutor`](execution.md) (like
[`ChurnStepExecutor`](../use-cases/churn-step-executor.md) /
[`IndexStepExecutor`](../indexing/index-step-executor.md)) that runs a Parquet dataset-load
connect step through `run_load_dataset_step` — so the graph executes via the native
Parquet reader and produces a `Run` whose `dataset` output references the real,
populated table (a `tbl:` id, not a logical placeholder). Any other step kind — and
any non-Parquet connect verb (e.g. `read_csv`) — delegates to `DefaultStepExecutor`,
so a mixed graph still runs end-to-end. Inject one into a `GraphExecutor`:

```cpp
auto executor = std::make_shared<zetta::input::DatasetStepExecutor>(node);
zetta::GraphExecutor runner(executor);
zetta::ExecutionResult result = runner.execute(node, lowered.graph);
// result.run->outputs().front() == { kind: "dataset", ref: "tbl:<id>" }
```

## No new JSON format

The `python` provenance bag rides inside the IR node's free-form `attributes`,
already validated by the [IR JSON boundary](execution-graph.md) (`ir::from_json`);
the Parquet bytes are decoded by the native reader, itself bounds-checked at the
binary boundary; the rows it produces are stored as ordinary free-form table rows.
This binding parses no new format of its own, so it defines no new schema.

## Scope / later slices

This is the parser-side lowering (already built) plus the **execution binding**.
Out of scope here, behind the same seam: resolving a HuggingFace Hub *name* to bytes
(a network resolver a consenting host injects), a top-level input dispatch that
auto-routes Python vs SQL (a [python-frontend](python-frontend.md) later slice), and
pushing the column projection *down into the decode* (the typed
[`ddf::load_parquet_data_frame`](../data/parquet-reader.md) reader decodes only the requested
column chunks — this binding decodes every column then projects at the row level, the
faithful path for a general table that may carry non-numeric columns).

## Files

- `include/zetta/input/datasets_bindings.hpp` /
  `src/core/input/datasets_bindings.cpp` — `dataset_load_spec`,
  `is_dataset_load_step`, the `DatasetResolver` IO seam + `default_dataset_resolver`,
  `run_load_dataset_step`, and `DatasetStepExecutor` (#521).
- `tests/test_datasets_bindings.cpp` — the #521 acceptance: lowering →
  spec read-back → load via the native reader → column projection → a partitioned
  table → end-to-end through the `GraphExecutor`, plus the error paths.

## See also

- [python-frontend.md](python-frontend.md) — the frontend that lowers
  `load_dataset(...)`, and the sibling cluster + ML engine bindings (#520 / #522).
- [parquet-reader.md](../data/parquet-reader.md) — the native (no-Arrow) Parquet reader this
  binding decodes with (`read_parquet_rows` / `load_parquet_data_frame`, #508).
- [partitioned-write-path.md](../data/partitioned-write-path.md) — `write_partitioned`, how a
  loaded table becomes a partitioned one.
- [execution.md](execution.md) — the `GraphExecutor` / `StepExecutor` seam the
  `DatasetStepExecutor` plugs into.
- the [`new-input`](../../.claude/skills/new-input/SKILL.md) skill — teaching zetta a
  new kind of input (its worked example is exactly `load_dataset`).
