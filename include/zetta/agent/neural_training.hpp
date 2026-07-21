#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "zetta/agent/distributed.hpp"   // AllReduce
#include "zetta/agent/linear_policy.hpp"
#include "zetta/agent/training.hpp"      // TrainingConfig, TrainingReport, UniformSource
#include "zetta/compute/compute_device.hpp"
#include "zetta/runtime.hpp"

// Distributed RLVR training of a real neural policy's WEIGHTS (#555;
// docs/agent/README.md) — the capstone that closes the loop: the forward pass runs
// on a ComputeDevice (the GPU under MlxDevice), and the policy's weight matrix W is
// trained by the same distributed, coroutine-native RLVR loop. Unlike the tabular
// SoftmaxPolicy, a LinearPolicy conditions on a context (feature vector), so this
// learns a CONTEXTUAL policy — a tabular policy cannot — which is why it exercises
// the neural network rather than a lookup table.
//
// The weight gradient is the policy gradient chained through the linear layer: for
// a rollout with context x, sampled action a, advantage A, and policy pi,
//   dW[k][f] += A * (1[k==a] - pi_k) * x[f]        (an outer product),
// summed across the group and allreduced across the cluster, then W += lr * dW.
// Single-node == multi-node via the injected allreduce.
namespace zetta::agent {

// Sample a context (feature vector) for a rollout. Injected.
using ContextSource = std::function<std::vector<float>()>;

// Score an action taken in a context — the rollout's reward, as an awaitable (its
// tool calls may be real async I/O). Injected.
using ContextualRewardTask = std::function<runtime::io_task<double>(
    const std::vector<float>& context, std::size_t action)>;

// Train `policy`'s weights in place. Per step: sample a local group of contexts,
// forward W*x on `device` to get each policy, sample an action, fan the reward
// rollouts out via when_all, form GRPO advantages (global via allreduce), accumulate
// and allreduce the weight gradient, and update W. Returns the per-step global mean
// reward. Awaited via runtime::block_on.
runtime::task<TrainingReport> train_linear_policy_distributed(
    LinearPolicy& policy, compute::ComputeDevice& device,
    const ContextSource& context, const ContextualRewardTask& reward,
    const UniformSource& uniform, const AllReduce& allreduce,
    const TrainingConfig& config = {});

}  // namespace zetta::agent
