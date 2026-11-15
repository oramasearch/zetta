// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/compute/compute_status.hpp"
#include "zetta/ddf/data_frame.hpp"

// The DDF **distributed equi-join** op (ARC-12; part of the native distributed
// accelerated-compute pass for UC-1 churn, #264 / #382). The first real *op* on
// top of the [DataFrame core](data_frame.hpp) (#380): an inner equi-join of two
// frames on a key column, executed across the cluster.
//
// **The shape.** Each participating node holds a shard of `left` and a shard of
// `right`. The join runs in two stages:
//
//   1. **Repartition by key** so every row whose key falls in partition `p` lands
//      on the node that owns `p`. The cluster has no all-to-all / scatter
//      collective, so the repartition rides the ones it does have
//      ([collectives](../collectives.md), #365): every node `gather`s its rows to
//      the root, the root concatenates them, and a `broadcast` hands the full
//      relation back to everyone — each node then keeps only the rows in its own
//      partition (`hash(key) % participant_count`). Correct-first: a true
//      point-to-point shuffle (sending partition `p` only to node `p`) is a later
//      optimization behind this same API, the way collectives ship a star reduce
//      before a ring/tree.
//   2. **Local join per shard** — a [hash](#hash_join) or [sort-merge](#sort_merge_join)
//      join of this node's left partition against its right partition.
//
// Because equal keys hash to the same partition, every matching `(left, right)`
// pair lands on exactly one node and is emitted exactly once; the **global join
// is the union of every node's returned shard**. A single-node cluster degenerates
// to "keep everything" — one shard that is the whole join — so single-node and
// multi-node runs produce identical results (the acceptance).
//
// **No new wire format.** The repartition carries rows as `std::vector<double>`
// through the existing, schema-validated `CollectiveMessage` — like the
// [job coordinator](../compute/job_coordinator.hpp), this op introduces no JSON of
// its own. The serialized blob is range-checked on the way back in
// (`deserialize_relation`), so a malformed/short broadcast fails with a clear
// status instead of corrupting state. Values ride as float64 (the collective plane
// is double-based); keys/payloads outside float64's exact-integer range lose
// precision — a documented boundary of the double-based compute plane.
//
// Scope of this slice: **inner equi-join** only (left / right / full outer are
// later), the local hash + sort-merge algorithms, and the collective repartition.
// No Node CRUD / SQL surface / ComputeJob `ddf`-kind wiring — the integration test
// drives it through an existing `Ddf` job. See docs/ddf/ddf-join.md.
namespace zetta::compute { class JobRound; }

namespace zetta::compute {

                 // this header stays light; the .cpp pulls in the definition.
}

namespace zetta::ddf {

// Which local join algorithm runs per shard. Both produce the same set of output
// rows (possibly in a different order — use sort_rows() to canonicalize).
enum class JoinAlgorithm { Hash, SortMerge };

// A host-side, value-level view of one relation: a key column plus the payload
// ("value") columns, materialized as float64 and laid out **row-major**. The
// bridge between a typed, device-backed DataFrame and the value-level join — and
// the unit the collective repartition serializes. Move/copy-able (plain data).
struct HostRelation {
    std::string key_name;                       // the join-key column's name
    std::vector<std::string> value_columns;     // payload column names (no key)
    std::vector<double> keys;                    // one key per row (size = rows)
    std::vector<double> values;                  // rows * value_columns.size(),
                                                 //   row-major: row r, column k at
                                                 //   values[r * width() + k]

    std::size_t width() const noexcept { return value_columns.size(); }
    std::size_t row_count() const noexcept { return keys.size(); }
};

// The result of a join: the output columns (the key, then the left value columns,
// then the right value columns) and the joined rows, row-major float64. Plain data.
struct JoinedRows {
    std::vector<std::string> columns;   // key_name, left values…, right values…
    std::vector<double> data;           // row_count * columns.size(), row-major

