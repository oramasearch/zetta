#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta::storage {

// Interface for a node's storage engine.
//
// A node will eventually be able to use different engines (in-memory, on-disk,
// replicated, ...). This is the seam they share. Keep it small — add methods as
// real engines actually need them, rather than guessing up front.
//
// Every row carries a stable string id (a UUID): it is the row's key in the
// replicated CRDT document (see src/core/catalog.hpp), so the same logical row
// has the same id on every node. The id lets reconciliation add a replicated row
// exactly once (`contains`) and lets a node enumerate its rows to replicate them
// (`id_at`). `at(i)` returns just the row data, never the id.
class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    // Short engine name for logs / UI (e.g. "json").
    virtual const char* name() const = 0;

    // Append a row with the given id. Caller ensures the id is unique.
    virtual void add(std::string id, nlohmann::json doc) = 0;

    // Replace the data of the row with this id, keeping the id + position. No-op if
    // absent. Backs SQL UPDATE (and the reconcile of a peer's row-value change).
    virtual void update(const std::string& id, nlohmann::json doc) = 0;

    // Remove the row with this id. No-op if absent. Backs SQL DELETE (and the
    // reconcile of a peer's row tombstone).
    virtual void remove(const std::string& id) = 0;

    // Whether a row with this id is already stored (idempotent reconciliation).
    virtual bool contains(const std::string& id) const = 0;

    // Number of rows currently held.
    virtual std::size_t size() const = 0;

    // The row data at index i. Precondition: i < size().
    virtual const nlohmann::json& at(std::size_t i) const = 0;

    // The id of the row at index i. Precondition: i < size().
    virtual const std::string& id_at(std::size_t i) const = 0;
};

}  // namespace zetta::storage