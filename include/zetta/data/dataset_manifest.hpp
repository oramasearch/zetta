#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The **partition manifest** (#837) — the table-level record of how many
// partitions a finished dataset HAS, so a node can tell a complete view of the
// partition catalog from an incomplete one.
//
// The CRDT replicates the **metadata describing the Parquet blobs**; it never
// replicates the blobs. Per-partition metadata already rides in the catalog
// (`part:<uuid>` — `DatasetPartitionValue`: the body's `blob_ref`, its row range,
// its column schema, its byte size). What was missing is the count: a coordinator
// planning a scatter-gather `SELECT` planned over the `part:` entries **that had
// reached it**, and an entry still in flight simply did not appear in the plan.
// The unreachable-owner guard never fired, because such a partition is not
// unreachable — it is *invisible* — so the query answered short and looked
// healthy. `SELECT count(*)` returning a plausible fraction is the single
// outcome this design exists to prevent.
//
// The manifest closes that. It is written **once**, by the node that completed
// the write (the partitioned write path for a local write; the ingest initiator,
// after the cluster-wide allreduce, for a co-launched one), and replicates under
// its own flat key `mfst:<table-id>` — one writer, so it can never be lost to a
// concurrent write of the same key. Every reader then compares the partitions it
// can SEE against the count the manifest declares, and fails the query when it is
// short, exactly as an unreachable owner already does.
//
// Why its own key rather than a field of the `tbl:` value: a co-launched ingest
// has every participant capture `tbl:<id>` for itself when it adopts the target
// table (`Node::adopt_replicated_table`), so the table key has several writers
// whose writes are concurrent in the CRDT sense. A manifest folded into that
// value could lose the merge. `mfst:<table-id>` has exactly one writer.
//
// Nothing here throws — parsing is a validated boundary (`parse_dataset_manifest_value`),
// like every other replicated value. See docs/data/partition-manifest.md.
namespace zetta::data {

// The replicated/on-disk value of a dataset's partition manifest — everything the
// CRDT key `mfst:<table-id>` carries. This is exactly the shape validated by the
// DatasetManifest JSON Schema at every boundary (the `tbl` folder's table.json +
// the document).
struct DatasetManifestValue {
    std::string table;                       // the tbl:<uuid> this describes; required
    std::uint64_t partitions = 0;            // catalog entries the dataset SHOULD have
    std::uint64_t rows = 0;                  // rows across all of them
    std::uint64_t bytes = 0;                 // body bytes across all of them (0 = unknown)
    std::vector<std::string> partition_ids;  // the entry ids, when the writer knew them all
    std::string created;                     // ISO-8601 UTC stamp at completion

    bool operator==(const DatasetManifestValue&) const = default;
};

// Serialize a manifest value to its JSON shape. The inverse of
// parse_dataset_manifest_value; used for the CRDT value, table.json, and tests.
nlohmann::json to_json(const DatasetManifestValue& value);

// The DatasetManifest JSON Schema (draft-07) — the single source of truth for the
// value shape, exposed so callers/tests can introspect it. Mirrors
// schemas/dataset_manifest.schema.json (kept in sync by tests/test_schemas.cpp).
const nlohmann::json& dataset_manifest_value_schema();

// Boundary validation: check a JSON value against the DatasetManifest JSON Schema.
// Returns true when valid; on failure returns false and (if `error` is non-null)
// fills it with a human-readable reason.
bool validate_dataset_manifest_value(const nlohmann::json& value,
                                     std::string* error = nullptr);

// Parse + validate a JSON value (from table.json or a `mfst:<table-id>` CRDT
// value) into a DatasetManifestValue. Returns nullopt when the value fails schema
// validation, so malformed data is rejected at the boundary with a reason (in
// `error`) instead of being trusted as an authority the read path fails against.
std::optional<DatasetManifestValue> parse_dataset_manifest_value(
    const nlohmann::json& value, std::string* error = nullptr);

// Why a dataset's catalog is incomplete, or an empty string when it is complete.
// `visible` is how many `part:` entries with a body the reader can currently see.
//
// This is the one place the completeness rule lives, so the scatter-gather
// planner and the `system.datasets` view can never disagree about whether a
// dataset is degraded. A view holding MORE entries than the manifest declares is
// not short (a table written twice accumulates entries), so only a shortfall is
// reported.
std::string catalog_shortfall(const DatasetManifestValue& manifest,
                              std::uint64_t visible);

}  // namespace zetta::data
