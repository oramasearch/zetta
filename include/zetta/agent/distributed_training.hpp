// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>

#include "zetta/agent/distributed.hpp"     // AllReduce
#include "zetta/agent/softmax_policy.hpp"
#include "zetta/agent/training.hpp"        // TrainingConfig, TrainingReport, UniformSource
#include "zetta/runtime.hpp"

// The distributed, coroutine-native RLVR training loop (#555; docs/agent/README.md)
// — the real training path, as the zetta first principle requires (native C++26,
// coroutine-based, cluster-native, distributed). Within a step, this node's local
// rollouts fan out CONCURRENTLY via runtime::when_all (each rollout is an io_task,
// so a real tool call's I/O overlaps instead of running one at a time), and the two
// global quantities — the GRPO group statistics and the policy gradient — are
// combined across the cluster with the injected AllReduce. Rollouts are the
// machine + cluster fan-out; the allreduce is the collective combine. With
// identity_allreduce on one node this is a single-node run; with a Collectives-
// backed allreduce on N nodes it is the same training, distributed — single-node ==
// multi-node by construction.
namespace zetta::agent {

// A rollout's reward as an awaitable: run the agent in the environment (whose tool
// calls may be real async I/O) and yield the scalar reward. Injected, so the loop
// is environment- and backend-agnostic.
using RewardTask = std::function<runtime::io_task<double>(std::size_t action)>;

// Train `policy` in place. `config.group_size` is the LOCAL group this node samples
// per step; the global group is that times the number of nodes (the allreduce makes
// the statistics global). Returns the per-step GLOBAL mean reward — the learning
// curve. A collective failure ends training early with the curve so far. Awaited via
// runtime::block_on from non-coroutine code (main / tests).
runtime::task<TrainingReport> train_softmax_policy_distributed(
    SoftmaxPolicy& policy, const RewardTask& reward, const UniformSource& uniform,
    const AllReduce& allreduce, const TrainingConfig& config = {});

}  // namespace zetta::agent
