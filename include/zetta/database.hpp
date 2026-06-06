#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "zetta/table.hpp"

namespace zetta {

// A named set of tables, persisted in its own folder (named by a UUID) under
// the node's data dir. Each table is a subfolder; database.json holds the
// human name. Ownership/lookups use shared_ptr handles, never raw pointers.
class Database {
public:
    Database(std::string id, std::string name, std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Rename the database (rewrites database.json; the folder/id is unchanged).
    void set_name(std::string name);

    // (Re)load tables by scanning dir for table folders. Called on node start.
    void load();

    // Write this database's metadata (id, name) to dir/database.json.
    void persist_meta() const;

    // Create a table (folder + table.json on disk); returns an empty handle if
    // the name is empty or already taken.
    std::shared_ptr<Table> create_table(std::string name);
    // Find a table by name, or an empty handle.
    std::shared_ptr<Table> find_table(const std::string& name) const;
    // Drop a table (removes its folder); returns true if one was removed.
    bool drop_table(const std::string& name);

    std::vector<std::string> table_names() const;
    std::size_t table_count() const { return tables_.size(); }

private:
    std::string id_;
    std::string name_;
    std::filesystem::path dir_;
    std::vector<std::shared_ptr<Table>> tables_;
};

}  // namespace zetta
