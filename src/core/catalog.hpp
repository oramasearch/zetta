#pragma once

#include <string>
#include <string_view>

// Internal (not a public header): the flat-key scheme that encodes the database
// catalog into the replicated CRDT document. The CRDT is the source of truth;
// the on-disk store + in-memory Node tree are a projection of these keys.
//
//   node:<node_id>     -> { address, reachability, node_type }  (cluster membership; #803/#809:
//                          address is the node's dialable address, empty for a
//                          dial-out-only node; reachability is "wan-accessible" or
//                          "dial-out-only" — transitive learners never dial the latter)
//   db:<db_uuid>       -> { name, replication? }        (a database; the optional
//                          replication field is the per-db ReplicationStrategy —
//                          ENT-35, omitted when the default ReplicateToAll; see
//                          replication_strategy.hpp)
//   tbl:<tbl_uuid>     -> { db: <db_uuid>, name, replication? }  (a table; the
//                          optional replication field is its ReplicationStrategy,
//                          omitted when default — ENT-35)
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
//                          project_graph.hpp / docs/pipeline/execution-graph.md)
//   deployment:<dep_uuid> -> { graph_name, target: {…}, node, node_count,
//                          edge_count, created }  (a graph placed onto a target;
//                          a top-level entity with no parent — see deployment.hpp)
//   run:<run_uuid>     -> { project, graph, status, created, updated, logs,
//                          outputs, lineage }  (one execution of a deployed
//                          graph — a top-level entity; see run.hpp)
//   study:<study_uuid> -> { spec, status, trials, spent_cost, stop_reason,
//                          promoted_model, created, updated }  (one AutoML
//                          search; each nested trial names its Run; #1138)
//   model:<model_uuid> -> { name, kind, project, run, feature_schema, metrics,
//                          created, weights:{ size, sha256 } }  (a trained model
//                          — a top-level entity; the serialized weights body is
//                          NOT in the document — like a large object it lives on
//                          disk and transfers peer-to-peer, verified against
//                          weights.size + weights.sha256; see model.hpp)
//   experiment:<uuid> -> { name, seed, allocations, created }  (a holdout /
//                          experiment definition; see solutions/experiment.hpp)
//   experiment_assignment:<experiment-uuid>:<entity-digest>
//                       -> { experiment, entity, allocation, tier, propensity,
//                            bucket, exempt, exemption_reason, exemption_actor,
//                            assigned_at }  (one independently convergent
//                            assignment and uplift-training row per entity)
//   monitor:<monitor_uuid> -> { solution, inputs, scores?, calibration, cohorts,
//                          guardrails, breaches, retrain_requested, created,
//                          updated }  (one drift and guardrail monitor per
//                          solution; see solutions/monitor.hpp)
//   job:<job_uuid>     -> { kind, status, project, graph, run, scaling_policy,
//                          created, updated, detail }  (a compute job — one unit
//                          of native distributed accelerated compute (ARC-11 /
//                          ENT-31); a top-level entity; see compute_job.hpp)
//   index:<index_uuid> -> { table, kind, backend, fields, created }  (a registered
//                          index over a table — search / vector / feature; a
//                          top-level entity with no parent (ENT-39 / ENT-40);
//                          metadata only — building the index is the IndexBackend
//                          index subsystem's job; see index/index.hpp)
//   principal:<uuid>   -> { kind, name, created }          (an authenticated
//                          actor — user/service/node; a top-level entity with
//                          no parent — see principal.hpp)
//   key:<uuid>         -> { principal, scope, capabilities, created }
//                          (an access key; owned by a principal — its scope is
//                          the V1 key-scoped access model (RULE-3) and its
//                          capabilities are the deny-by-default rights model
//                          (ENT-22); see access_key.hpp / capability.hpp)
//   audit:<uuid>       -> legacy only; migrated locally and retired on open (#1164)
//                          (an immutable, append-only audit event — a graph
//                          change / deploy / data-access decision; a top-level
//                          entity with no parent, never edited or removed; see
//                          audit_event.hpp)
//   ledger:<uuid>      -> an immutable action or outcome record in the
//                          solutions reward ledger (#1107); action records carry
//                          score, decision, versions, cell, cost and timestamps,
//                          while outcome records reference the action id
//   roi_readout:<uuid>  -> { id, solution, version, project, graph, experiment,
//                          roi_model, created_by, created }  (one immutable
//                          customer-supplied ROI configuration version; #1108)
//   user:<uuid>        -> { name, status, created }        (an authenticatable
//                          identity — conceptually a Principal of kind User in
//                          the FEAT-9 access model; a top-level entity — see
//                          governance/user.hpp)
//   role:<uuid>        -> { name, scope, capabilities, created }  (a named
//                          permission set granted to users via user_role; the
//                          built-in admin/writer/reader plus custom roles (#746);
//                          see governance/role.hpp)
//   token:<uuid>       -> { user, type, hash, salt, created, expires_at?,
//                          revoked }  (a bearer credential authenticating as a
//                          user, stored HASHED — the secret plaintext is never in
//                          the document; see governance/token.hpp / #744)
//   user_role:<uuid>   -> { user, role }                   (a user↔role
//                          assignment; a user's rights are the union of its
//                          roles; see governance/user_role.hpp)
//   policy:<uuid>      -> { name, database, table, command, using?, check?,
//                          roles?, created }  (a row-level security policy —
//                          Postgres-style RLS: a USING read filter + a WITH CHECK
//                          write filter, evaluated by the SQL WHERE engine against
//                          the acting identity; see governance/policy.hpp / #748)
//   decision_policy:<uuid> -> { id, name, current_version, versions, created,
//                          updated }  (the immutable version history for one
//                          score-to-action policy; see decision_policy.hpp)
//   decision:<uuid>    -> { id, policy_id, policy_version, entity_id, score,
//                          action, autonomy, status, actionable, ... }  (one
//                          durable policy result, stamped with its exact policy
//                          version; see decision_policy.hpp)
//   vtable:<name>      -> { name, prefix, columns:[{name,type,tier,key}] }  (a
//                          user-declared VIRTUAL TABLE over a CRDT key-family —
//                          CREATE VIRTUAL TABLE (#809 slice 3). Keyed by its
//                          human name (names are globally unique). Durable,
//                          replicated catalog metadata: its rows are computed at
//                          query time from the entries under `prefix`, so it
//                          converges + survives restart like db:/tbl:; see
//                          data/virtual_table.hpp / docs/data/virtual-tables.md)
//   part:<uuid>        -> { table, blob_ref, row_range:{start,end}, stats,
//                          owner_nodes }  (a dataset partition — a record of where
//                          a slice of a table's data physically lives: its table
//                          (tbl:) id, the large-object body (lo:) holding its
//                          bytes, the row range it covers, free-form column stats,
//                          and the nodes that own a copy. The catalog is
//                          replicate-to-all so placement is globally visible
//                          (ENT-36 / TBL-13); a top-level entity — see
//                          dataset_partition.hpp / docs/data/dataset-partitions.md)
//   mfst:<table-uuid>  -> { table, partitions, rows, bytes, partition_ids?,
//                          created }  (a dataset's PARTITION MANIFEST — how many
//                          part: entries the finished dataset has, so a reader
//                          can tell "I can see 6 of 8" from "there are 6" and
//                          fail the query rather than answer short (#837). Keyed
//                          by the table id: one manifest per table, written once
//                          by the node that completed the write, so the key has
//                          exactly one writer — see data/dataset_manifest.hpp /
//                          docs/data/partition-manifest.md)
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
inline constexpr std::string_view kStudyPrefix = "study:";
inline constexpr std::string_view kModelPrefix = "model:";
inline constexpr std::string_view kMonitorPrefix = "monitor:";
inline constexpr std::string_view kWorkspaceProfilePrefix = "profile:";
inline constexpr std::string_view kPlanPrefix = "plan:";
inline constexpr std::string_view kExperimentPrefix = "experiment:";
inline constexpr std::string_view kExperimentAssignmentPrefix =
    "experiment_assignment:";
