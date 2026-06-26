#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "zetta/access_key.hpp"
#include "zetta/database.hpp"
#include "zetta/deployment.hpp"
#include "zetta/principal.hpp"
#include "zetta/project.hpp"
#include "zetta/run.hpp"
#include "zetta/scope.hpp"

namespace zetta {

class ReplicationManager;
namespace ir { class ExecutionGraph; }

// Where a node's persistent state lives and how it presents itself. More
// fields (listen address, replication peers, cache sizes) land alongside the
// storage + query engine.
struct NodeConfig {
    std::string data_dir = "./zetta-data";  // database directory on disk
    bool preflight = true;  // run an integrity check on start() (--no-preflight)
};

// Result of the startup integrity ("preflight") check — a snapshot of how sound
// the on-disk + replicated state is. Counts plus the specific problems found.
struct IntegrityReport {
    std::size_t databases = 0;
    std::size_t tables = 0;
    std::size_t rows = 0;
    std::size_t large_objects = 0;
    std::size_t large_object_bodies_present = 0;
    std::size_t large_object_bodies_missing = 0;  // metadata here, body not (yet)
    std::size_t corrupt_large_objects = 0;         // on-disk body != recorded sha256
    std::size_t dangling_tables = 0;               // table -> missing database
    std::size_t dangling_rows = 0;                 // row -> missing table
    std::size_t dangling_large_objects = 0;        // large object -> missing database
    std::size_t dangling_inputs = 0;               // project input -> missing project
    std::size_t dangling_graphs = 0;               // graph -> missing project
    std::size_t dangling_access_keys = 0;          // access key -> missing principal

    // A "good state": no corruption and no dangling references. (A missing body
    // is informational — it may simply be awaiting transfer from a peer.)
    bool ok() const {
        return corrupt_large_objects == 0 && dangling_tables == 0 &&
               dangling_rows == 0 && dangling_large_objects == 0 &&
               dangling_inputs == 0 && dangling_graphs == 0 &&
               dangling_access_keys == 0;
    }
};

// A single zetta database node — the headless core.
//
// A Node owns the database lifecycle and knows nothing about any UI: the GUI
// (zetta::gui::App) is a thin renderer driven by a Node, and headless mode
// (zetta::run_headless) drives the same Node with no renderer at all.
//
// Replication: when constructed with a ReplicationManager, the **CRDT document
// is the source of truth** and the on-disk store + this in-memory tree are a
// projection of it. Local DDL/row mutations are captured into the document; on
// each tick the node reconciles the projection to match the document (applying
// changes merged from peers) and persists the document to disk. With no
// ReplicationManager (the default), a Node is a standalone local store.
class Node {
public:
    explicit Node(NodeConfig config, ReplicationManager* replication = nullptr);

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Bring the node up (open data_dir, load the replicated document, reconcile).
    // Returns false without changing state if already running.
    bool start();

    // Advance background work, including reconciling the local projection with
    // the replicated document when it has changed. Cheap and non-blocking.
    void tick();

    // Bring the node down cleanly (flush, close). Safe to call when stopped.
    void stop();

    // CRUD for databases. create/rename yield an empty handle / false on an
    // empty name or a name clash; the UI surfaces that inline.
    std::shared_ptr<Database> create_database(std::string name);
    std::shared_ptr<Database> find_database(const std::string& name) const;
    bool drop_database(const std::string& name);
    bool rename_database(const std::string& from, const std::string& to);
    std::vector<std::string> database_names() const;
    std::size_t database_count() const { return databases_.size(); }

    // CRUD for projects (the central, user-managed AI artifact — see
    // project.hpp / docs/projects.md), mirroring the database CRUD shape.
    // create stamps created/updated and takes an optional owner key;
    // create/rename yield an empty handle / false on an empty name or a clash.
    std::shared_ptr<Project> create_project(std::string name,
                                            std::string owner = {});
    std::shared_ptr<Project> find_project(const std::string& name) const;
    bool drop_project(const std::string& name);
    bool rename_project(const std::string& from, const std::string& to);
    std::vector<std::string> project_names() const;
    std::size_t project_count() const { return projects_.size(); }

    // This node's id (its cluster id + replication actor id), stable across
    // restarts (persisted in data_dir/node_id). Set by start(); empty before it.
    const std::string& node_id() const { return node_id_; }

    // Deployment (ENT-6 / FEAT-4): place a validated execution graph onto a
    // target and record it. R1 supports a **Local** target only; deploy_graph
    // returns an empty handle for a non-local target or an empty graph. The
    // recorded deployment is replicated + on disk and queryable via
    // deployments() / find_deployment() (and the system.zetta_deployments SQL
    // table). The graph is recorded as a summary (name + node/edge counts) since
    // the IR has no serialized form yet. Reconcile/self-heal is a later slice.
    std::shared_ptr<Deployment> deploy_graph(const ir::ExecutionGraph& graph,
                                             DeploymentTarget target,
                                             std::string graph_name);
    std::shared_ptr<Deployment> find_deployment(const std::string& id) const;
    std::vector<std::shared_ptr<Deployment>> deployments() const {
        return deployments_;
    }
    std::size_t deployment_count() const { return deployments_.size(); }

