#pragma once

#include <string>
#include <string_view>

// Internal (not a public header): the flat-key scheme that encodes the database
// catalog into the replicated CRDT document. The CRDT is the source of truth;
// the on-disk store + in-memory Node tree are a projection of these keys.
//
//   node:<node_id>  -> { address }                     (cluster membership)
//   db:<db_uuid>    -> { name }                         (a database)
//   tbl:<tbl_uuid>  -> { db: <db_uuid>, name }          (a table)
//   row:<row_uuid>  -> { tbl: <tbl_uuid>, data: {…} }   (a row)
//
// Each row/table/db owns its own key by UUID, so concurrent edits on different
// nodes never collide; the UUID is also the on-disk folder name (databases,
// tables) so the projection is identical on every node.
namespace zetta::catalog {

inline constexpr std::string_view kNodePrefix = "node:";
inline constexpr std::string_view kDatabasePrefix = "db:";
inline constexpr std::string_view kTablePrefix = "tbl:";
inline constexpr std::string_view kRowPrefix = "row:";

inline std::string node_key(const std::string& node_id) {
    return std::string(kNodePrefix) + node_id;
}
inline std::string database_key(const std::string& database_id) {
    return std::string(kDatabasePrefix) + database_id;
}
inline std::string table_key(const std::string& table_id) {
    return std::string(kTablePrefix) + table_id;
}
inline std::string row_key(const std::string& row_id) {
    return std::string(kRowPrefix) + row_id;
}

// If `key` starts with `prefix`, return the id that follows it; else empty.
inline std::string id_after(std::string_view key, std::string_view prefix) {
    if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) {
        return {};
    }
    return std::string(key.substr(prefix.size()));
}

}  // namespace zetta::catalog
