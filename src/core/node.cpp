#include "zetta/node.hpp"

#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/project_graph.hpp"
#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/timestamp.hpp"
#include "zetta/uuid.hpp"
#include "zetta/version.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

Node::Node(NodeConfig config, ReplicationManager* replication)
    : config_(std::move(config)), replication_(replication) {}

bool Node::start() {
    if (running_) return false;
    running_ = true;
    started_at_ = Clock::now();
    ticks_ = 0;
    node_id_ = resolve_node_id();
    load_databases();
    load_projects();
    load_deployments();
    load_runs();
    load_principals();
    if (replication_) {
        load_replication();         // bring in the persisted CRDT document
        migrate_to_replication();   // capture any local-only data into it
        reconcile_from_replication();  // materialize anything only in the document
        persist_replication();
        last_seen_version_ = replication_->version();
    }
    std::printf(
        "zetta %s: node started (data_dir=%s, %zu databases, %zu projects, "
        "%zu deployments, %zu runs, %zu principals)\n",
        kVersion, config_.data_dir.c_str(), databases_.size(),
        projects_.size(), deployments_.size(), runs_.size(),
        principals_.size());
    if (config_.preflight) {
        integrity_ = check_integrity();
        std::printf(
            "zetta: preflight: %zu databases, %zu tables, %zu rows, %zu large "
            "objects (%zu present, %zu awaiting body) — %s\n",
            integrity_.databases, integrity_.tables, integrity_.rows,
            integrity_.large_objects, integrity_.large_object_bodies_present,
            integrity_.large_object_bodies_missing,
            integrity_.ok() ? "OK" : "ISSUES FOUND");
        if (!integrity_.ok()) {
            std::printf(
                "zetta: preflight WARN: %zu corrupt large object(s), %zu "
                "dangling table(s), %zu dangling row(s), %zu dangling large "
                "object(s), %zu dangling input(s), %zu dangling graph(s), %zu "
                "dangling access key(s)\n",
                integrity_.corrupt_large_objects, integrity_.dangling_tables,
                integrity_.dangling_rows, integrity_.dangling_large_objects,
                integrity_.dangling_inputs, integrity_.dangling_graphs,
                integrity_.dangling_access_keys);
        }
    }
    std::fflush(stdout);
    return true;
}

void Node::tick() {
    if (!running_) return;
    ++ticks_;
    // The CRDT is the source of truth; when it has advanced (a local capture or
    // a peer's gossip merged in on the network thread), reconcile the on-disk
    // projection to match it and persist. Reconcile only materializes (never
    // captures), so it doesn't move the version — no feedback loop.
    if (replication_) {
        std::string version = replication_->version();
        if (version != last_seen_version_) {
            reconcile_from_replication();
            persist_replication();
            last_seen_version_ = std::move(version);
        }
    }
}

void Node::stop() {
    if (!running_) return;
    if (replication_) persist_replication();
    running_ = false;
    std::printf("zetta: node stopped\n");
    std::fflush(stdout);
}

double Node::uptime_seconds() const {
    if (!running_) return 0.0;
    const std::chrono::duration<double> elapsed = Clock::now() - started_at_;
    return elapsed.count();
}

void Node::load_databases() {
    databases_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "database.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        if (meta.is_discarded() || !meta.contains("name")) continue;
        auto db = std::make_shared<Database>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(), entry.path(), replication_);
        db->load();
        databases_.push_back(std::move(db));
    }
    // Stable order for the UI (directory iteration order is unspecified).
    std::sort(databases_.begin(), databases_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

void Node::load_projects() {
    projects_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "project.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: skip anything that doesn't match the project.json
        // shape (a name is required; the rest default if an older file omits it).
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("name") || !meta["name"].is_string()) {
            continue;
        }
        auto project = std::make_shared<Project>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(),
            meta.value("created", std::string{}),
            meta.value("updated", std::string{}),
            meta.value("owner", std::string{}), entry.path(), replication_);
        project->load();  // bring in this project's inputs + graphs from disk
        projects_.push_back(std::move(project));
    }
    // Stable order (directory iteration order is unspecified).
    std::sort(projects_.begin(), projects_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Project> Node::find_project_by_id(const std::string& id) const {
    for (const auto& project : projects_) {
        if (project->id() == id) return project;
    }
    return nullptr;
}

std::string Node::resolve_node_id() {
    // When replicated, the node id IS the replication actor id (the same UUID),
    // so the recorded placement node matches the document's actor.
    if (replication_) return replication_->node_id();
    // Standalone: the id is persisted in data_dir/node_id (created on first use),
    // so it's stable across restarts — mirroring how main.cpp seeds it.
    const fs::path path = fs::path(config_.data_dir) / "node_id";
    std::error_code ec;
    fs::create_directories(config_.data_dir, ec);
    if (std::ifstream in(path); in) {
        std::string id;
        std::getline(in, id);
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r' ||
                               id.back() == ' ')) {
            id.pop_back();
        }
        if (!id.empty()) return id;
    }
    const std::string id = new_uuid();
    std::ofstream(path, std::ios::trunc) << id << '\n';
    return id;
}

