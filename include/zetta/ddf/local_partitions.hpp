#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/ddf/data_frame.hpp"

// Compute-near-data: load a node's **local shard** for a DDF op straight from its
// **owned partitions** in the catalog, instead of having the shard handed in
// out-of-band (#516, ARC-12 / FEAT-8). This is the placement-aware shard source
// every distributed DDF op (group-by / featurize / filter / scan / …) reads from:
// a node materializes only the partitions it physically holds, runs the op over
// them, and exchanges only the small per-node partial over the
// [collectives](../collectives.md) — the raw rows never cross the wire.
//
// The catalog (`Node::dataset_partitions()` → the read-only
// `system.dataset_partitions` table) records where each table's data lives — its
// `blob_ref` large-object body, `row_range`, and the `owner_nodes` that hold a
// copy. With [sharded blob placement](../sharded-blob-placement.md) (#515) a
// partition body is fetched + held only by its owner node(s), so a node enumerates
// the partitions of a table whose `owner_nodes` include its own id and reads those
// bodies **from local disk** — no `LOREQ`, no full-dataset pull.
//
// **The body format is the existing JsonlStore line shape** — one
// `{"_zid": <id>, "doc": <row>}` JSON object per line — exactly what the
// [partitioned write path](../partitioned-write-path.md) (#511) emits, and what
// `JsonlStore` reads off disk. It is **not** a new JSON format, and the blob is
// already SHA-256-verified at the large-object layer; this loader parses it with
// the same tolerant `{_zid,doc}` / bare-row reading JsonlStore uses, but **fails
// fast** (rather than silently skipping) on a body line that is not valid JSON or
// not a JSON object — a silent drop would diverge from the single-node oracle.
//
// Non-throwing throughout (zetta-core's no-exceptions convention): every entry
// point returns a ComputeStatus alongside its result. See docs/compute-near-data.md.
namespace zetta {
class Node;
}

namespace zetta::ddf {

// The outcome of loading a node's local shard from its owned partitions: the frame
// (populated only when status.ok()) plus instrumentation that makes the near-data
// guarantee observable.
struct LocalShardResult {
    DataFrame frame;                 // this node's local shard, on `device`
    compute::ComputeStatus status;   // explains any failure
    std::size_t partitions_read = 0;  // owned partitions of the table materialized
    std::uint64_t rows_read = 0;      // rows in the local shard
    // Partition-body bytes read **from local disk** building the shard. This is the
    // data read *near* the node — it is NOT moved over the wire (only the op's
    // per-node partial crosses the network — see Collectives::wire_bytes()), which
    // is exactly the near-data property: bytes-moved ≈ partials, not ≈ dataset.
    std::uint64_t bytes_read = 0;

    bool ok() const noexcept { return status.ok(); }
};

// Enumerate the partitions of `table` (the table id the catalog records, i.e.
// `Table::id()`) this node owns — every `DatasetPartition` whose `table` matches
// and whose `owner_nodes` contains `node.node_id()` — read each one's blob body
// from the node's local large-object store, parse its JSON-Lines rows, and load
// them into one typed, chunked DataFrame on `device` per `schema`, in ascending
// `row_range` order (deterministic, so the frame is reproducible). This is the
// node's local shard for a distributed DDF op: compute-near-data, placement-aware.
//
// Partitions this node does not own are skipped (their bodies live elsewhere); a
// partition with no `blob_ref` (an unplaced/empty entry) contributes no rows. A
// table with no owned partitions yields an empty frame (a valid empty shard).
//
// Non-throwing: a failing ComputeStatus (and an empty frame) when an owned
// partition's body is not present locally (the owner should hold it — a missing
// body is surfaced, not silently skipped), a body line is malformed, or the
// underlying load_data_frame rejects a value at its boundary.
LocalShardResult load_local_partitions(
    Node& node, const std::string& table, const DataFrameSchema& schema,
    compute::ComputeDevice& device, std::size_t chunk_rows = kDefaultChunkRows);

}  // namespace zetta::ddf
