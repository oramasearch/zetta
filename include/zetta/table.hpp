#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta {

// A named collection of JSON documents (rows), backed by a storage engine.
// The engine is a JsonStore for now; a table is the unit that owns one.
class Table {
public:
    explicit Table(std::string name);

    const std::string& name() const { return name_; }

    StorageEngine& store() { return *store_; }
    const StorageEngine& store() const { return *store_; }

    // Append a row; returns the new row count.
    std::size_t add_row(nlohmann::json doc);
    std::size_t row_count() const;

private:
    std::string name_;
    std::unique_ptr<StorageEngine> store_;
};

}  // namespace zetta
