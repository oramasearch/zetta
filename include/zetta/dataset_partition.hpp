#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

class ReplicationManager;

// A half-open range of row ordinals [start, end) covered by a partition. The row
// ordinals are the table's logical row positions, so a partition names a
// contiguous slice of a table without copying its keys.
struct PartitionRowRange {
    std::uint64_t start = 0;
    std::uint64_t end = 0;  // exclusive
};

// The replicated/on-disk value of a dataset partition — everything the CRDT key
// `part:<uuid>` carries (the id is the key / folder name, so it isn't repeated
// here). This is exactly the shape validated by the DatasetPartition JSON Schema
// at every boundary (disk + the document).
//
// A partition records **where a slice of a table's data physically lives** (ENT-36
// / TBL-13): which `table` it belongs to (a `tbl:<uuid>` id), the `blob_ref`
// large-object body (a `lo:<uuid>` id) holding its bytes, the `row_range` it
// covers, free-form column `stats` for partition pruning, and the `owner_nodes`
// that hold a copy. The catalog is **replicate-to-all** so placement is globally
// visible — every node sees the same partition set.
struct DatasetPartitionValue {
    std::string table;              // owning table id (tbl:<uuid>); required
    std::string blob_ref;           // large-object body id (lo:<uuid>) (optional)
    PartitionRowRange row_range;    // [start, end) row ordinals this slice covers
    nlohmann::json stats =          // free-form column stats (for pruning)
        nlohmann::json::object();
    std::vector<std::string> owner_nodes;  // node ids holding a copy of the body
    std::string created;            // ISO-8601 UTC stamp at creation
};

// Serialize a partition value to its JSON shape (no id). The inverse of
// parse_dataset_partition_value; used for the CRDT value, partition.json, tests.
nlohmann::json to_json(const DatasetPartitionValue& value);

// The DatasetPartition JSON Schema (draft-07) — the single source of truth for the
// value shape, exposed so callers/tests can introspect it. Mirrors
// schemas/dataset_partition.schema.json (kept in sync by tests/test_schemas.cpp).
const nlohmann::json& dataset_partition_value_schema();

// Boundary validation: check a JSON value against the DatasetPartition JSON Schema.
// Returns true when valid; on failure returns false and (if `error` is non-null)
// fills it with a human-readable reason. Every stored/replicated partition value
// passes through here before it is trusted.
bool validate_dataset_partition_value(const nlohmann::json& value,
                                      std::string* error = nullptr);

// Parse + validate a JSON value (from partition.json on disk or a `part:<id>` CRDT
// value) into a DatasetPartitionValue. Returns nullopt when the value fails schema
// validation — so malformed data is rejected at the boundary with a clear reason
// (in `error`) instead of corrupting the in-memory tree.
std::optional<DatasetPartitionValue> parse_dataset_partition_value(
    const nlohmann::json& value, std::string* error = nullptr);

// A DatasetPartition: one entry in the partition catalog (ENT-36 / TBL-13) — a
// record of where a slice of a table's data physically lives. A top-level entity
// with no parent, modeled like zetta::ComputeJob: **replicated state** (flat key
// `part:<uuid>`) whose in-memory + on-disk (`<part-uuid>/partition.json`) forms are
// a projection of the Automerge CRDT document.
//
// The id is the folder name and never changes; the value is recorded at creation.
// Reconciliation uses apply_value(), which updates the projection from the document
// with NO capture (a peer's change arrives over the gossip, never written back).
// Lookups hand back shared_ptr handles, never raw pointers. See
// docs/dataset-partitions.md.
class DatasetPartition {
public:
    DatasetPartition(std::string id, DatasetPartitionValue value,
                     std::filesystem::path dir,
                     ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& table() const { return value_.table; }
    const std::string& blob_ref() const { return value_.blob_ref; }
    const PartitionRowRange& row_range() const { return value_.row_range; }
    const nlohmann::json& stats() const { return value_.stats; }
    const std::vector<std::string>& owner_nodes() const {
        return value_.owner_nodes;
    }
    const std::string& created() const { return value_.created; }
    const DatasetPartitionValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Replace the value to match a (replicated) document value, then rewrite
    // partition.json — with NO capture (used by reconciliation).
    void apply_value(DatasetPartitionValue value);

    // Write this partition's value (+ id) to dir/partition.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    std::string id_;
    DatasetPartitionValue value_;
    std::filesystem::path dir_;
    ReplicationManager* replication_;  // null = standalone (not replicated)
};

}  // namespace zetta
