#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta {

class ReplicationManager;

// A named collection of JSON documents (rows), persisted on disk in its own
// folder (named by a UUID) via a JSONL storage engine. The folder name is the
// id; the human name lives in table.json beside the row chunks.
//
// With a ReplicationManager, each inserted row is also written to the replicated
// CRDT (capture); `materialize_row` is the reverse path used by reconciliation
// to apply a row that arrived from a peer, without re-capturing it.
class Table {
public:
    Table(std::string id, std::string name, std::filesystem::path dir,
          ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::filesystem::path& dir() const { return dir_; }

    StorageEngine& store() { return *store_; }
    const StorageEngine& store() const { return *store_; }

    // Insert a row (persisted immediately + captured into the CRDT); returns the
    // new row count. The row's id is generated here and used as its CRDT key.
    std::size_t add_row(nlohmann::json doc);

    // Apply a replicated row (from a peer) to the local store, only if not
    // already present. Does NOT write back to the CRDT. Returns whether added.
    bool materialize_row(std::string row_id, nlohmann::json doc);

    std::size_t row_count() const;

    // Write this table's metadata (id, name) to dir/table.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string name_;
    std::filesystem::path dir_;
    std::unique_ptr<StorageEngine> store_;
    ReplicationManager* replication_;  // null = not replicated
};

}  // namespace zetta
