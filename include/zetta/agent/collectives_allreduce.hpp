#pragma once

#include <chrono>

#include "zetta/agent/distributed.hpp"

// Bridge the cluster's real collective to the RLVR loop's injected AllReduce (#555;
// docs/agent/README.md). identity_allreduce is the single-node stand-in;
// collectives_allreduce is the REAL cross-node combine — it forwards to
// cluster::Collectives::allreduce over the signal plane, so N cluster nodes train
// one policy in lock-step. This is the one adapter that ties the distributed RLVR
// loop to the running cluster, the same way the GBDT job's allreduce ties to it.
namespace zetta::cluster {
class Collectives;
}

namespace zetta::agent {

// Adapt `collectives` to an AllReduce: maps Reduce -> CollectiveOp and forwards the
// round. An unreachable peer, a length mismatch, or a timeout yields an empty
// vector — which the loop treats as a collective failure (it stops rather than
// training on partial data). The reference is captured, so `collectives` must
// outlive the returned callable.
AllReduce collectives_allreduce(
    cluster::Collectives& collectives,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

}  // namespace zetta::agent
