#include "zetta/storage/jsonl_store.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace zetta {
namespace fs = std::filesystem;

JsonlStore::JsonlStore(fs::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    load();
}

void JsonlStore::load() {
    records_.clear();
    // Chunks are contiguous from 0; stop at the first missing file.
    for (std::size_t chunk = 0;; ++chunk) {
        const fs::path path = chunk_path(chunk);
        std::error_code ec;
        if (!fs::exists(path, ec)) break;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto doc = nlohmann::json::parse(line, nullptr,
                                             /*allow_exceptions=*/false);
            if (!doc.is_discarded()) records_.push_back(std::move(doc));
        }
    }
}

void JsonlStore::add(nlohmann::json doc) {
    records_.push_back(std::move(doc));
    // Append the new record to its chunk (derived from its index), rolling to a
    // fresh file every kRecordsPerChunk records. Append-open per record keeps it
    // simple and crash-resilient; can be optimized to a held stream later.
    const std::size_t index = records_.size() - 1;
    const std::size_t chunk = index / kRecordsPerChunk;
    std::ofstream out(chunk_path(chunk), std::ios::app);
    out << records_.back().dump() << '\n';
}

fs::path JsonlStore::chunk_path(std::size_t chunk_index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%06zu.jsonl", chunk_index);
    return dir_ / name;
}

}  // namespace zetta
