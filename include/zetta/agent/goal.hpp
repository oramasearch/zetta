#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/action.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/interaction.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/reward.hpp"

// An agent's GOALS — what it is being trained to be good at, and how much each one matters.
//
// Why this exists. RLVR needs a reward, and until now the reward was fixed in code: the end-state
// grader plus a tool-discipline rubric, identical for every agent. But "good" is not the same for
// every deployment. A regulated lender cares most that nothing is claimed it cannot evidence; an
// ops team cares most that nothing acts without approval; a growth team cares most that a Pipeline
// gets built at all. Goals make that difference DATA on the agent record rather than a fork of the
// engine, and the RLVR loop anchors to them: the reward is the importance-weighted satisfaction of
// the agent's own goals, so training moves the policy toward what this customer actually wants.
//
// THE DISCIPLINE THAT KEEPS THIS RLVR. A goal is written in plain language, but its reward must be
// VERIFIABLE — a check that runs, not a preference that is learned. So a goal resolves to one or
// more deterministic checks over the facts a response projects (`response_solution_facts`), and a
// goal that resolves to NO check earns nothing and is reported as unmapped. A model may choose the
// mapping (see `GoalInterpreter`) — that is judgement about intent, made once, offline, and
// inspectable — but the check it selects is deterministic and the reward stays a measurement. The
// moment a model scores the ROLLOUT instead of selecting the check, this stops being verifiable
// reward and becomes a learned reward model, with all the reward-hacking that invites.
//
// #555 (the base agent) · #853 (the per-customer fork, where goals differ per customer) ·
// docs/agent/README.md.
namespace zetta::agent {

// One thing the agent is meant to be good at, and how much it matters relative to the others.
struct Goal {
    std::string statement;    // plain language: "never state a number we cannot evidence"
    double importance = 1.0;  // relative weight (>= 0); 0 means "stated but not trained on"

    // The checks this goal resolves to — keys of the response projection that must hold.
    // Populated by a GoalInterpreter (or set directly). Empty = unmapped: it earns nothing, and
    // `unmapped_goals` reports it rather than letting it silently pass.
    std::vector<std::string> checks;

