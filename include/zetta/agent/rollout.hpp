#pragma once

#include <cstddef>
#include <vector>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/policy.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/task.hpp"
#include "zetta/agent/tool_call.hpp"

// A rollout runs an AgentPolicy against a task's sandbox for a bounded number of
// turns and captures what happened — the trajectory (each call + its outcome) and
// the final world-state the grader then scores (#555; docs/agent/README.md). The
// turn budget is a hard bound: an agent that never stops must still terminate, so
// a rollout always ends and always yields a final state to reward.
namespace zetta::agent {

// One turn: the call the policy made and what the sandbox did with it.
struct RolloutTurn {
    ToolCall call;
    ApplyOutcome outcome;
};

// The record of a completed rollout.
struct Rollout {
    std::vector<RolloutTurn> turns;
    EndState final_state;
    bool stopped_by_policy = false;  // true if the policy stopped; false if the
                                     // turn budget was exhausted first
};

// Run `policy` against `sandbox` (its live world) for at most `max_turns` turns,
// with `task` as the context the policy sees. Each non-stop decision is applied to
// the sandbox and recorded — including a failed or unknown call, so the trajectory
// is honest about what happened. Returns the trajectory + the final world-state.
Rollout run_rollout(AgentPolicy& policy, const Task& task, Sandbox& sandbox,
                    std::size_t max_turns);

}  // namespace zetta::agent
