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
            id_index_.emplace(id, ids_.size());
            ids_.push_back(std::move(id));
            records_.push_back(std::move(doc));
        }
    }
}

void zetta::storage::JsonlStore::add(std::string id, nlohmann::json doc) {
    const std::size_t index = records_.size();
    const nlohmann::json line{{"_zid", id}, {"doc", doc}};
    records_.push_back(std::move(doc));
    ids_.push_back(id);
    id_index_.emplace(std::move(id), index);
    // Append to the row's chunk (derived from its index), rolling to a fresh
    // file every kRecordsPerChunk rows. Append-open per row keeps it simple and
    // crash-resilient; can be optimized to a held stream later.
    const std::size_t chunk = index / kRecordsPerChunk;
    std::ofstream out(chunk_path(chunk), std::ios::app);
    out << line.dump() << '\n';
}

void zetta::storage::JsonlStore::update(const std::string& id,
                                        nlohmann::json doc) {
    const std::size_t i = index_of(id);
    if (i == kMissing) return;
    records_[i] = std::move(doc);
    // Only the row's OWN chunk changed. Rewriting every chunk made an update cost
    // O(rows) — and reconciling a table means one update per row, so the whole
    // reconcile was O(rows squared) and saturated the node's owner loop on a
    // few thousand rows (#952). An update does not move any other row, so no
    // other chunk can differ.
    persist_chunk(i / kRecordsPerChunk);
}

void zetta::storage::JsonlStore::remove(const std::string& id) {
    const std::size_t i = index_of(id);
    if (i == kMissing) return;
    records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(i));
    ids_.erase(ids_.begin() + static_cast<std::ptrdiff_t>(i));
    id_index_.erase(id);
    // Every row after `i` shifted down, so its chunk assignment may have changed
    // and a trailing chunk may now be empty. Unlike update, this genuinely needs
    // the full rewrite.
    for (std::size_t at = i; at < ids_.size(); ++at) id_index_[ids_[at]] = at;
    persist_all();
}

std::size_t zetta::storage::JsonlStore::index_of(const std::string& id) const {
    const auto found = id_index_.find(id);
    return found == id_index_.end() ? kMissing : found->second;
}

// Rewrite ONE chunk from the in-memory rows. The unit of persistence: an update
// touches a single row, and a row's chunk is fixed by its index, so only that
// chunk can differ.
//
// One stream for the whole chunk. The previous code opened a fresh append stream
// PER ROW, so rewriting a 10,000-row chunk meant 10,000 file opens — which is
// what actually burned the CPU, not the bytes written.
void zetta::storage::JsonlStore::persist_chunk(std::size_t chunk_index) {
    const std::size_t first = chunk_index * kRecordsPerChunk;
    if (first >= records_.size()) {
        std::error_code ec;
        fs::remove(chunk_path(chunk_index), ec);  // the chunk is now empty
        return;
    }
    const std::size_t last = std::min(first + kRecordsPerChunk, records_.size());
    std::ofstream out(chunk_path(chunk_index), std::ios::trunc);
    for (std::size_t i = first; i < last; ++i) {
        out << nlohmann::json{{"_zid", ids_[i]}, {"doc", records_[i]}}.dump() << '\n';
    }
}

// Rewrite every chunk. Needed when row indices MOVE (a removal shifts everything
// after it, and can leave a trailing chunk empty) — not for an update, which
// leaves every other row exactly where it was.
void zetta::storage::JsonlStore::persist_all() {
    std::error_code ec;
    for (std::size_t chunk = 0;; ++chunk) {
        const fs::path path = chunk_path(chunk);
        if (!fs::exists(path, ec)) break;
        fs::remove(path, ec);
    }
    const std::size_t chunks =
        records_.empty() ? 0 : (records_.size() - 1) / kRecordsPerChunk + 1;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) persist_chunk(chunk);
}

fs::path JsonlStore::chunk_path(std::size_t chunk_index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%06zu.jsonl", chunk_index);
    return dir_ / name;
}

}  // namespace zetta::storage