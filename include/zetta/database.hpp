#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "zetta/large_object.hpp"
#include "zetta/table.hpp"

namespace zetta {

class ReplicationManager;

// A named set of tables, persisted in its own folder (named by a UUID) under
// the node's data dir. Each table is a subfolder; database.json holds the
// human name. Ownership/lookups use shared_ptr handles, never raw pointers.
//
// With a ReplicationManager, table create/drop are captured into the replicated
// CRDT; `materialize_table` is the reverse path used by reconciliation.
class Database {
public:
    Database(std::string id, std::string name, std::filesystem::path dir,
             ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Rename the database (rewrites database.json; the folder/id is unchanged).
    void set_name(std::string name);

    // (Re)load tables by scanning dir for table folders. Called on node start.
    void load();

    // Write this database's metadata (id, name) to dir/database.json.
    void persist_meta() const;

    // Create a table (folder + table.json on disk + captured into the CRDT);
    // returns an empty handle if the name is empty or already taken.
    std::shared_ptr<Table> create_table(std::string name);
    // Apply a replicated table (from a peer) with a given id, without capturing.
    // Returns the existing handle if already present.
    std::shared_ptr<Table> materialize_table(std::string id, std::string name);
    // Find a table by name, or an empty handle.
    std::shared_ptr<Table> find_table(const std::string& name) const;
    // Find a table by its id (UUID), or an empty handle.
    std::shared_ptr<Table> find_table_by_id(const std::string& id) const;
    // Drop a table (removes its folder + the CRDT key); true if one was removed.
    bool drop_table(const std::string& name);
    // Drop a table by id without capturing (reconciliation removing a peer's
    // drop). Returns whether one was removed.
    bool materialize_drop_table(const std::string& id);

    std::vector<std::string> table_names() const;
    const std::vector<std::shared_ptr<Table>>& tables() const { return tables_; }
    std::size_t table_count() const { return tables_.size(); }

    // Files stored in this database as binary blobs (Postgres-style LO API).
    LargeObjectStore& large_objects() { return large_objects_; }
    const LargeObjectStore& large_objects() const { return large_objects_; }

private:
    std::string id_;
    std::string name_;
    std::filesystem::path dir_;
    std::vector<std::shared_ptr<Table>> tables_;
    LargeObjectStore large_objects_;
    ReplicationManager* replication_;  // null = not replicated
};

}  // namespace zetta
