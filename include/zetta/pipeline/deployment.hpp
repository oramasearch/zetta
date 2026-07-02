#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/deployment_target.hpp"

namespace zetta::pipeline {

// A recorded **placement of an execution graph onto a target** (ENT-6 / FEAT-4).
// Deploying a validated ExecutionGraph to a DeploymentTarget records one of
// these: the orchestrator's desired state — "this graph should run here". The
// reconcile/self-heal loop that *acts* on it (drives actual → desired) is a
// later slice; this slice records + surfaces the deployment.
//
// A Deployment is **replicated state** like Project: the Automerge CRDT document
// is the source of truth (flat key `deployment:<uuid>`), and a Deployment is its
// in-memory + on-disk projection — a folder named by the UUID under the data dir
// holding deployment.json. Lookups hand back shared_ptr handles, never raw
// pointers.
//
// The deployed graph is recorded as a **summary** (its name + node/edge counts),
// not the full graph: the ExecutionGraph IR has no serialized form yet (a later
// slice). When it does, a deployment will reference the stored graph by id; the
// summary is the honest record until then. A deployment is immutable once
// recorded in this slice (no rename/update path).
class Deployment {
public:
    Deployment(std::string id, std::string graph_name, DeploymentTarget target,
               std::string node_id, std::size_t node_count,
               std::size_t edge_count, std::string created,
               std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& graph_name() const { return graph_name_; }
    const DeploymentTarget& target() const { return target_; }
    const std::string& node_id() const { return node_id_; }
    std::size_t node_count() const { return node_count_; }
    std::size_t edge_count() const { return edge_count_; }
    const std::string& created() const { return created_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Write this deployment's metadata (id + fields) to dir/deployment.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (without the id, which is the CRDT key
    // / folder name):
    //   { graph_name, target: {…}, node, node_count, edge_count, created }
    nlohmann::json metadata() const;

private:
    std::string id_;
    std::string graph_name_;
    DeploymentTarget target_;
    std::string node_id_;     // the node the graph was placed on (cluster id)
    std::size_t node_count_;  // graph summary captured at deploy time
    std::size_t edge_count_;  // graph summary captured at deploy time
    std::string created_;
    std::filesystem::path dir_;
};

}  // namespace zetta::pipeline