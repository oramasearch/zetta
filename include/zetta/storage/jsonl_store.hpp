#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
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
    const std::string& id_at(std::size_t i) const override {
        return ids_.at(i);
    }

private:
    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);
    void load();
    void persist_all();  // rewrite every chunk file from the in-memory rows
    std::size_t index_of(const std::string& id) const;
    std::filesystem::path chunk_path(std::size_t chunk_index) const;

    std::filesystem::path dir_;
    std::vector<nlohmann::json> records_;
    std::vector<std::string> ids_;             // parallel to records_
    std::unordered_set<std::string> id_index_;  // for contains()
};

}  // namespace zetta::storage