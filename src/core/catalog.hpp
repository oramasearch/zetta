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
//   run:<run_uuid>     -> { project, graph, status, created, updated, logs,
//                          outputs, lineage }  (one execution of a deployed
//                          graph — a top-level entity; see run.hpp)
//   model:<model_uuid> -> { name, kind, project, run, feature_schema, metrics,
//                          created, weights:{ size, sha256 } }  (a trained model
//                          — a top-level entity; the serialized weights body is
//                          NOT in the document — like a large object it lives on
//                          disk and transfers peer-to-peer, verified against
//                          weights.size + weights.sha256; see model.hpp)
//   job:<job_uuid>     -> { kind, status, project, graph, run, scaling_policy,
//                          created, updated, detail }  (a compute job — one unit
//                          of native distributed accelerated compute (ARC-11 /
//                          ENT-31); a top-level entity; see compute_job.hpp)
//   index:<index_uuid> -> { table, kind, backend, fields, created }  (a registered
//                          index over a table — search / vector / feature; a
//                          top-level entity with no parent (ENT-39 / ENT-40);
//                          metadata only — building the index is the IndexBackend
//                          seam's job; see index/index.hpp)
//   principal:<uuid>   -> { kind, name, created }          (an authenticated
//                          actor — user/service/node; a top-level entity with
//                          no parent — see principal.hpp)
//   key:<uuid>         -> { principal, scope, capabilities, created }
//                          (an access key; owned by a principal — its scope is
//                          the V1 key-scoped access model (RULE-3) and its
//                          capabilities are the deny-by-default rights model
//                          (ENT-22); see access_key.hpp / capability.hpp)
//   audit:<uuid>       -> { action, actor, target, allowed, detail, time }
//                          (an immutable, append-only audit event — a graph
//                          change / deploy / data-access decision; a top-level
//                          entity with no parent, never edited or removed; see
//                          audit_event.hpp)
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
inline constexpr std::string_view kRunPrefix = "run:";
inline constexpr std::string_view kModelPrefix = "model:";
inline constexpr std::string_view kJobPrefix = "job:";
inline constexpr std::string_view kIndexPrefix = "index:";
inline constexpr std::string_view kPrincipalPrefix = "principal:";
inline constexpr std::string_view kAccessKeyPrefix = "key:";
inline constexpr std::string_view kAuditPrefix = "audit:";

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
inline std::string run_key(const std::string& run_id) {
    return std::string(kRunPrefix) + run_id;
}
inline std::string model_key(const std::string& model_id) {
    return std::string(kModelPrefix) + model_id;
}
inline std::string job_key(const std::string& job_id) {
    return std::string(kJobPrefix) + job_id;
}
inline std::string index_key(const std::string& index_id) {
    return std::string(kIndexPrefix) + index_id;
}
inline std::string principal_key(const std::string& principal_id) {
    return std::string(kPrincipalPrefix) + principal_id;
}
inline std::string access_key_key(const std::string& access_key_id) {
    return std::string(kAccessKeyPrefix) + access_key_id;
}
inline std::string audit_key(const std::string& audit_id) {
    return std::string(kAuditPrefix) + audit_id;
}

// If `key` starts with `prefix`, return the id that follows it; else empty.
inline std::string id_after(std::string_view key, std::string_view prefix) {
    if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) {
        return {};
    }
    return std::string(key.substr(prefix.size()));
}

}  // namespace zetta::catalog
