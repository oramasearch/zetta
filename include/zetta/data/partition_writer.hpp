// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zetta { class Node; }

namespace zetta::data {

class Database;
class Table;
class DatasetPartition;

// The result of a partitioned write (`write_partitioned`, below). On success
// `partitions` holds the catalog entries created (one per non-empty partition, in
// ascending partition-id order), `rows_written` is the total rows distributed, and
// `error` is empty. On failure `error` carries a human-readable reason and the
// partitions/blobs already created (if any) are listed in `partitions` (the write
// is not transactional in this slice — a partial failure leaves what succeeded).
struct PartitionWriteResult {
    std::vector<std::shared_ptr<DatasetPartition>> partitions;
    std::size_t rows_written = 0;
    std::uint64_t bytes_written = 0;  // body bytes across the partitions written
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// The **partitioned write path** (#511) — rows → partition → blob, placement
// recorded. For a table configured with a `Partitioner` (the operational signal a
// table is partitioned; `Table::set_partitioner`), it:
//
//   1. **assigns each row to a partition** via the table's `Partitioner`
//      (`assign_partition`). A monotonically-increasing surrogate (`_seq` = the
//      row's table ordinal) is stamped onto the assignment record when absent, so
//      a `RoundRobinPartitioner` with its default surrogate field distributes
//      evenly without the caller pre-stamping it (the doc's "the write path stamps
//      the monotonic surrogate"). Other partitioners ignore the extra field.
//   2. **serializes each non-empty partition's rows to bytes** — a **Parquet**
//      body (`zetta::data::serialize_partition_body`): the rows' fields as
//      columns with each row's CRDT id in a `_zid` column beside them, so the
//      body is a self-describing slice of the table that can be read a page at a
//      time and projected column by column. Rows a flat scalar schema cannot
//      express fall back to the previous JSON Lines shape, recorded in the
//      catalog's `stats.format`. See docs/data/partition-body.md.
//   3. **stores each partition's bytes as a LargeObject** in `database`'s LO store
//      (`LargeObjectStore::import_bytes`) — so the body is fingerprinted (SHA-256),
//      its `lo:` metadata rides in the CRDT, and it gossips peer-to-peer like any
//      other large-object body.
//   4. **records placement** as a `DatasetPartition` catalog entry per partition
//      (`Node::create_dataset_partition`, captured into `part:<uuid>` and visible
//      in `system.dataset_partitions`): the `blob_ref` (the body's id), the
//      `row_range`, the `owner_nodes` (from the partitioner's `place_partition`
//      over `members`), and `stats` recording the partition id + row count.
//
// **Row range.** A partition's `row_range` is its **cumulative slice in
// partition-id order** — partition 0 covers `[0, n0)`, the next non-empty
// partition `[n0, n0 + n1)`, and so on — so the ranges tile `[0, rows_written)`
// with no gaps or overlaps (and reduce to the whole table `[0, total)` for the
// single-partition baseline). A `HashPartitioner` scatters a partition's rows
// across the table's original ordinals, which a single `[start, end)` cannot
// represent; the catalog's range names the slice in the partitioned (re-numbered)
// row space instead. Only **non-empty** partitions get a blob + a catalog row, so
// `partitions.size()` is the number of partitions that received data.
//
// It also, as step 5, **records the dataset's partition manifest** on the table
// (`Table::set_partition_manifest`, captured into `mfst:<table-id>`): the number
// of catalog entries the finished dataset has, its total rows and body bytes, and
// the entry ids. That count is what lets a reader tell an incomplete view of the
// catalog from a complete one and FAIL rather than answer short (#837) — see
// docs/data/partition-manifest.md.
//
// `members` is the cluster membership placement is computed over (each a node id,
// e.g. from `Cluster::node_ids()`); when empty it defaults to this node only
// (`{node.node_id()}`), so a standalone call still records a concrete owner. The
// table must have a partitioner configured — otherwise the result carries an
// error and nothing is written.
//
// `record_manifest` is false for one **tile** of a cluster-wide write (the
// cross-node connector ingest, #833 section 7): a tile knows only its own share,
// and a manifest describing a third of the dataset would fail every read of a
// perfectly healthy one. The coordinator writes the manifest there, from the
// cluster-wide totals its allreduce already agrees on. Non-throwing.
PartitionWriteResult write_partitioned(
    Node& node, Database& database, Table& table,
    const std::vector<std::string>& members = {},
    bool record_manifest = true);

}  // namespace zetta::data