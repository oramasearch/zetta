#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetta/table.hpp"

namespace zetta {

// A named set of tables. Tables are kept in insertion order so the UI lists
// them stably; names are unique within a database. Ownership is via
// shared_ptr so lookups can hand back a safe handle rather than a raw pointer.
class Database {
public:
    explicit Database(std::string name);

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // Create a table; returns an empty handle if the name is empty or taken.
    std::shared_ptr<Table> create_table(std::string name);
    // Find a table by name, or an empty handle.
    std::shared_ptr<Table> find_table(const std::string& name) const;
    // Drop a table; returns true if one was removed.
    bool drop_table(const std::string& name);

    std::vector<std::string> table_names() const;
    std::size_t table_count() const { return tables_.size(); }

private:
    std::string name_;
    std::vector<std::shared_ptr<Table>> tables_;
};

}  // namespace zetta
