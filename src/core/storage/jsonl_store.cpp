#include "zetta/storage/jsonl_store.hpp"

#include "zetta/util/uuid.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace zetta::storage {
namespace fs = std::filesystem;

zetta::storage::JsonlStore::JsonlStore(fs::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    load();
}

void zetta::storage::JsonlStore::load() {
    records_.clear();
    ids_.clear();
    id_index_.clear();
    // Chunks are contiguous from 0; stop at the first missing file.
    for (std::size_t chunk = 0;; ++chunk) {
        const fs::path path = chunk_path(chunk);
        std::error_code ec;
        if (!fs::exists(path, ec)) break;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto parsed = nlohmann::json::parse(line, nullptr,
                                                /*allow_exceptions=*/false);
            if (parsed.is_discarded()) continue;
            // Current format: { "_zid": id, "doc": data }. Legacy lines are the
            // bare row — read them too, assigning a fresh id.
            std::string id;
            nlohmann::json doc;
            if (parsed.is_object() && parsed.contains("_zid") &&
                parsed.contains("doc")) {
                id = parsed["_zid"].get<std::string>();
                doc = std::move(parsed["doc"]);
            } else {
                id = zetta::util::new_uuid();
                doc = std::move(parsed);
            }
            ids_.push_back(id);
            id_index_.insert(id);
            records_.push_back(std::move(doc));
        }
    }
}

void zetta::storage::JsonlStore::add(std::string id, nlohmann::json doc) {
    const std::size_t index = records_.size();
    const nlohmann::json line{{"_zid", id}, {"doc", doc}};
    records_.push_back(std::move(doc));
    ids_.push_back(id);
    id_index_.insert(std::move(id));
    // Append to the row's chunk (derived from its index), rolling to a fresh
    // file every kRecordsPerChunk rows. Append-open per row keeps it simple and
    // crash-resilient; can be optimized to a held stream later.
    const std::size_t chunk = index / kRecordsPerChunk;
    std::ofstream out(chunk_path(chunk), std::ios::app);
    out << line.dump() << '\n';
}

fs::path JsonlStore::chunk_path(std::size_t chunk_index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%06zu.jsonl", chunk_index);
    return dir_ / name;
}

}  // namespace zetta::storage