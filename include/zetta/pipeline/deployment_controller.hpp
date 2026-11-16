// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zetta { class Node; }

namespace zetta::pipeline {

// build; surfaced in the status line and asserted in tests.
struct ReconcileReport {
    std::size_t local_deployments = 0;  // deployments placed on THIS node (R1)
    std::size_t satisfied = 0;          // already had a healthy run
    std::size_t runs_started = 0;       // runs (re)started this pass
    std::vector<std::string> healed;    // deployment ids a run was started for
};

// The **control plane** for local deployments — the reconcile/self-heal loop
// (FEAT-4, TC-3). A Deployment records *desired* state ("this graph should run
// here"); a Run is the *actual* execution of it. This controller drives actual →
// desired: for every deployment placed on **this** node it ensures there is a
// healthy run, and **self-heals** by starting a replacement when the placed run
// is killed (dropped) or fails.
//
// Driven by Node::tick() (the heartbeat), so it runs automatically in headless,
// headed, and tests. Steady state — every local deployment already satisfied —
// is a read-only scan that captures nothing, so it adds no replication churn.
//
// R1 is **local only**: a node reconciles the deployments it itself placed
// (target local, `node == node_id()`). Deployments placed on a *peer* are that
// peer's responsibility; cross-node failover (a survivor adopting a dead node's
// placements) needs leadership/lease (see docs/cluster/replication.md → Consistency
// model) and is a later slice — like ClusterRole, declared before it is built.
class DeploymentController {
public:
    explicit DeploymentController(Node& node) : node_(node) {}

    DeploymentController(const DeploymentController&) = delete;
    DeploymentController& operator=(const DeploymentController&) = delete;

    // One reconcile pass over this node's local deployments. Satisfied = the
    // deployment has a run that is pending, running, or succeeded; otherwise
    // (no run, or all of its runs failed) a fresh `running` run is started and
    // recorded against the deployment. Returns what it saw + did.
    ReconcileReport reconcile();

    // Cumulative counters across all passes since construction (observability).
    std::uint64_t total_runs_started() const { return total_runs_started_; }
    std::uint64_t passes() const { return passes_; }

private:
    Node& node_;
    std::uint64_t total_runs_started_ = 0;
    std::uint64_t passes_ = 0;
};

}  // namespace zetta::pipeline