void Node::load_deployments() {
    deployments_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "deployment.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a deployment needs a string graph_name and a
        // well-formed target (DeploymentTarget::from_json rejects an unknown
        // kind). Anything malformed is skipped, never trusted into the tree.
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("graph_name") || !meta["graph_name"].is_string()) {
            continue;
        }
        const std::optional<DeploymentTarget> target =
            DeploymentTarget::from_json(
                meta.value("target", nlohmann::json::object()));
        if (!target) continue;
        deployments_.push_back(std::make_shared<Deployment>(
            entry.path().filename().string(),
            meta["graph_name"].get<std::string>(), *target,
            meta.value("node", std::string{}),
            meta.value("node_count", std::size_t{0}),
            meta.value("edge_count", std::size_t{0}),
            meta.value("created", std::string{}), entry.path()));
    }
    // Stable, chronological order (id breaks ties on equal stamps).
    std::sort(deployments_.begin(), deployments_.end(),
              [](const auto& a, const auto& b) {
                  return std::tie(a->created(), a->id()) <
                         std::tie(b->created(), b->id());
              });
}

void Node::load_principals() {
    principals_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "principal.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: require an object with a known kind and a name;
        // skip anything that doesn't match the principal.json shape.
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("kind") || !meta["kind"].is_string() ||
            !meta.contains("name") || !meta["name"].is_string()) {
            continue;
        }
        const auto kind = principal_kind_from(meta["kind"].get<std::string>());
        if (!kind) continue;
        auto principal = std::make_shared<Principal>(
            entry.path().filename().string(), *kind,
            meta["name"].get<std::string>(),
            meta.value("created", std::string{}), entry.path(), replication_);
        principal->load();  // bring in this principal's keys from disk
        principals_.push_back(std::move(principal));
    }
    // Stable order (directory iteration order is unspecified).
    std::sort(principals_.begin(), principals_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Principal> Node::find_principal_by_id(
    const std::string& id) const {
    for (const auto& principal : principals_) {
        if (principal->id() == id) return principal;
    }
    return nullptr;
}

std::shared_ptr<Database> Node::create_database(std::string name) {
    // "system" is reserved for the virtual system database (computed system
    // tables like system.zetta_nodes); never a real on-disk database.
    if (name.empty() || name == "system" || find_database(name)) return nullptr;
    const std::string id = new_uuid();
    if (replication_) {
        replication_->set(catalog::database_key(id),
                          nlohmann::json{{"name", name}});
    }
    return materialize_database(id, std::move(name));
}

std::shared_ptr<Database> Node::materialize_database(std::string id,
                                                     std::string name) {
    if (auto existing = find_database_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto db = std::make_shared<Database>(std::move(id), std::move(name), dir,
                                         replication_);
    db->persist_meta();
    databases_.push_back(db);
    return db;
}

std::shared_ptr<Database> Node::find_database(const std::string& name) const {
    for (const auto& db : databases_) {
        if (db->name() == name) return db;
    }
    return nullptr;
}

std::shared_ptr<Database> Node::find_database_by_id(
    const std::string& id) const {
    for (const auto& db : databases_) {
        if (db->id() == id) return db;
    }
    return nullptr;
}

bool Node::drop_database(const std::string& name) {
    auto db = find_database(name);
    if (!db) return false;
    if (replication_) replication_->remove(catalog::database_key(db->id()));
    return materialize_drop_database(db->id());
}

bool Node::materialize_drop_database(const std::string& id) {
    const auto it = std::find_if(
        databases_.begin(), databases_.end(),
        [&](const std::shared_ptr<Database>& d) { return d->id() == id; });
    if (it == databases_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    databases_.erase(it);
    return true;
}

bool Node::rename_database(const std::string& from, const std::string& to) {
    if (to.empty() || find_database(to)) return false;  // empty or clashes
    auto db = find_database(from);
    if (!db) return false;
    if (replication_) {
        replication_->set(catalog::database_key(db->id()),
                          nlohmann::json{{"name", to}});
    }
    db->set_name(to);
    return true;
}

std::vector<std::string> Node::database_names() const {
    std::vector<std::string> names;
    names.reserve(databases_.size());
    for (const auto& db : databases_) names.push_back(db->name());
    return names;
}

// ----- projects (mirror the database CRUD; replicated as project:<uuid>) -----

std::shared_ptr<Project> Node::create_project(std::string name,
                                              std::string owner) {
    if (name.empty() || find_project(name)) return nullptr;
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    if (replication_) {
        replication_->set(catalog::project_key(id),
                          nlohmann::json{{"name", name},
                                         {"created", now},
                                         {"updated", now},
                                         {"owner", owner}});
    }
    return materialize_project(id, std::move(name), now, now, std::move(owner));
}

std::shared_ptr<Project> Node::materialize_project(std::string id,
                                                   std::string name,
                                                   std::string created,
                                                   std::string updated,
                                                   std::string owner) {
    if (auto existing = find_project_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto project = std::make_shared<Project>(std::move(id), std::move(name),
                                             std::move(created),
                                             std::move(updated),
                                             std::move(owner), dir,
                                             replication_);
    project->persist_meta();
    projects_.push_back(project);
    return project;
}

std::shared_ptr<Project> Node::find_project(const std::string& name) const {
    for (const auto& project : projects_) {
        if (project->name() == name) return project;
    }
    return nullptr;
}

bool Node::drop_project(const std::string& name) {
    auto project = find_project(name);
    if (!project) return false;
    if (replication_) {
        // Drop the project's graphs from the document too, so they don't linger
        // as dangling `graph:` keys once their parent is gone.
        for (const auto& graph : project->graphs()) {
            replication_->remove(catalog::graph_key(graph->id()));
        }
        replication_->remove(catalog::project_key(project->id()));
    }
    return materialize_drop_project(project->id());
}

bool Node::materialize_drop_project(const std::string& id) {
    const auto it = std::find_if(
        projects_.begin(), projects_.end(),
        [&](const std::shared_ptr<Project>& p) { return p->id() == id; });
    if (it == projects_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    projects_.erase(it);
    return true;
}

bool Node::rename_project(const std::string& from, const std::string& to) {
    if (to.empty() || find_project(to)) return false;  // empty or clashes
    auto project = find_project(from);
    if (!project) return false;
    project->set_metadata(to, project->created(), now_iso8601(),
                          project->owner());
    if (replication_) {
        replication_->set(catalog::project_key(project->id()),
                          project->metadata());
    }
    return true;
}

std::vector<std::string> Node::project_names() const {
    std::vector<std::string> names;
    names.reserve(projects_.size());
    for (const auto& project : projects_) names.push_back(project->name());
    return names;
}

// ----- deployments (place a graph onto a target; replicated as deployment:<id>)

std::shared_ptr<Deployment> Node::deploy_graph(const ir::ExecutionGraph& graph,
                                               DeploymentTarget target,
                                               std::string graph_name) {
    // R1 places onto the local node only; k8s/cloud targets are deferred.
    if (!target.is_local()) return nullptr;
    // A deployable graph must have something to place. Full semantic validation
    // (acyclicity, legal stage transitions) is the gate — a separate slice;
    // deploy_graph records an already-validated graph and guards the basics.
    if (graph.empty()) return nullptr;

    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    const std::size_t node_count = graph.node_count();
    const std::size_t edge_count = graph.edge_count();
    if (replication_) {
        replication_->set(catalog::deployment_key(id),
                          nlohmann::json{{"graph_name", graph_name},
                                         {"target", target.to_json()},
                                         {"node", node_id_},
                                         {"node_count", node_count},
                                         {"edge_count", edge_count},
                                         {"created", now}});
    }
    return materialize_deployment(id, std::move(graph_name), std::move(target),
                                  node_id_, node_count, edge_count, now);
}

std::shared_ptr<Deployment> Node::materialize_deployment(
    std::string id, std::string graph_name, DeploymentTarget target,
    std::string node_id, std::size_t node_count, std::size_t edge_count,
    std::string created) {
    if (auto existing = find_deployment(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto deployment = std::make_shared<Deployment>(
        std::move(id), std::move(graph_name), std::move(target),
        std::move(node_id), node_count, edge_count, std::move(created), dir);
    deployment->persist_meta();
    deployments_.push_back(deployment);
    return deployment;
}

std::shared_ptr<Deployment> Node::find_deployment(const std::string& id) const {
    for (const auto& deployment : deployments_) {
        if (deployment->id() == id) return deployment;
    }
    return nullptr;
}

// ----- principals (mirror the project CRUD; replicated as principal:<uuid>) ---

std::shared_ptr<Principal> Node::create_principal(std::string name,
                                                  PrincipalKind kind) {
    if (name.empty() || find_principal(name)) return nullptr;
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    if (replication_) {
        replication_->set(catalog::principal_key(id),
                          nlohmann::json{{"kind", to_string(kind)},
                                         {"name", name},
                                         {"created", now}});
    }
    return materialize_principal(id, kind, std::move(name), now);
}

std::shared_ptr<Principal> Node::materialize_principal(std::string id,
                                                       PrincipalKind kind,
                                                       std::string name,
                                                       std::string created) {
    if (auto existing = find_principal_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto principal = std::make_shared<Principal>(
        std::move(id), kind, std::move(name), std::move(created), dir,
        replication_);
    principal->persist_meta();
    principals_.push_back(principal);
    return principal;
}

std::shared_ptr<Principal> Node::find_principal(const std::string& name) const {
    for (const auto& principal : principals_) {
        if (principal->name() == name) return principal;
    }
    return nullptr;
}

bool Node::materialize_drop_deployment(const std::string& id) {
    const auto it = std::find_if(
        deployments_.begin(), deployments_.end(),
        [&](const std::shared_ptr<Deployment>& d) { return d->id() == id; });
    if (it == deployments_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    deployments_.erase(it);
    return true;
}

// ----- runs (one execution of a deployed graph; replicated as run:<uuid>) ----

void Node::load_runs() {
    runs_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "run.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a run.json that fails the Run JSON Schema is
        // rejected here (with a logged reason) rather than allowed to corrupt
        // the in-memory tree — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<RunValue> value =
            meta.is_discarded()
                ? std::nullopt
                : parse_run_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid run %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        runs_.push_back(std::make_shared<Run>(entry.path().filename().string(),
                                              *value, entry.path(),
                                              replication_));
    }
    // Stable order (directory iteration order is unspecified): chronological by
    // creation stamp (fixed-width ISO-8601 sorts as text), then id to break ties.
    std::sort(runs_.begin(), runs_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<Run> Node::create_run(std::string project, std::string graph) {
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    RunValue value;
    value.project = std::move(project);
    value.graph = std::move(graph);
    value.status = RunStatus::Pending;
    value.created = now;
    value.updated = now;
    if (replication_) replication_->set(catalog::run_key(id), to_json(value));
    return materialize_run(id, std::move(value));
}

std::shared_ptr<Run> Node::materialize_run(std::string id, RunValue value) {
    if (auto existing = find_run(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto run = std::make_shared<Run>(std::move(id), std::move(value), dir,
                                     replication_);
    run->persist_meta();
    runs_.push_back(run);
    return run;
}

std::shared_ptr<Run> Node::find_run(const std::string& id) const {
    for (const auto& run : runs_) {
        if (run->id() == id) return run;
    }
    return nullptr;
}

// ----- principals (mirror the project CRUD; replicated as principal:<uuid>) ---

bool Node::drop_principal(const std::string& name) {
    auto principal = find_principal(name);
    if (!principal) return false;
    // Drop the principal's keys from the document too (their folders go with the
    // principal folder below). Snapshot the ids first — drop_key mutates keys().
    if (replication_) {
        std::vector<std::string> key_ids;
        for (const auto& key : principal->keys()) key_ids.push_back(key->id());
        for (const std::string& key_id : key_ids) {
            replication_->remove(catalog::access_key_key(key_id));
        }
        replication_->remove(catalog::principal_key(principal->id()));
    }
    return materialize_drop_principal(principal->id());
}

bool Node::materialize_drop_principal(const std::string& id) {
    const auto it = std::find_if(
        principals_.begin(), principals_.end(),
        [&](const std::shared_ptr<Principal>& p) { return p->id() == id; });
    if (it == principals_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    principals_.erase(it);
    return true;
}

bool Node::rename_principal(const std::string& from, const std::string& to) {
    if (to.empty() || find_principal(to)) return false;  // empty or clashes
    auto principal = find_principal(from);
    if (!principal) return false;
    principal->set_metadata(principal->kind(), to, principal->created());
    if (replication_) {
        replication_->set(catalog::principal_key(principal->id()),
                          principal->metadata());
    }
    return true;
}

std::vector<std::string> Node::principal_names() const {
    std::vector<std::string> names;
    names.reserve(principals_.size());
    for (const auto& principal : principals_) names.push_back(principal->name());
    return names;
}

std::shared_ptr<AccessKey> Node::find_access_key(
    const std::string& key_id) const {
    for (const auto& principal : principals_) {
        if (auto key = principal->find_key(key_id)) return key;
    }
    return nullptr;
}

bool Node::drop_run(const std::string& id) {
    if (!find_run(id)) return false;
    if (replication_) replication_->remove(catalog::run_key(id));
    return materialize_drop_run(id);
}

bool Node::materialize_drop_run(const std::string& id) {
    const auto it = std::find_if(
        runs_.begin(), runs_.end(),
        [&](const std::shared_ptr<Run>& r) { return r->id() == id; });
    if (it == runs_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    runs_.erase(it);
    return true;
}

AccessDecision Node::authorize(const std::string& key_id,
                               const Resource& resource) const {
    auto key = find_access_key(key_id);
    if (!key) {
        return AccessDecision{false, "unknown access key — denied"};
    }
    return zetta::authorize(*key, resource);
}

// ----- replication: persistence ---------------------------------------------

fs::path Node::replication_path() const {
    return fs::path(config_.data_dir) / "replication.automerge";
}

void Node::load_replication() {
    std::error_code ec;
    const fs::path path = replication_path();
    if (!fs::exists(path, ec)) return;
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    if (raw.empty()) return;
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    replication_->merge(bytes);
}

void Node::persist_replication() const {
    const std::vector<std::byte> bytes = replication_->save();
    const fs::path path = replication_path();
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);  // atomic replace
}

// ----- replication: capture local-only data ---------------------------------

void Node::migrate_to_replication() {
    const nlohmann::json snapshot = replication_->snapshot();
    for (const auto& db : databases_) {
        if (!snapshot.contains(catalog::database_key(db->id()))) {
            replication_->set(catalog::database_key(db->id()),
                              nlohmann::json{{"name", db->name()}});
        }
        for (const auto& table : db->tables()) {
            if (!snapshot.contains(catalog::table_key(table->id()))) {
                replication_->set(
                    catalog::table_key(table->id()),
                    nlohmann::json{{"db", db->id()}, {"name", table->name()}});
            }
            const StorageEngine& store = table->store();
            for (std::size_t i = 0; i < store.size(); ++i) {
                const std::string& row_id = store.id_at(i);
                if (!snapshot.contains(catalog::row_key(row_id))) {
                    replication_->set(
                        catalog::row_key(row_id),
                        nlohmann::json{{"tbl", table->id()},
                                       {"data", store.at(i)}});
                }
            }
        }
        for (const LargeObject& lo : db->large_objects().list()) {
            if (!snapshot.contains(catalog::large_object_key(lo.id))) {
                replication_->set(catalog::large_object_key(lo.id),
                                  nlohmann::json{{"db", db->id()},
                                                 {"name", lo.name},
                                                 {"size", lo.size},
                                                 {"sha256", lo.sha256}});
            }
        }
    }
    for (const auto& project : projects_) {
        if (!snapshot.contains(catalog::project_key(project->id()))) {
            replication_->set(catalog::project_key(project->id()),
                              project->metadata());
        }
        for (const auto& input : project->inputs()) {
            if (!snapshot.contains(catalog::input_key(input->id()))) {
                replication_->set(catalog::input_key(input->id()),
                                  input->metadata());
            }
        }
        for (const auto& graph : project->graphs()) {
            if (!snapshot.contains(catalog::graph_key(graph->id()))) {
                replication_->set(catalog::graph_key(graph->id()),
                                  graph->record());
            }
        }
    }
    for (const auto& deployment : deployments_) {
        if (!snapshot.contains(catalog::deployment_key(deployment->id()))) {
            replication_->set(catalog::deployment_key(deployment->id()),
                              deployment->metadata());
        }
    }
    for (const auto& run : runs_) {
        if (!snapshot.contains(catalog::run_key(run->id()))) {
            replication_->set(catalog::run_key(run->id()), run->metadata());
        }
    }
    for (const auto& principal : principals_) {
        if (!snapshot.contains(catalog::principal_key(principal->id()))) {
            replication_->set(catalog::principal_key(principal->id()),
                              principal->metadata());
        }
        for (const auto& key : principal->keys()) {
            if (!snapshot.contains(catalog::access_key_key(key->id()))) {
                replication_->set(catalog::access_key_key(key->id()),
                                  key->metadata());
            }
        }
    }
}

// ----- replication: apply the document to the local projection --------------

void Node::reconcile_from_replication() {
    const nlohmann::json snapshot = replication_->snapshot();

    // Decode the flat catalog keys into desired databases / tables / rows.
    std::map<std::string, std::string> databases;             // id -> name
    std::map<std::string, std::pair<std::string, std::string>> tables;  // id -> (db, name)
    struct RowEntry { std::string table_id; nlohmann::json data; };
    std::map<std::string, RowEntry> rows;                     // id -> entry
    struct LargeObjectEntry {
        std::string database_id;
        std::string name;
        std::uintmax_t size;
        std::string sha256;
    };
    std::map<std::string, LargeObjectEntry> large_objects;    // id -> entry

    for (const auto& [key, value] : snapshot.items()) {
        if (std::string id = catalog::id_after(key, catalog::kDatabasePrefix);
            !id.empty() && value.is_object() && value.contains("name")) {
            databases.emplace(std::move(id), value["name"].get<std::string>());
        } else if (std::string table_id =
                       catalog::id_after(key, catalog::kTablePrefix);
                   !table_id.empty() && value.is_object() &&
                   value.contains("db") && value.contains("name")) {
            tables.emplace(std::move(table_id),
                           std::pair{value["db"].get<std::string>(),
                                     value["name"].get<std::string>()});
        } else if (std::string row_id =
                       catalog::id_after(key, catalog::kRowPrefix);
                   !row_id.empty() && value.is_object() &&
                   value.contains("tbl") && value.contains("data")) {
            rows.emplace(std::move(row_id),
                         RowEntry{value["tbl"].get<std::string>(),
                                  value["data"]});
        } else if (std::string lo_id =
                       catalog::id_after(key, catalog::kLargeObjectPrefix);
                   !lo_id.empty() && value.is_object() && value.contains("db")) {
            large_objects.emplace(
                std::move(lo_id),
                LargeObjectEntry{value.value("db", std::string{}),
                                 value.value("name", std::string{}),
                                 value.value("size", std::uintmax_t{0}),
                                 value.value("sha256", std::string{})});
        }
    }

    // Removals first (a peer dropped a database/table). Snapshot ids up front so
    // we don't mutate while iterating.
    std::vector<std::string> local_db_ids;
    for (const auto& db : databases_) local_db_ids.push_back(db->id());
    for (const std::string& db_id : local_db_ids) {
        if (!databases.contains(db_id)) {
            materialize_drop_database(db_id);
            continue;
        }
        auto db = find_database_by_id(db_id);
        std::vector<std::string> local_table_ids;
        for (const auto& table : db->tables()) {
            local_table_ids.push_back(table->id());
        }
        for (const std::string& table_id : local_table_ids) {
            const auto it = tables.find(table_id);
            if (it == tables.end() || it->second.first != db_id) {
                db->materialize_drop_table(table_id);
            }
        }
        std::vector<std::string> local_lo_ids;
        for (const LargeObject& lo : db->large_objects().list()) {
            local_lo_ids.push_back(lo.id);
        }
        for (const std::string& lo_id : local_lo_ids) {
            const auto it = large_objects.find(lo_id);
            if (it == large_objects.end() || it->second.database_id != db_id) {
                db->large_objects().materialize_unlink(lo_id);
            }
        }
    }

    // Additions / renames.
    for (const auto& [id, name] : databases) {
        auto db = find_database_by_id(id);
        if (!db) {
            materialize_database(id, name);
        } else if (db->name() != name) {
            db->set_name(name);  // a peer renamed it (no re-capture)
        }
    }
    for (const auto& [id, db_and_name] : tables) {
        auto db = find_database_by_id(db_and_name.first);
        if (db && !db->find_table_by_id(id)) {
            db->materialize_table(id, db_and_name.second);
        }
    }
    for (auto& [id, entry] : rows) {
        auto db_table = [&]() -> std::shared_ptr<Table> {
            for (const auto& db : databases_) {
                if (auto table = db->find_table_by_id(entry.table_id)) {
                    return table;
                }
            }
            return nullptr;
        }();
        if (db_table) db_table->materialize_row(id, std::move(entry.data));
    }
    for (const auto& [id, entry] : large_objects) {
        auto db = find_database_by_id(entry.database_id);
        if (!db) continue;
        LargeObject object;
        object.id = id;
        object.name = entry.name;
        object.size = entry.size;
        object.sha256 = entry.sha256;
        db->large_objects().materialize_object(std::move(object));
    }

    // Projects (top-level, no parent): project:<id> -> { name, created,
    // updated, owner }. Decode the desired set, then drop / add / update the
    // local projection to match the document — no re-capture.
    struct ProjectEntry {
        std::string name;
        std::string created;
        std::string updated;
        std::string owner;
    };
    std::map<std::string, ProjectEntry> projects;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kProjectPrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) {
            continue;
        }
        projects.emplace(std::move(id),
                         ProjectEntry{value.value("name", std::string{}),
                                      value.value("created", std::string{}),
                                      value.value("updated", std::string{}),
                                      value.value("owner", std::string{})});
    }
    std::vector<std::string> local_project_ids;
    for (const auto& project : projects_) {
        local_project_ids.push_back(project->id());
    }
    for (const std::string& project_id : local_project_ids) {
        if (!projects.contains(project_id)) materialize_drop_project(project_id);
    }
    for (const auto& [id, entry] : projects) {
        auto project = find_project_by_id(id);
        if (!project) {
            materialize_project(id, entry.name, entry.created, entry.updated,
                                entry.owner);
        } else if (project->name() != entry.name ||
                   project->updated() != entry.updated ||
                   project->owner() != entry.owner) {
            project->set_metadata(entry.name, entry.created, entry.updated,
                                  entry.owner);  // a peer changed it (no re-capture)
        }
    }

    // Project inputs (owned by a project): input:<id> -> { project, kind,
    // source, filename, content_hash, size, created_by, created_at }. Inputs are
    // immutable provenance records (never renamed/edited), so reconcile only adds
    // them under their owning project and drops the ones a peer removed. The
    // payload body is not in the document and does not transfer yet.
    struct InputEntry {
        std::string project_id;
        ProjectInputKind kind;
        ProjectInputSource source;
        std::string filename;
        std::string content_hash;
        std::uintmax_t size;
        std::string created_by;
        std::string created_at;
    };
    std::map<std::string, InputEntry> inputs;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kInputPrefix);
        if (id.empty() || !value.is_object() || !value.contains("project")) {
            continue;
        }
        const auto kind = project_input_kind_from(
            value.value("kind", std::string{}));
        const auto source = project_input_source_from(
            value.value("source", std::string{}));
        if (!kind || !source) continue;  // boundary validation
        inputs.emplace(std::move(id),
                       InputEntry{value.value("project", std::string{}), *kind,
                                  *source, value.value("filename", std::string{}),
                                  value.value("content_hash", std::string{}),
                                  value.value("size", std::uintmax_t{0}),
                                  value.value("created_by", std::string{}),
                                  value.value("created_at", std::string{})});
    }
    // Removals first (a peer dropped an input, or its project is gone).
    for (const auto& project : projects_) {
        std::vector<std::string> local_input_ids;
        for (const auto& input : project->inputs()) {
            local_input_ids.push_back(input->id());
        }
        for (const std::string& input_id : local_input_ids) {
            const auto it = inputs.find(input_id);
            if (it == inputs.end() || it->second.project_id != project->id()) {
                project->materialize_drop_input(input_id);
            }
        }
    }
    // Additions (under the owning project, if it exists locally).
    for (const auto& [id, entry] : inputs) {
        auto project = find_project_by_id(entry.project_id);
        if (project && !project->find_input(id)) {
            project->materialize_input(id, entry.kind, entry.source,
                                       entry.filename, entry.content_hash,
                                       entry.size, entry.created_by,
                                       entry.created_at);
        }
    }

    // Execution graphs (under a project): graph:<id> -> { project, name,
    // graph: {nodes, edges} }. Decode the desired set, parsing each graph
    // against the IR schema — a graph that doesn't parse is skipped, so a
    // malformed peer payload can't corrupt the projection — then drop / add /
    // update the local projection to match. Runs after projects so a graph's
    // parent already exists locally.
    struct GraphEntry {
        std::string project_id;
        std::string name;
        ir::ExecutionGraph graph;
    };
    std::map<std::string, GraphEntry> desired_graphs;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kGraphPrefix);
        if (id.empty() || !value.is_object() || !value.contains("project") ||
            !value.contains("graph")) {
            continue;
        }
        std::optional<ir::ExecutionGraph> graph =
            ir::from_json(value["graph"]);
        if (!graph) continue;  // boundary validation: drop a malformed graph
        desired_graphs.emplace(
            std::move(id),
            GraphEntry{value.value("project", std::string{}),
                       value.value("name", std::string{}), std::move(*graph)});
    }
    for (const auto& project : projects_) {
        std::vector<std::string> local_graph_ids;
        for (const auto& graph : project->graphs()) {
            local_graph_ids.push_back(graph->id());
        }
        for (const std::string& graph_id : local_graph_ids) {
            const auto it = desired_graphs.find(graph_id);
            if (it == desired_graphs.end() ||
                it->second.project_id != project->id()) {
                project->materialize_drop_graph(graph_id);
            }
        }
    }
    for (auto& [id, entry] : desired_graphs) {
        auto project = find_project_by_id(entry.project_id);
        if (!project) continue;  // dangling graph (project gone) — ignore
        auto existing = project->find_graph_by_id(id);
        if (!existing) {
            project->materialize_graph(id, entry.name, std::move(entry.graph));
        } else if (existing->name() != entry.name ||
                   ir::to_json(existing->graph()) != ir::to_json(entry.graph)) {
            existing->set_contents(entry.name,
                                   std::move(entry.graph));  // a peer changed it
        }
    }

    // Deployments (top-level, no parent): deployment:<id> -> { graph_name,
    // target, node, node_count, edge_count, created }. Decode the desired set,
    // then drop / add the local projection to match the document. Deployments
    // are immutable once recorded in this slice (no update branch); reconcile /
    // self-heal that mutates them is a later task.
    struct DeploymentEntry {
        std::string graph_name;
        DeploymentTarget target;
        std::string node_id;
        std::size_t node_count;
        std::size_t edge_count;
        std::string created;
    };
    std::map<std::string, DeploymentEntry> deployments;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kDeploymentPrefix);
        if (id.empty() || !value.is_object() || !value.contains("graph_name")) {
            continue;
        }
        const std::optional<DeploymentTarget> target =
            DeploymentTarget::from_json(
                value.value("target", nlohmann::json::object()));
        if (!target) continue;
        deployments.emplace(
            std::move(id),
            DeploymentEntry{value.value("graph_name", std::string{}), *target,
                            value.value("node", std::string{}),
                            value.value("node_count", std::size_t{0}),
                            value.value("edge_count", std::size_t{0}),
                            value.value("created", std::string{})});
    }
    std::vector<std::string> local_deployment_ids;
    for (const auto& deployment : deployments_) {
        local_deployment_ids.push_back(deployment->id());
    }
    for (const std::string& deployment_id : local_deployment_ids) {
        if (!deployments.contains(deployment_id)) {
            materialize_drop_deployment(deployment_id);
        }
    }
    for (const auto& [id, entry] : deployments) {
        if (!find_deployment(id)) {
            materialize_deployment(id, entry.graph_name, entry.target,
                                   entry.node_id, entry.node_count,
                                   entry.edge_count, entry.created);
        }
    }

    // Runs (top-level, no parent): run:<id> -> the run value. Decode + validate
    // the desired set at the boundary (a malformed CRDT value is ignored, not
    // trusted), then drop / add / update the local projection to match — with no
    // re-capture (apply_value / materialize_run do not write back to the CRDT).
    std::map<std::string, RunValue> runs;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kRunPrefix);
        if (id.empty()) continue;
        if (std::optional<RunValue> parsed = parse_run_value(value)) {
            runs.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_run_ids;
    for (const auto& run : runs_) local_run_ids.push_back(run->id());
    for (const std::string& run_id : local_run_ids) {
        if (!runs.contains(run_id)) materialize_drop_run(run_id);
    }
    for (auto& [id, value] : runs) {
        auto run = find_run(id);
        if (!run) {
            materialize_run(id, std::move(value));
        } else if (run->metadata() != to_json(value)) {
            run->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }

    // Principals (top-level, no parent): principal:<id> -> { kind, name,
    // created }. Decode the desired set, then drop / add / update the local
    // projection to match the document — no re-capture.
    struct PrincipalEntry {
        PrincipalKind kind;
        std::string name;
        std::string created;
    };
    std::map<std::string, PrincipalEntry> principals;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPrincipalPrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) {
            continue;
        }
        const auto kind = principal_kind_from(value.value("kind", std::string{}));
        if (!kind) continue;  // boundary validation
        principals.emplace(std::move(id),
                           PrincipalEntry{*kind,
                                          value.value("name", std::string{}),
                                          value.value("created", std::string{})});
    }
    std::vector<std::string> local_principal_ids;
    for (const auto& principal : principals_) {
        local_principal_ids.push_back(principal->id());
    }
    for (const std::string& principal_id : local_principal_ids) {
        if (!principals.contains(principal_id)) {
            materialize_drop_principal(principal_id);
        }
    }
    for (const auto& [id, entry] : principals) {
        auto principal = find_principal_by_id(id);
        if (!principal) {
            materialize_principal(id, entry.kind, entry.name, entry.created);
        } else if (principal->name() != entry.name ||
                   principal->kind() != entry.kind) {
            principal->set_metadata(entry.kind, entry.name,
                                    entry.created);  // a peer changed it (no re-capture)
        }
    }

    // Access keys (owned by a principal): key:<id> -> { principal, scope,
    // created }. Keys are immutable credentials (never edited), so reconcile only
    // adds them under their owning principal and drops the ones a peer removed.
    struct AccessKeyEntry {
        std::string principal_id;
        Scope scope;
        std::string created;
    };
    std::map<std::string, AccessKeyEntry> access_keys;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kAccessKeyPrefix);
        if (id.empty() || !value.is_object() || !value.contains("principal") ||
            !value.contains("scope")) {
            continue;
        }
        Scope scope;
        if (!scope_from_json(value["scope"], scope)) continue;  // boundary
        access_keys.emplace(
            std::move(id),
            AccessKeyEntry{value.value("principal", std::string{}),
                           std::move(scope),
                           value.value("created", std::string{})});
    }
    // Removals first (a peer dropped a key, or its principal is gone).
    for (const auto& principal : principals_) {
        std::vector<std::string> local_key_ids;
        for (const auto& key : principal->keys()) {
            local_key_ids.push_back(key->id());
        }
        for (const std::string& key_id : local_key_ids) {
            const auto it = access_keys.find(key_id);
            if (it == access_keys.end() ||
                it->second.principal_id != principal->id()) {
                principal->materialize_drop_key(key_id);
            }
        }
    }
    // Additions (under the owning principal, if it exists locally).
    for (auto& [id, entry] : access_keys) {
        auto principal = find_principal_by_id(entry.principal_id);
        if (principal && !principal->find_key(id)) {
            principal->materialize_key(id, std::move(entry.scope),
                                       entry.created);
        }
    }
}

IntegrityReport Node::check_integrity() const {
    IntegrityReport report;
    for (const auto& db : databases_) {
        ++report.databases;
        for (const auto& table : db->tables()) {
            ++report.tables;
            report.rows += table->row_count();
        }
        for (const LargeObject& object : db->large_objects().list()) {
            ++report.large_objects;
            if (!object.body_present) {
                ++report.large_object_bodies_missing;
                continue;
            }
            ++report.large_object_bodies_present;
            // Re-hash the on-disk body and compare to the recorded digest.
            const std::optional<std::string> body =
                db->large_objects().read(object.id);
            if (!body || (!object.sha256.empty() &&
                          sha256_hex(*body) != object.sha256)) {
                ++report.corrupt_large_objects;
            }
        }
    }

    // Dangling references in the replicated catalog: a table/large object whose
    // database is gone, or a row whose table is gone (orphaned keys the
    // reconciler ignores but that shouldn't accumulate).
    if (replication_) {
        const nlohmann::json snapshot = replication_->snapshot();
        std::unordered_set<std::string> database_ids;
        std::unordered_set<std::string> table_ids;
        std::unordered_set<std::string> project_ids;
        std::unordered_set<std::string> principal_ids;
        for (const auto& [key, value] : snapshot.items()) {
            if (std::string id = catalog::id_after(key, catalog::kDatabasePrefix);
                !id.empty()) {
                database_ids.insert(std::move(id));
            } else if (std::string table_id =
                           catalog::id_after(key, catalog::kTablePrefix);
                       !table_id.empty()) {
                table_ids.insert(std::move(table_id));
            } else if (std::string project_id =
                           catalog::id_after(key, catalog::kProjectPrefix);
                       !project_id.empty()) {
                project_ids.insert(std::move(project_id));
            } else if (std::string principal_id =
                           catalog::id_after(key, catalog::kPrincipalPrefix);
                       !principal_id.empty()) {
                principal_ids.insert(std::move(principal_id));
            }
        }
        const auto parent_missing = [](const nlohmann::json& value,
                                       const char* field,
                                       const std::unordered_set<std::string>& set) {
            return value.is_object() && value.contains(field) &&
                   !set.contains(value[field].get<std::string>());
        };
        for (const auto& [key, value] : snapshot.items()) {
            if (!catalog::id_after(key, catalog::kTablePrefix).empty()) {
                if (parent_missing(value, "db", database_ids)) ++report.dangling_tables;
            } else if (!catalog::id_after(key, catalog::kRowPrefix).empty()) {
                if (parent_missing(value, "tbl", table_ids)) ++report.dangling_rows;
            } else if (!catalog::id_after(key, catalog::kLargeObjectPrefix).empty()) {
                if (parent_missing(value, "db", database_ids)) {
                    ++report.dangling_large_objects;
                }
            } else if (!catalog::id_after(key, catalog::kInputPrefix).empty()) {
                if (parent_missing(value, "project", project_ids)) {
                    ++report.dangling_inputs;
                }
            } else if (!catalog::id_after(key, catalog::kGraphPrefix).empty()) {
                if (parent_missing(value, "project", project_ids)) {
                    ++report.dangling_graphs;
                }
            } else if (!catalog::id_after(key, catalog::kAccessKeyPrefix)
                            .empty()) {
                if (parent_missing(value, "principal", principal_ids)) {
                    ++report.dangling_access_keys;
                }
            }
        }
    }
    return report;
}

std::string Node::status_line() const {
    if (!running_) return "stopped";
    char buf[224];
    std::snprintf(
        buf, sizeof(buf),
        "running — up %.0fs, %llu ticks, %zu databases, %zu projects, "
        "%zu deployments, %zu runs, %zu principals",
        uptime_seconds(), static_cast<unsigned long long>(ticks_),
        databases_.size(), projects_.size(), deployments_.size(),
        runs_.size(), principals_.size());
    return buf;
}

}  // namespace zetta
