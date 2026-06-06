#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta {

// A named collection of JSON documents (rows), persisted on disk in its own
// folder (named by a UUID) via a JSONL storage engine. The folder name is the
// id; the human name lives in table.json beside the record chunks.
class Table {
public:
    Table(std::string id, std::string name, std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::filesystem::path& dir() const { return dir_; }

    StorageEngine& store() { return *store_; }
    const StorageEngine& store() const { return *store_; }

    // Append a row (persisted immediately); returns the new row count.
    std::size_t add_row(nlohmann::json doc);
    std::size_t row_count() const;

    // Write this table's metadata (id, name) to dir/table.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string name_;
    std::filesystem::path dir_;
    std::unique_ptr<StorageEngine> store_;
};

}  // namespace zetta
