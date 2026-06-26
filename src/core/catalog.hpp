#pragma once

#include <string>
#include <string_view>

// Internal (not a public header): the flat-key scheme that encodes the database
// catalog into the replicated CRDT document. The CRDT is the source of truth;
// the on-disk store + in-memory Node tree are a projection of these keys.
//
//   node:<node_id>     -> { address }                  (cluster membership)
//   db:<db_uuid>       -> { name }                      (a database)
//   tbl:<tbl_uuid>     -> { db: <db_uuid>, name }       (a table)
//   row:<row_uuid>     -> { tbl: <tbl_uuid>, data: {…} } (a row)
//   lo:<lo_uuid>       -> { db, name, size, sha256 }    (large-object metadata;
//                          the body is NOT in the document — it transfers
//                          peer-to-peer and is verified against size + sha256)
//   project:<proj_uuid> -> { name, created, updated, owner }  (an AI project;
//                          a top-level entity with no parent — see project.hpp)
//   input:<input_uuid> -> { project, kind, source, filename, content_hash,
//                          size, created_by, created_at }  (a project input;
//                          owned by a project — the raw payload body is NOT in
//                          the document, it lives on disk like a large object;
//                          see project_input.hpp)
//   graph:<graph_uuid> -> { project: <proj_uuid>, name,       (an ExecutionGraph
//                          graph: { nodes, edges } }            IR under a project
//                          — the whole serialized graph as one value; see
//                          project_graph.hpp / docs/execution-graph.md)
//   deployment:<dep_uuid> -> { graph_name, target: {…}, node, node_count,
//                          edge_count, created }  (a graph placed onto a target;
//                          a top-level entity with no parent — see deployment.hpp)
//
// Each row/table/db owns its own key by UUID, so concurrent edits on different
// nodes never collide; the UUID is also the on-disk folder name (databases,
// tables) so the projection is identical on every node.
namespace zetta::catalog {

inline constexpr std::string_view kNodePrefix = "node:";
inline constexpr std::string_view kDatabasePrefix = "db:";
inline constexpr std::string_view kTablePrefix = "tbl:";
inline constexpr std::string_view kRowPrefix = "row:";
inline constexpr std::string_view kLargeObjectPrefix = "lo:";
inline constexpr std::string_view kProjectPrefix = "project:";
inline constexpr std::string_view kInputPrefix = "input:";
inline constexpr std::string_view kGraphPrefix = "graph:";
inline constexpr std::string_view kDeploymentPrefix = "deployment:";

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
inline std::string large_object_key(const std::string& large_object_id) {
    return std::string(kLargeObjectPrefix) + large_object_id;
}
inline std::string project_key(const std::string& project_id) {
    return std::string(kProjectPrefix) + project_id;
}
inline std::string input_key(const std::string& input_id) {
    return std::string(kInputPrefix) + input_id;
}
inline std::string graph_key(const std::string& graph_id) {
    return std::string(kGraphPrefix) + graph_id;
}
inline std::string deployment_key(const std::string& deployment_id) {
    return std::string(kDeploymentPrefix) + deployment_id;
}

// If `key` starts with `prefix`, return the id that follows it; else empty.
inline std::string id_after(std::string_view key, std::string_view prefix) {
    if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) {
        return {};
    }
    return std::string(key.substr(prefix.size()));
}

}  // namespace zetta::catalog
