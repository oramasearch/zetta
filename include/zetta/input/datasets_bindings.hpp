// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zetta/pipeline/executor.hpp"            // StepExecutor, StepOutcome
#include "zetta/ir/execution_graph.hpp"  // ir::Node

// Sim-Python **`datasets` library shim** (#521). The
// [Python frontend](python_frontend.hpp) lowers HuggingFace-shaped
//
//   from datasets import load_dataset
//   ds = load_dataset("<path-or-name>", columns=["a", "b"])
//
// to a `connect`-stage IR node (the `load_dataset` verb maps to
// `StepKind::Connect`), recording the call's arguments under the node's namespaced
// `python` provenance bag. This is the matching **execution** binding: take that
// lowered IR node and reproduce the *meaning* of `load_dataset` with zetta's own
// primitives — read the referenced Parquet bytes through the **native Parquet
// reader** ([`parquet::read_parquet_rows`](../parquet/reader.hpp), no Apache Arrow /
// parquet-cpp), project to the requested columns, and populate a (partitioned)
// table on the node.
//
// The split — frontend lowers, this binding executes — keeps the frontend a pure,
// non-throwing parser (it never touches the engine or the filesystem) while the
// verb still runs end-to-end, the same shape as the cluster control verb
// `cluster.add_nodes` ([cluster_bindings.hpp](cluster_bindings.hpp)) and the ML
// verbs (`train`/`rank`/`explain`), which run through the
// [`ChurnStepExecutor`](../churn_step_executor.hpp).
//
// **HuggingFace `datasets` is the fidelity reference only — never run.** zetta does
// not import the Python library, fetch from the Hub, or touch the network in the
// engine / build / CI (the [new-input principle](../../.claude/skills/new-input/SKILL.md):
// re-implement, don't run). File/network IO lives behind the swappable
// **`DatasetResolver`** seam: the default resolves a **local filesystem path** to
// its bytes; resolving a bare Hub *name* to bytes is a future resolver a consenting
// host can inject, so the default build stays offline.
//
// **No new JSON format.** The `python` provenance bag rides inside the IR node's
// free-form `attributes`, already validated by the IR JSON boundary
// ([`ir::from_json`](../ir/serialization.hpp)); the Parquet bytes are decoded by the
// native reader, which is itself bounds-checked at the binary boundary; the rows it
// produces are stored as ordinary free-form table rows. This binding parses no new
// format of its own. See docs/pipeline/datasets-shim.md.
namespace zetta {

class Node;
namespace data { class Database; }
namespace data { class Table; }
namespace data { class DatasetPartition; }
namespace data { class Partitioner; }
namespace input {

// What a lowered `load_dataset(...)` IR node asks to load: the dataset `source`
// (the path-or-name), the optional `columns` **projection** (empty = every column),
// and the optional HuggingFace `split` (recorded for naming + lineage). Read from
// the node's `python` provenance bag — no new JSON format.
struct DatasetLoadSpec {
    std::string source;                // the path-or-name to load
    std::vector<std::string> columns;  // projection; empty = all columns
    std::string split;                 // optional HF split (provenance / naming)
};

// Read the load spec from a lowered sim-Python `load_dataset(...)` IR step. The
// `source` is the first positional string argument
// (`load_dataset("file.parquet")`); when the first positional names a builder
// (`"parquet"` / `"csv"` / `"json"` / `"arrow"`, the HF builder-with-data_files
// shape) the `data_files` / `data_dir` / `path` keyword is used instead. `columns`
// reads a `columns=` keyword (a list of strings, or a single string). `split`
// reads a `split=` keyword. Returns std::nullopt when no usable source is present,
// so a caller can tell "nothing to load" from "malformed load step".
std::optional<DatasetLoadSpec> dataset_load_spec(const ir::Node& step);

// True iff `step` is a Parquet dataset-load step this binding should execute: a
// `connect`-stage node whose verb is one of the dataset-load verbs
// (`load_dataset` / `load` / `read_parquet`). Other connect verbs (e.g.
// `read_csv`) are left to the default executor — they are not Parquet.
bool is_dataset_load_step(const ir::Node& step);

// The outcome of resolving a dataset `source` to its raw Parquet bytes. On success
// `bytes` holds the whole file and `error` is empty.
struct DatasetResolveResult {
    std::string bytes;
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// The IO seam: resolve a dataset `source` (a path-or-name) to its Parquet bytes.
// Injected so the engine/build/CI never reach the network or a Python runtime — a
// test passes a fake, a host passes whatever resolver it consents to.
using DatasetResolver =
    std::function<DatasetResolveResult(const std::string& source)>;

// The default resolver: treat `source` as a **local filesystem path** and read it.
// A non-existent / unreadable path — or a bare HuggingFace name, which this offline
// default cannot fetch — fails with a clear reason (no network access). Non-throwing.
DatasetResolver default_dataset_resolver();

// How to materialize the loaded rows. `database` is created if absent (default
// "datasets"). When `partitioner` is set, the populated table is written through
// the [partitioned write path](../partition_writer.hpp) (`write_partitioned`) so it
// is a *partitioned* table — `members` is the cluster membership placement is
// computed over (empty = this node only). With no partitioner the result is a plain
// table.
struct LoadDatasetOptions {
    std::string database = "datasets";
    std::shared_ptr<zetta::data::Partitioner> partitioner;  // null = a plain (un-partitioned) table
    std::vector<std::string> members;          // for write_partitioned placement
};

// The outcome of executing a lowered `load_dataset(...)` step. On success `error`
// is empty, `table` is the populated table, `rows` the count loaded, and (when a
// partitioner was configured) `partitions` lists the catalog entries written.
struct DatasetLoadResult {
    std::shared_ptr<zetta::data::Database> database;
    std::shared_ptr<zetta::data::Table> table;
    std::size_t rows = 0;
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> partitions;  // when partitioned
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// Execute a lowered sim-Python `load_dataset(...)` IR step against `node`: read the
// spec (`dataset_load_spec`), resolve the Parquet bytes (`resolver`), decode them
// with the native Parquet reader, apply the column projection, and populate a
// (partitioned) table per `options`. Non-throwing — any failure (no source, an
// unresolvable source, a malformed Parquet file, a projection naming an absent
// column) returns a result whose `error` says why and writes nothing further.
DatasetLoadResult run_load_dataset_step(
    const ir::Node& step, Node& node,
    const DatasetResolver& resolver = default_dataset_resolver(),
    const LoadDatasetOptions& options = {});

// A `StepExecutor` (the [executor seam](../executor.hpp), like
// [`IndexStepExecutor`](../index/index_step_executor.hpp) /
// [`ChurnStepExecutor`](../churn_step_executor.hpp)) that runs a deployed
// `load_dataset` connect step through `run_load_dataset_step` — so a graph
// containing a `load_dataset(...)` node executes via the native Parquet reader and
// produces a `Run` whose `dataset` output references the real, populated table. Any
// other step kind (and any non-Parquet connect verb) delegates to
// `DefaultStepExecutor`, so a mixed graph still runs end-to-end. One instance binds
// to one `node` + `resolver` + `options`; inject it into a `GraphExecutor`.
class DatasetStepExecutor final : public zetta::pipeline::StepExecutor {
public:
    explicit DatasetStepExecutor(
        Node& node, DatasetResolver resolver = default_dataset_resolver(),
        LoadDatasetOptions options = {});

    zetta::pipeline::StepOutcome run_step(const ir::Node& step) const override;

private:
    Node& node_;
    DatasetResolver resolver_;
    LoadDatasetOptions options_;
    zetta::pipeline::DefaultStepExecutor fallback_;
};

}  // namespace input
}  // namespace zetta
