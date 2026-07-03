#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_kind.hpp"

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::index {

// `index:<uuid>` carries (the id is the key / folder name, so it isn't repeated).
// This is exactly the shape validated by the Index JSON Schema at every boundary.
//
// The four fields ENT-39 / ENT-40 name: the source `table` the index is over, its
// `kind` (search / vector / feature), the `backend` that builds it (an
// IndexBackend name — see index/index_backend.hpp), and the `fields` it indexes
// (>= 1). `created` is the creation stamp every entity carries. `spec()` hands the
// IndexBackend seam its view of this index.
struct IndexValue {
    std::string table;                 // the source table (required, non-empty)
    IndexKind kind = IndexKind::Search;  // what the index accelerates
    std::string backend;               // the backend that builds it (non-empty)
    std::vector<std::string> fields;   // the indexed fields (>= 1, unique)
    std::string created;               // ISO-8601 UTC stamp at creation

    // This value's IndexBackend view (table + kind + backend + fields). The bridge
    // from the durable entity to the seam: registry.resolve(value.spec()).
    IndexSpec spec() const {
        return IndexSpec{table, kind, backend, fields};
    }

    bool operator==(const IndexValue&) const = default;
};

// Serialize an index value to its JSON shape (no id). The inverse of
// parse_index_value; used for the CRDT value, index.json, and tests.
nlohmann::json to_json(const IndexValue& value);

// The Index JSON Schema (draft-07) — the single source of truth for the value
// shape, exposed so callers/tests can introspect it. `schemas/index.schema.json`
// is generated from this and drift-checked (tests/test_schemas.cpp).
const nlohmann::json& index_value_schema();

// Boundary validation: check a JSON value against the Index JSON Schema. Returns
// true when valid; on failure returns false and (if `error` is non-null) fills it
// with a human-readable reason. The gate every stored/replicated index value
// passes before it is trusted.
bool validate_index_value(const nlohmann::json& value,
                          std::string* error = nullptr);

// Parse + validate a JSON value (from index.json on disk or an `index:<id>` CRDT
// value) into an IndexValue. Returns nullopt when the value fails schema
// validation — so malformed data is rejected at the boundary with a clear reason
// (in `error`) instead of corrupting the in-memory tree.
std::optional<IndexValue> parse_index_value(const nlohmann::json& value,
                                            std::string* error = nullptr);

// An Index record: a registered index over a table (ENT-39 / ENT-40, ADR-34). A
// top-level entity with no parent, modeled like zetta::ml::Model (minus the weights
// body — an index has no large-object body, only metadata) — **replicated state**
// (flat key `index:<uuid>`) whose in-memory + on-disk (`<index-uuid>/index.json`)
// forms are a projection of the Automerge CRDT document.
//
// The id is the folder name and never changes; `table` / `kind` / `backend` /
// `fields` are fixed at creation (an index is rebuilt, not re-specified — changing
// what it indexes is a new index). apply_value updates the projection from the
// document with NO capture (reconciliation). Lookups hand back shared_ptr handles,
// like Model / Run. See docs/indexing/index-backend.md.
class Index {
public:
    Index(std::string id, IndexValue value, std::filesystem::path dir,
          zetta::cluster::ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& table() const { return value_.table; }
    IndexKind kind() const { return value_.kind; }
    // The kind as its canonical lowercase name ("search" / …), for SQL / logs.
    std::string_view kind_name() const { return to_string(value_.kind); }
    const std::string& backend() const { return value_.backend; }
    const std::vector<std::string>& fields() const { return value_.fields; }
    const std::string& created() const { return value_.created; }
    const IndexValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // This index's IndexBackend view — resolve(index->spec()) finds its backend.
    IndexSpec spec() const { return value_.spec(); }

    // Replace the metadata to match a (replicated) document value, then rewrite
    // index.json — with NO capture (used by reconciliation; a peer's change
    // arrives already in the document).
    void apply_value(IndexValue value);

    // Write this index's metadata (+ id) to dir/index.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    std::string id_;
    IndexValue value_;
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone (not replicated)
};

}  // namespace zetta::index