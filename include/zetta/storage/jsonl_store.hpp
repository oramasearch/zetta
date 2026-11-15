// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta::storage {

// A storage engine that persists rows as JSON Lines, chunked at kRecordsPerChunk
// rows per file (000000.jsonl, 000001.jsonl, ...) inside a directory, and caches
// every row in memory for reads. Existing chunks in `dir` are loaded on
// construction.
//
// Each line is `{ "_zid": "<row-uuid>", "doc": <row data> }` — the id is the
// row's key in the replicated CRDT (see src/core/catalog.hpp). Legacy lines that
// are just the bare row (no `_zid`) are still read; they're assigned a fresh id.
//
// (All rows are held in memory for now; streaming/lazy reads can come later
// behind the same StorageEngine interface.)
class JsonlStore final : public StorageEngine {
public:
    static constexpr std::size_t kRecordsPerChunk = 10'000;

    explicit JsonlStore(std::filesystem::path dir);

    const char* name() const override { return "jsonl"; }
    void add(std::string id, nlohmann::json doc) override;
    void update(const std::string& id, nlohmann::json doc) override;
    void remove(const std::string& id) override;
    bool contains(const std::string& id) const override {
        return id_index_.contains(id);
    }
    std::size_t size() const override { return records_.size(); }
    StorageMemoryUsage memory_usage() const override {
        return StorageMemoryUsage{resident_bytes_, records_.size(), 0, 0};
    }
    // Always resident: the handle borrows the row and its id straight out of
    // this store's own vectors — no page, no refcount, no allocation.
    ParquetLazyPtr at(std::size_t i) const override {
        if (i >= records_.size()) {
            return ParquetLazyPtr::failed("jsonl store: row " + std::to_string(i) +
                                          " is past the end (" +
                                          std::to_string(records_.size()) + " rows)");
        }
        return ParquetLazyPtr(records_[i], ids_[i]);
    }
    std::string id_at(std::size_t i) const override { return ids_.at(i); }
    // The JSONL chunk files' bytes, from per-row line sizes kept on every write
    // (#1195): a dataset list reads this instead of listing the chunk files.
    std::optional<std::uint64_t> stored_bytes() const override { return stored_bytes_; }
    void flush() override;

private:
    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);
    void load();
    void persist_all();            // rewrite every chunk file from the in-memory rows
    void persist_chunk(std::size_t chunk_index);  // rewrite ONE chunk
    std::size_t index_of(const std::string& id) const;
    std::filesystem::path chunk_path(std::size_t chunk_index) const;

    std::filesystem::path dir_;
    std::vector<nlohmann::json> records_;
    std::vector<std::string> ids_;             // parallel to records_
    std::vector<std::size_t> resident_sizes_;  // parallel resident estimates
    std::size_t resident_bytes_ = 0;
    // Each row's line in its chunk file, newline included, and their sum: the
    // chunk files' size once every dirty chunk is flushed (#1195).
    std::vector<std::uint64_t> stored_sizes_;  // parallel to records_
    std::uint64_t stored_bytes_ = 0;
    // id -> row index. A SET plus a linear scan was O(n) on every update, and
    // `update` is the hot path when a multi-thousand-row table reconciles: the
    // lookup alone made reconcile quadratic before a single byte was written.
    std::unordered_map<std::string, std::size_t> id_index_;

    // Chunks changed since the last flush. An `update` records the chunk instead
    // of rewriting it: reconciling 15,000 rows issued 15,000 updates, and a chunk
    // holds up to kRecordsPerChunk rows, so rewriting on each one wrote ~10,000
    // rows per update. Deferring collapses a whole reconcile into one rewrite per
    // touched chunk.
    std::set<std::size_t> dirty_chunks_;
};

}  // namespace zetta::storage
