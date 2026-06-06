#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta {

// A storage engine that persists records as JSON Lines, chunked at
// kRecordsPerChunk records per file (000000.jsonl, 000001.jsonl, ...) inside a
// directory, and caches every record in memory for reads. Existing chunks in
// `dir` are loaded on construction.
//
// (All records are held in memory for now; streaming/lazy reads can come later
// behind the same StorageEngine interface.)
class JsonlStore final : public StorageEngine {
public:
    static constexpr std::size_t kRecordsPerChunk = 10'000;

    explicit JsonlStore(std::filesystem::path dir);

    const char* name() const override { return "jsonl"; }
    void add(nlohmann::json doc) override;
    std::size_t size() const override { return records_.size(); }
    const nlohmann::json& at(std::size_t i) const override {
        return records_.at(i);
    }

private:
    void load();
    std::filesystem::path chunk_path(std::size_t chunk_index) const;

    std::filesystem::path dir_;
    std::vector<nlohmann::json> records_;
};

}  // namespace zetta