    bool operator==(const Goal&) const = default;
};

// A named, deterministic check over the facts a response projects.
//
// The catalogue is deliberately small and honest: every entry corresponds to something
// `response_solution_facts` can actually decide from a response. A goal that wants something the
// projection cannot see is unmapped until the projection can see it — which is the same rule the
// corpus follows ("a required fact must be projectable"), for the same reason: you cannot reward
// what you cannot check.
struct GoalCheck {
    std::string name;         // "numbers_from_context", "approval_gated", …
    std::string description;  // what it means, in the vocabulary a user reads
};

// Every check a goal may resolve to today.
const std::vector<GoalCheck>& goal_check_catalogue();

// Map a plain-language goal onto checks from the catalogue. Deterministic keyword matching — the
// first implementation, and the fallback whenever no model is available. Returns the check names;
// empty when nothing matched (an unmapped goal, which must not be silently rewarded).
std::vector<std::string> interpret_goal(const std::string& statement);

// Resolve every goal's checks in place, for goals that do not already carry them.
void interpret_goals(std::vector<Goal>& goals);

// The rubric terms an agent's goals produce for one response.
//
// One term per goal: weight = importance, score = the fraction of its checks the response
// satisfied, active = the response exercised at least one of them. Feeding these to
// `rubric_reward` gives the importance-weighted reward, renormalized over the goals the response
// actually engaged — so an agent is not penalized on a goal the turn had no bearing on.
std::vector<RubricTerm> goal_rubric_terms(const std::vector<Goal>& goals, const EndState& facts);

// The reward in [0,1] for one response under `goals`. No goals, or none engaged, returns 0 —
// nothing was measured, which is not the same as doing badly and must not read as success.
double goal_reward(const std::vector<Goal>& goals, const EndState& facts);

// Goals that resolved to no check. Surfaced rather than ignored: a goal nobody can verify is a
// goal the training silently drops, and a user who wrote it deserves to be told.
std::vector<std::string> unmapped_goals(const std::vector<Goal>& goals);

// What a turn's tool calls ACTUALLY DID, as gradeable facts (#941).
//
// Every other projection describes what a response CLAIMED. This one describes what happened when
// the harness ran it — the sandbox's own verdict per call. A response can satisfy every projected
// fact and still not survive execution, and rewarding it would be rewarding a claim.
//
// Takes ToolOutcome — the same type a turn's history carries, so what the reward grades and
// what a follow-up sees are one shape rather than two that must be kept in step.
//
//   tools_ran            every call succeeded
//   tools_failed         at least one ran and reported failure
//   called_unknown_tool  it named a tool that does not exist — it invented a capability, which is
//                        a different mistake from misusing a real one and worth distinguishing
//
// A turn that called NOTHING projects none of these. Absent, not false: an answer that ran no
// tools must not score full marks on a goal about tools running, and `false` would read as a
// measured failure rather than as nothing measured.
EndState execution_facts(const std::vector<ToolOutcome>& outcomes);

// The RLVR reward for one generated response, anchored to an agent's goals.
//
// This is the connection the loop was missing: `grpo_step` takes a `CompletionReward`, and until
// now callers hand-built one around a fixed rubric. Here the reward comes from the AGENT — its
// goals, its importances — so training moves the policy toward what this deployment actually wants
// rather than toward what was compiled in.
//
// `generation` is the model's raw text. It is parsed as a StudioAction and projected through
// `response_solution_facts`; a generation that does not parse scores 0, because a response the
// harness cannot execute is worth nothing regardless of what it says.
//
// `grounding` is the VERIFIER's view, which may be richer than what the policy saw — the same
// asymmetry the rollout harvest relies on. A product answer can be served with no capability
// listing and still be scored against one.
// `history` reaches `response_solution_facts`, so a generation that simply repeats the previous
// reply grades as `repeated_prior_turn` and can be penalized. Without it the reward is blind to
// the relationship between turns, and a model that answers every follow-up with the same sentence
// scores exactly as well as one that actually follows the thread — which is how that behavior
// survived to serving. Empty history (every single-turn caller) grades unchanged.
double goal_reward_for_generation(const std::vector<Goal>& goals, const std::string& generation,
                                  const std::string& grounding,
                                  const std::vector<PriorTurn>& history = {});

// The reward for one turn, counting BOTH what the response claimed and what its calls actually
// did (#941).
//
// This is the distinction that makes the reward verifiable rather than merely well-formed: two
// identical actions — same message, same tool calls, same projected facts — score differently when
// one's calls succeeded and the other's did not. A projection cannot tell them apart.
//
// Execution does not replace the projection. A goal about grounding still grades the response, a
// goal about execution grades the outcome, and a reward can be about both. Where they overlap, the
// execution fact wins, because what happened outranks what was claimed.
double goal_reward_for_execution(const std::vector<Goal>& goals, const StudioAction& action,
                                 const std::string& grounding,
                                 const std::vector<ToolOutcome>& outcomes);

// The RLVR reward for one rollout: parse the generation, RUN its tool calls in `sandbox`, and
// grade what running them did (#941).
//
// This is the reward `grpo_step` should take. Everything before it graded text; this grades the
// environment's answer. A rollout naming a tool the agent does not have earns less than one that
// runs — which no amount of reading the text could establish.
//
// The sandbox is MUTATED, so the caller supplies a fresh one per rollout when rollouts must not
// see each other's effects. A generation that does not parse is never executed and scores 0:
// running half a malformed action is worse than not running it.
double goal_reward_for_rollout(const std::vector<Goal>& goals, const std::string& generation,
                               const std::string& grounding, Sandbox& sandbox);

nlohmann::json to_json(const Goal& goal);
std::optional<Goal> goal_from_json(const nlohmann::json& value, std::string* error = nullptr);
const nlohmann::json& goal_schema();

}  // namespace zetta::agent
