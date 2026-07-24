#include "zetta/agent/rollout.hpp"

#include <utility>

namespace zetta::agent {

Rollout run_rollout(AgentPolicy& policy, const Task& task, Sandbox& sandbox,
                    std::size_t max_turns) {
    Rollout rollout;
    for (std::size_t turn = 0; turn < max_turns; ++turn) {
        const PolicyDecision decision =
            policy.next(RolloutStep{task, sandbox.state(), turn});
        if (decision.stop || !decision.call.has_value()) {
            rollout.stopped_by_policy = true;
            break;
        }
        ApplyOutcome outcome = sandbox.apply(*decision.call);
        rollout.turns.push_back(RolloutTurn{*decision.call, std::move(outcome)});
    }
    rollout.final_state = sandbox.state();
    return rollout;
}

}  // namespace zetta::agent
