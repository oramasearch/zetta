#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "zetta/agent/policy_gradient.hpp"  // PolicySample
#include "zetta/agent/softmax_policy.hpp"

// Distributed RLVR primitives (#555; docs/agent/README.md). RL is embarrassingly
// parallel: a training step's rollouts are sharded across the cluster, and the two
// quantities that must be GLOBAL — the GRPO group statistics and the policy
// gradient — are combined with an allreduce, exactly the histogram-allreduce
// pattern the GBDT trainer uses (zetta::ml::AllReduce). The cross-node combine is an
// INJECTED callable, so the single-node path passes an identity and the cluster
// passes a Collectives-backed reduce — single-node == multi-node by construction
// (the zetta distributed-first invariant).
namespace zetta::agent {

enum class Reduce { Sum, Min, Max };

// Reduce `local` elementwise across every participant, returning the global vector
// (same length). An empty return signals a collective failure — the caller then
// fails rather than training on partial data.
using AllReduce = std::function<std::vector<double>(
    const std::string& tag, const std::vector<double>& local, Reduce op)>;

// The single-node identity: this node's shard is the whole cluster.
std::vector<double> identity_allreduce(const std::string& tag,
                                       const std::vector<double>& local,
                                       Reduce op);

// The local policy-gradient contribution of `batch` at `policy`'s current
// distribution: gradient[j] = sum over samples of advantage * (1[j==a] - pi_j).
// The gradient is ADDITIVE across shards — the sum of the shards' local gradients
// equals the gradient of the combined batch — which is why an allreduce-Sum yields
// the correct global gradient.
std::vector<double> local_policy_gradient(
    const SoftmaxPolicy& policy, const std::vector<PolicySample>& batch);

// GRPO advantages for THIS node's `local_rewards`, normalized by the GLOBAL group
// statistics (mean + stddev over every node's rewards, obtained by allreducing the
// global sum, sum-of-squares, and count). Returns advantages for the local rewards
// only; empty on an allreduce failure. With identity_allreduce this equals
// group_advantages(local_rewards) (within floating-point).
std::vector<double> distributed_group_advantages(
    const std::vector<double>& local_rewards, const AllReduce& allreduce,
    const std::string& tag = "grpo-adv", double epsilon = 1e-8);

// One distributed REINFORCE/GRPO step: each node computes its LOCAL gradient
// contribution from `local_batch` at the shared policy, the gradients are summed
// across the cluster (allreduce Sum), and every node applies the same global update
// — so the policy stays bit-identical on every node. With identity_allreduce this
// equals policy_gradient_step. Returns false on an allreduce failure (the policy is
// left unchanged).
bool distributed_policy_gradient_step(
    SoftmaxPolicy& policy, const std::vector<PolicySample>& local_batch,
    const AllReduce& allreduce, double learning_rate,
    const std::string& tag = "grpo-grad");

}  // namespace zetta::agent
