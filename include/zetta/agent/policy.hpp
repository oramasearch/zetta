// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/task.hpp"
#include "zetta/agent/tool_call.hpp"

// The AgentPolicy is the actor a rollout drives: given what it can see this turn,
// it decides the next tool call — or that it is done (#555; docs/agent/README.md).
// It is the interface the trained model plugs into; tests drive rollouts with a
// scripted policy behind the same interface. Keeping the policy behind this
// interface is what lets the rollout / reward / training machinery be built and
// tested with no model present.
namespace zetta::agent {

// What the policy can see when it decides its next action.
struct RolloutStep {
    const Task& task;       // the goal, description, and tools in scope
    const EndState& state;  // the current world-state
    std::size_t turn;       // 0-based turn index
};

// The policy's decision for one turn: emit a tool call, or stop.
struct PolicyDecision {
    std::optional<ToolCall> call;  // the next call, or nullopt when stopping
    bool stop = false;             // the agent considers itself done

    static PolicyDecision act(ToolCall call) {
        return PolicyDecision{std::move(call), false};
    }
    static PolicyDecision done() { return PolicyDecision{std::nullopt, true}; }
};

class AgentPolicy {
public:
    virtual ~AgentPolicy() = default;

    // Decide the next action given what the agent can see this turn.
    virtual PolicyDecision next(const RolloutStep& step) = 0;
};

}  // namespace zetta::agent
