#include "zetta/query/executor.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "zetta/cluster.hpp"
#include "zetta/database.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/table.hpp"

namespace zetta::query {
namespace {

// The system table exposing cluster membership + replication status, à la
// Postgres' pg_stat_replication. SELECT-only; backed by Cluster::nodes().
constexpr std::string_view kNodesTable = "zetta_nodes";

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

QueryResult error(std::string message) {
    QueryResult r;
    r.error = std::move(message);
    return r;
}

// One JSON row per node for the zetta_nodes system table. Unknown replication
// fields (e.g. a peer that hasn't reported yet, or self's sync state) are null.
std::vector<nlohmann::json> cluster_node_rows(const Cluster& cluster) {
    std::vector<nlohmann::json> rows;
    for (const NodeStatus& node : cluster.nodes()) {
        const NodeReplication& replication = node.replication;
        nlohmann::json row;
        row["node_id"] = node.node_id;
        row["address"] = node.address;
        row["role"] = to_string(node.role);
        row["is_self"] = node.is_self;
        row["connected"] = node.connected;
        row["entries"] = replication.known ? nlohmann::json(replication.entries)
                                           : nlohmann::json();
        row["version"] = replication.known ? nlohmann::json(replication.version)
                                           : nlohmann::json();
        row["in_sync"] = (replication.known && !node.is_self)
                             ? nlohmann::json(replication.in_sync)
                             : nlohmann::json();
        row["last_sync_seconds"] =
            (replication.known && !node.is_self)
                ? nlohmann::json(replication.seconds_since_sync)
                : nlohmann::json();
        rows.push_back(std::move(row));
    }
    return rows;
}

// Apply a SELECT's WHERE equality + projection to in-memory rows (used by the
// system table; regular tables stream straight from the storage engine).
QueryResult select_over_rows(const SelectStmt& statement,
                             const std::vector<nlohmann::json>& source) {
    QueryResult result;
    if (!statement.all) result.columns = statement.columns;
    for (const nlohmann::json& row : source) {
        if (statement.where) {
            if (!row.contains(statement.where->column) ||
                row.at(statement.where->column) != statement.where->value) {
                continue;
            }
        }
        if (statement.all) {
            result.rows.push_back(row);
        } else {
            nlohmann::json projected = nlohmann::json::object();
            for (const std::string& column : statement.columns) {
                projected[column] =
                    row.contains(column) ? row.at(column) : nlohmann::json();
            }
            result.rows.push_back(std::move(projected));
        }
    }
    result.message = "SELECT " + std::to_string(result.rows.size());
    return result;
}

}  // namespace

QueryResult Executor::execute(const Statement& stmt,
                              const std::string& current_database) {
    // Resolve a TableRef's database to a handle, or produce a clear error.
    const auto resolve_db =
        [&](const TableRef& ref) -> std::shared_ptr<Database> {
        const std::string& name =
            ref.database.empty() ? current_database : ref.database;
        if (name.empty()) return nullptr;  // no qualifier and no current db
        return node_.find_database(name);
    };
    const auto db_name = [&](const TableRef& ref) -> std::string {
        return ref.database.empty() ? current_database : ref.database;
    };

    return std::visit(
        overloaded{
            [&](const CreateDatabaseStmt& s) -> QueryResult {
                if (!node_.create_database(s.name)) {
                    return error("database \"" + s.name +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE DATABASE"};
            },
            [&](const DropDatabaseStmt& s) -> QueryResult {
                if (!node_.drop_database(s.name)) {
                    return error("database \"" + s.name + "\" does not exist");
                }
                return {.message = "DROP DATABASE"};
            },
            [&](const CreateTableStmt& s) -> QueryResult {
                const std::string name = db_name(s.table);
                if (name.empty()) {
                    return error("no database selected — qualify as db.table or "
                                 "select a database");
                }
                auto db = node_.find_database(name);
                if (!db) return error("database \"" + name + "\" does not exist");
                // Column definitions are parsed but not yet stored (the engine
                // is schemaless); the table is created by name.
                if (!db->create_table(s.table.table)) {
                    return error("table \"" + s.table.table +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE TABLE"};
            },
            [&](const DropTableStmt& s) -> QueryResult {
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                if (!db->drop_table(s.table.table)) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                return {.message = "DROP TABLE"};
            },
            [&](const InsertStmt& s) -> QueryResult {
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                if (s.columns.empty()) {
                    return error("INSERT requires an explicit column list, e.g. "
                                 "INSERT INTO t (a, b) VALUES (...)");
                }
                std::size_t inserted = 0;
                for (const auto& tuple : s.rows) {
                    if (tuple.size() != s.columns.size()) {
                        return error("INSERT has " +
                                     std::to_string(s.columns.size()) +
                                     " columns but " +
                                     std::to_string(tuple.size()) + " values");
                    }
                    nlohmann::json row = nlohmann::json::object();
                    for (std::size_t i = 0; i < s.columns.size(); ++i) {
                        row[s.columns[i]] = tuple[i];
                    }
                    table->add_row(std::move(row));
                    ++inserted;
                }
                return {.message = "INSERT 0 " + std::to_string(inserted)};
            },
            [&](const SelectStmt& s) -> QueryResult {
                // System table: cluster membership + replication status.
                if (s.table.database.empty() && s.table.table == kNodesTable) {
                    if (!cluster_) {
                        return error(
                            "\"zetta_nodes\" is unavailable here (no cluster)");
                    }
                    return select_over_rows(s, cluster_node_rows(*cluster_));
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                QueryResult r;
                if (!s.all) r.columns = s.columns;
                const StorageEngine& store = table->store();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    const nlohmann::json& row = store.at(i);
                    if (s.where) {
                        if (!row.contains(s.where->column) ||
                            row.at(s.where->column) != s.where->value) {
                            continue;
                        }
                    }
                    if (s.all) {
                        r.rows.push_back(row);
                    } else {
                        nlohmann::json projected = nlohmann::json::object();
                        for (const std::string& col : s.columns) {
                            projected[col] =
                                row.contains(col) ? row.at(col) : nlohmann::json();
                        }
                        r.rows.push_back(std::move(projected));
                    }
                }
                r.message = "SELECT " + std::to_string(r.rows.size());
                return r;
            },
        },
        stmt);
}

}  // namespace zetta::query
