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
    // Record the chunk; do not rewrite it yet. Reconciling a table issues one
    // update PER ROW, and a chunk holds up to kRecordsPerChunk rows — so
    // rewriting on each update still wrote ~10,000 rows per row reconciled, which
    // is the quadratic term that outlived the first fix (#952). Deferring
    // collapses a whole reconcile into one rewrite per touched chunk.
    //
    // Reads are unaffected: `records_` already holds the new value, so `at`,
    // `contains` and `size` answer correctly before the flush. Only the on-disk
    // projection lags, and only until the owner loop flushes.
    dirty_chunks_.insert(i / kRecordsPerChunk);
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
    dirty_chunks_.clear();  // persist_all wrote everything
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

// Write every chunk an update touched since the last flush. Called by the owner
// loop once per tick, so a reconcile of any size costs one rewrite per chunk it
// actually changed rather than one per row.
void zetta::storage::JsonlStore::flush() {
    if (dirty_chunks_.empty()) return;
    for (const std::size_t chunk : dirty_chunks_) persist_chunk(chunk);
    dirty_chunks_.clear();
}

fs::path JsonlStore::chunk_path(std::size_t chunk_index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%06zu.jsonl", chunk_index);
    return dir_ / name;
}

}  // namespace zetta::storage