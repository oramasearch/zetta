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
    bool contains(const std::string& id) const override {
        return id_index_.contains(id);
    }
    std::size_t size() const override { return records_.size(); }
    const nlohmann::json& at(std::size_t i) const override {
        return records_.at(i);
    }
    const std::string& id_at(std::size_t i) const override {
        return ids_.at(i);
    }

private:
    void load();
    std::filesystem::path chunk_path(std::size_t chunk_index) const;

    std::filesystem::path dir_;
    std::vector<nlohmann::json> records_;
    std::vector<std::string> ids_;             // parallel to records_
    std::unordered_set<std::string> id_index_;  // for contains()
};

}  // namespace zetta::storage