    // CRUD for runs (one execution of a deployed graph — see run.hpp /
    // docs/runs.md). Runs have no human name: they are keyed by id, so find /
    // drop take the id. create stamps created/updated, starts Pending, and takes
    // optional project + graph references. The "update" half of the slice is the
    // mutators on the returned Run handle (set_status / add_log / set_outputs /
    // set_lineage), which capture into the CRDT and persist. runs() is in a
    // stable order (created, then id).
    std::shared_ptr<Run> create_run(std::string project = {},
                                    std::string graph = {});
    std::shared_ptr<Run> find_run(const std::string& id) const;
    bool drop_run(const std::string& id);
    std::vector<std::shared_ptr<Run>> runs() const { return runs_; }
    std::size_t run_count() const { return runs_.size(); }

    // CRUD for principals (authenticated actors — see principal.hpp /
    // docs/access-control.md), mirroring the project CRUD shape. create stamps
    // `created` and takes an optional kind (default User); create/rename yield
    // an empty handle / false on an empty name or a clash. A principal owns its
    // access keys (Principal::create_key); a request's reach is the scope of the
    // key it logged in with (RULE-3).
    std::shared_ptr<Principal> create_principal(
        std::string name, PrincipalKind kind = PrincipalKind::User);
    std::shared_ptr<Principal> find_principal(const std::string& name) const;
    bool drop_principal(const std::string& name);
    bool rename_principal(const std::string& from, const std::string& to);
    std::vector<std::string> principal_names() const;
    std::size_t principal_count() const { return principals_.size(); }

    // Resolve the access key presented at login (by its id), or an empty handle.
    // Keys are globally identified by UUID; this searches every principal.
    std::shared_ptr<AccessKey> find_access_key(const std::string& key_id) const;

    // The V1 key-scoped access decision (RULE-3 / PERM-1 / ADR-6): a request that
    // logged in with `key_id` inherits that key's scope, so `resource` is allowed
    // iff the key permits it. Deny-by-default — an unknown/empty key id denies.
    AccessDecision authorize(const std::string& key_id,
                             const Resource& resource) const;

    bool running() const { return running_; }
    const NodeConfig& config() const { return config_; }

    // Seconds since start(); 0 when stopped.
    double uptime_seconds() const;

    // Monotonic heartbeat count since the most recent start().
    std::uint64_t ticks() const { return ticks_; }

    // One-line, human-readable status for logs and the UI status bar.
    std::string status_line() const;

    // Scan the data for integrity problems (corrupt large-object bodies, dangling
    // references) without changing anything. Run automatically on start() unless
    // NodeConfig::preflight is false; the last result is kept here.
    IntegrityReport check_integrity() const;
    const IntegrityReport& integrity() const { return integrity_; }

private:
    using Clock = std::chrono::steady_clock;

    // Rebuild databases_ by scanning config_.data_dir. Called from start().
    void load_databases();
    std::shared_ptr<Database> find_database_by_id(const std::string& id) const;

    // Rebuild projects_ by scanning config_.data_dir. Called from start().
    void load_projects();
    std::shared_ptr<Project> find_project_by_id(const std::string& id) const;

    // Rebuild deployments_ by scanning config_.data_dir. Called from start().
    void load_deployments();

    // This node's stable id: the replication actor id when replicated, else the
    // UUID persisted in data_dir/node_id (created on first use). Set by start().
    std::string resolve_node_id();

    // Rebuild runs_ by scanning config_.data_dir. Called from start().
    void load_runs();

    // Rebuild principals_ by scanning config_.data_dir. Called from start().
    void load_principals();
    std::shared_ptr<Principal> find_principal_by_id(const std::string& id) const;

    // --- replication (only when replication_ != nullptr) -------------------
    std::filesystem::path replication_path() const;
    void load_replication();           // read the persisted CRDT document
    void persist_replication() const;  // write the CRDT document to disk
    void migrate_to_replication();     // capture local-only data into the CRDT
    void reconcile_from_replication(); // apply the CRDT document to the projection
    // Materialize (no capture) — used by reconciliation:
    std::shared_ptr<Database> materialize_database(std::string id,
                                                   std::string name);
    bool materialize_drop_database(const std::string& id);
    std::shared_ptr<Project> materialize_project(std::string id,
                                                 std::string name,
                                                 std::string created,
                                                 std::string updated,
                                                 std::string owner);
    bool materialize_drop_project(const std::string& id);
    std::shared_ptr<Deployment> materialize_deployment(
        std::string id, std::string graph_name, DeploymentTarget target,
        std::string node_id, std::size_t node_count, std::size_t edge_count,
        std::string created);
    bool materialize_drop_deployment(const std::string& id);
    std::shared_ptr<Run> materialize_run(std::string id, RunValue value);
    bool materialize_drop_run(const std::string& id);
    std::shared_ptr<Principal> materialize_principal(std::string id,
                                                     PrincipalKind kind,
                                                     std::string name,
                                                     std::string created);
    bool materialize_drop_principal(const std::string& id);

    NodeConfig config_;
    ReplicationManager* replication_;  // null = standalone (not replicated)
    std::string node_id_;              // this node's stable UUID (set by start())
    std::string last_seen_version_;    // CRDT version last reconciled/persisted
    IntegrityReport integrity_;        // last preflight result
    bool running_ = false;
    Clock::time_point started_at_{};
    std::uint64_t ticks_ = 0;
    std::vector<std::shared_ptr<Database>> databases_;      // insertion order
    std::vector<std::shared_ptr<Project>> projects_;        // insertion order
    std::vector<std::shared_ptr<Deployment>> deployments_;  // sorted by created
    std::vector<std::shared_ptr<Run>> runs_;                // sorted (created, id)
    std::vector<std::shared_ptr<Principal>> principals_;    // insertion order
};

}  // namespace zetta
