// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/storage/lazy_row.hpp"

namespace zetta::storage {

struct StorageMemoryUsage {
    std::uint64_t bytes = 0;
    std::uint64_t items = 0;
    std::uint64_t cache_bytes = 0;
    std::uint64_t cache_items = 0;
};

// Interface for a node's storage engine.
//
// A node can use different engines (in-memory, on-disk, disk-backed and paging,
// replicated, ...). This is the boundary they share. Keep it small — add methods
// as real engines actually need them, rather than guessing up front.
//
// Every row carries a stable string id (a UUID): it is the row's key in the
// replicated CRDT document (see src/core/catalog.hpp), so the same logical row
// has the same id on every node. The id lets reconciliation add a replicated row
// exactly once (`contains`) and lets a node enumerate its rows to replicate them
// (`id_at`).
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

    // Cheap resident-memory counters for system.memory. Engines with caches or
    // fully resident rows override this; the fallback covers container nodes.
    virtual StorageMemoryUsage memory_usage() const {
        return StorageMemoryUsage{size() * 64, size(), 0, 0};
    }

    // The row at index i — its data and its id — as a handle that keeps the row
    // alive for as long as the caller holds it (`ParquetLazyPtr`, which for an
    // always-resident engine is two borrowed pointers and no allocation).
    //
    // A scan stays a scan:
    //
    //     for (std::size_t i = 0; i < store.size(); ++i) {
    //         const auto row = store.at(i);
    //         if (!row) continue;          // row.error() says why
    //         use(*row, row.id());
    //     }
    //
    // A disk-backed engine reads to answer this, and a read can fail: the
    // returned handle is then not dereferenceable and carries a path-qualified
    // `error()`. It is never null-for-absence — `i < size()` is the caller's
    // precondition, so anything short of a row here is a failure, not absence.
    virtual ParquetLazyPtr at(std::size_t i) const = 0;

    // The id of the row at index i. Precondition: i < size().
    //
    // BY VALUE, deliberately. Returning a reference obliged every engine to keep
    // a `std::string` per row alive somewhere durable, so an engine that pages
    // row *data* still had memory scaling with row COUNT regardless of its cache
    // budget — the binding constraint before any genuinely large pull (#836). By
    // value, an engine is free to *derive* an id instead of storing one: a
    // partition-body row is identified by (blob, row index), so its id is a
    // computation, not a lookup. Engines that do hold ids still just return a
    // copy, and nearly every caller copied already (a CRDT key, an error string,
    // an element pushed into a vector).
    virtual std::string id_at(std::size_t i) const = 0;

    // The bytes of row data this engine keeps on disk, kept as a counter that
    // every write updates, so answering costs nothing per row and reads no file
    // (#1195, the lean `list_datasets`). A deferred write is counted when it is
    // made, not when it is flushed: after a flush the counter equals the files.
    // `std::nullopt` from an engine that keeps no such counter.
    virtual std::optional<std::uint64_t> stored_bytes() const { return std::nullopt; }

    // Make every pending write durable.
    //
    // An engine MAY defer the cost of writing — reconciling a table issues one
    // `update` per row, and paying a file rewrite on each made reconcile
    // quadratic and saturated the node's owner loop (#952). Deferring lets a
    // batch of updates cost one rewrite instead of one per row.
    //
    // The contract that keeps that safe: an engine that defers must be
    // **indistinguishable through its own reads** — `at`, `contains` and `size`
    // answer from memory and already reflect the write. Only the on-disk
    // projection lags, and only until the owner loop's next flush.
    //
    // Default: nothing to do, for engines that write through.
    virtual void flush() {}
};

}  // namespace zetta::storage