inline constexpr std::string_view kAgentPrefix = "agent:";
inline constexpr std::string_view kJobPrefix = "job:";
inline constexpr std::string_view kIndexPrefix = "index:";
inline constexpr std::string_view kPrincipalPrefix = "principal:";
inline constexpr std::string_view kAccessKeyPrefix = "key:";
inline constexpr std::string_view kAuditPrefix = "audit:";
inline constexpr std::string_view kLedgerPrefix = "ledger:";
inline constexpr std::string_view kRoiReadoutPrefix = "roi_readout:";
inline constexpr std::string_view kPartitionPrefix = "part:";
inline constexpr std::string_view kManifestPrefix = "mfst:";
inline constexpr std::string_view kUserPrefix = "user:";
inline constexpr std::string_view kRolePrefix = "role:";
inline constexpr std::string_view kTokenPrefix = "token:";
inline constexpr std::string_view kUserRolePrefix = "user_role:";
inline constexpr std::string_view kPolicyPrefix = "policy:";
inline constexpr std::string_view kDecisionPolicyPrefix = "decision_policy:";
inline constexpr std::string_view kDecisionPrefix = "decision:";
inline constexpr std::string_view kVirtualTablePrefix = "vtable:";

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
inline std::string study_key(const std::string& study_id) {
    return std::string(kStudyPrefix) + study_id;
}
inline std::string model_key(const std::string& model_id) {
    return std::string(kModelPrefix) + model_id;
}
inline std::string monitor_key(const std::string& monitor_id) {
    return std::string(kMonitorPrefix) + monitor_id;
}
inline std::string workspace_profile_key(const std::string& workspace_profile_id) {
    return std::string(kWorkspaceProfilePrefix) + workspace_profile_id;
}
inline std::string plan_key(const std::string& plan_id) {
    return std::string(kPlanPrefix) + plan_id;
}
inline std::string experiment_key(const std::string& experiment_id) {
    return std::string(kExperimentPrefix) + experiment_id;
}
inline std::string experiment_assignment_key(
    const std::string& experiment_id,
    const std::string& assignment_id) {
    return std::string(kExperimentAssignmentPrefix) + experiment_id + ":" +
           assignment_id;
}
inline std::string agent_key(const std::string& agent_id) {
    return std::string(kAgentPrefix) + agent_id;
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
inline std::string decision_policy_key(const std::string& policy_id) {
    return std::string(kDecisionPolicyPrefix) + policy_id;
}
inline std::string decision_key(const std::string& decision_id) {
    return std::string(kDecisionPrefix) + decision_id;
}
inline std::string audit_key(const std::string& audit_id) {
    return std::string(kAuditPrefix) + audit_id;
}
inline std::string ledger_key(const std::string& ledger_id) {
    return std::string(kLedgerPrefix) + ledger_id;
}
inline std::string roi_readout_key(const std::string& readout_id) {
    return std::string(kRoiReadoutPrefix) + readout_id;
}
inline std::string partition_key(const std::string& partition_id) {
    return std::string(kPartitionPrefix) + partition_id;
}
// A dataset's partition manifest (#837) is keyed by the TABLE it describes — one
// manifest per table, written once by the node that completed the write, so the
// key has exactly one writer and can never lose a merge.
inline std::string manifest_key(const std::string& table_id) {
    return std::string(kManifestPrefix) + table_id;
}
inline std::string user_key(const std::string& user_id) {
    return std::string(kUserPrefix) + user_id;
}
inline std::string role_key(const std::string& role_id) {
    return std::string(kRolePrefix) + role_id;
}
inline std::string token_key(const std::string& token_id) {
    return std::string(kTokenPrefix) + token_id;
}
inline std::string user_role_key(const std::string& user_role_id) {
    return std::string(kUserRolePrefix) + user_role_id;
}
inline std::string policy_key(const std::string& policy_id) {
    return std::string(kPolicyPrefix) + policy_id;
}
// A virtual table is keyed by its human name (globally unique), not a UUID.
inline std::string virtual_table_key(const std::string& name) {
    return std::string(kVirtualTablePrefix) + name;
}

// If `key` starts with `prefix`, return the id that follows it; else empty.
inline std::string id_after(std::string_view key, std::string_view prefix) {
    if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) {
        return {};
    }
    return std::string(key.substr(prefix.size()));
}

}  // namespace zetta::catalog
