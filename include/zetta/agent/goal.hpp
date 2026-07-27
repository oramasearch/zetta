#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
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

nlohmann::json to_json(const Goal& goal);
std::optional<Goal> goal_from_json(const nlohmann::json& value, std::string* error = nullptr);
const nlohmann::json& goal_schema();

}  // namespace zetta::agent