    std::size_t width() const noexcept { return columns.size(); }
    std::size_t row_count() const noexcept {
        return columns.empty() ? 0 : data.size() / columns.size();
    }
};

// The outcome of `to_host_relation`: the relation (populated only when ok) plus the
// status that explains any failure. Mirrors DataFrameResult.
struct HostRelationResult {
    HostRelation relation;
    compute::ComputeStatus status;
    bool ok() const noexcept { return status.ok(); }
};

// The outcome of `distributed_join`: **this node's shard** of the global join (its
// partition) plus the status. The global join is the union of every node's `rows`.
struct DistributedJoinResult {
    JoinedRows rows;
    compute::ComputeStatus status;
    bool ok() const noexcept { return status.ok(); }
};

// Materialize `frame`'s `key_column` plus every other column (as the value columns,
// in frame order) into a host relation of float64. Non-throwing: an absent key
// column, an empty/deviceless frame with rows, or a device read error yields a
// failing status. Each column is widened to double regardless of its dtype.
HostRelationResult to_host_relation(const DataFrame& frame,
                                    std::string_view key_column);

// Inner equi-join `left ⋈ right` on the key, in memory. Builds a hash multimap on
// `right`'s keys and probes it with `left`'s rows; duplicate keys on either side
// produce the full cartesian product of the matching rows (classic inner join).
// Output columns: left.key_name, then left.value_columns, then right.value_columns.
JoinedRows hash_join(const HostRelation& left, const HostRelation& right);

// Inner equi-join `left ⋈ right` on the key, in memory, via sort-merge: sort both
// sides by key, then merge equal-key groups (their cartesian product). Produces the
// same set of rows as hash_join (a different order before sort_rows()).
JoinedRows sort_merge_join(const HostRelation& left, const HostRelation& right);

// Dispatch to hash_join / sort_merge_join by `algorithm`.
JoinedRows local_join(const HostRelation& left, const HostRelation& right,
                      JoinAlgorithm algorithm);

// Sort `rows` into a canonical order (lexicographically by each row's doubles), so
// hash-vs-sort-merge and single-vs-multi-node results compare as equal sets.
void sort_rows(JoinedRows& rows);

// The partition (0 .. participant_count-1) a `key` belongs to. Deterministic and a
// pure function of the key value, so equal keys always share a partition on every
// node. participant_count must be > 0 (a single participant => always 0).
std::size_t partition_index(double key, std::size_t participant_count);

// Keep only the rows of `relation` whose key is in partition `index` of
// `participant_count`. The filter applied after the broadcast so each node joins
// just its shard.
HostRelation filter_partition(const HostRelation& relation, std::size_t index,
                              std::size_t participant_count);

// Serialize a relation's rows to the flat float64 form the collective plane carries:
// row-major [key, value_0, … value_{width-1}] per row, width = 1 + relation.width().
// Names do not travel — every node knows them from its own (SPMD-identical) schema.
std::vector<double> serialize_relation(const HostRelation& relation);

// The boundary inverse of serialize_relation: rebuild a relation from a flat blob,
// re-labeling with the (locally known) `key_name` + `value_columns`. Range-checked —
// nullopt when the blob length is not a whole number of rows of width
// 1 + value_columns.size(), so a malformed/short broadcast fails fast.
std::optional<HostRelation> deserialize_relation(
    const std::vector<double>& blob, std::string key_name,
    std::vector<std::string> value_columns);

// Distributed inner equi-join. Run identically on every participant (SPMD): each
// supplies its own shard of `left` (joined on `left_key`) and `right` (joined on
// `right_key`). Repartitions both by key across the cluster via `round`'s
// collectives, then does a local `algorithm` join on this node's partition,
// returning this node's shard of the global join. `tag` names the collective rounds
// (use a distinct tag for joins that run concurrently on the same nodes).
//
// Failing status (and an empty shard) on: a missing key column, a device read
// error, a malformed broadcast, or a collective failure (unreachable peer /
// timeout). `round` must be inside a running coordinator (its collectives wired).
DistributedJoinResult distributed_join(compute::JobRound& round,
                                       const DataFrame& left,
                                       std::string_view left_key,
                                       const DataFrame& right,
                                       std::string_view right_key,
                                       JoinAlgorithm algorithm = JoinAlgorithm::Hash,
                                       std::string_view tag = "join");

}  // namespace zetta::ddf
