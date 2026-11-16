// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/goal.hpp"

// When an agent should be retrained, and what it was trained on — the policy behind manual and
// scheduled retraining, and the version history that makes a retrain reversible.
//
// The load-bearing idea is the ENVIRONMENT FINGERPRINT. An agent's behaviour is a function of the
// world it was trained against: the tools it may call, the goals it is rewarded on, and the corpus
// it learned from. Change any of those and the deployed checkpoint is training for a world that no
// longer exists — it will keep calling a tool you removed, and keep optimizing a goal you
// re-weighted. Recording the fingerprint on each trained version turns "is this agent stale?" from
// a judgement call into a comparison, which is what lets the UI say so plainly and a schedule act
// on it.
//
// Why a fingerprint rather than a timestamp: editing a description, renaming an agent, or
// retraining on the same inputs should NOT mark anything stale. Only a change to what the agent can
// do, what it is rewarded for, or what it learned from does — and those are exactly the three
// fields hashed here.
//
// #849 (the agent record) · #853 (per-customer forks, retrained as their data moves) ·
// #555 (the base agent).
namespace zetta::agent {

// How often an agent retrains on its own. Deliberately coarse — an agent is not a cron daemon, and
// a cadence a user can pick from a menu is one they can reason about. `Manual` is the default: an
// agent never retrains behind your back.
enum class RetrainCadence {
    Manual,      // only when someone asks
    OnChange,    // whenever the environment fingerprint moves (tools/goals/corpus)
    Daily,
    Weekly,
    Monthly,
};

std::string_view to_string(RetrainCadence cadence);
std::optional<RetrainCadence> retrain_cadence_from_string(std::string_view name);

// One trained checkpoint of an agent — a **Saved Version** in the language the UI uses.
//
// Kept in full rather than replaced, because retraining is the operation most likely to make an
// agent worse: a corpus change lands, the new checkpoint regresses on the thing the customer
// cared about, and the only acceptable answer is pointing back at the version that worked. That
// requires the old checkpoint to still exist and to still say what it was trained against.
struct AgentVersion {
    std::string checkpoint;    // the model dir this version produced
    std::string created_at;    // ISO-8601, supplied by the caller (no clock in the core)
    std::string job;           // the ComputeJob that produced it (empty if trained elsewhere)
    std::string fingerprint;   // the environment it was trained FOR (see environment_fingerprint)
    std::string corpus;        // the corpus it learned from
    double eval_score = -1.0;  // the canary number, when one was measured; < 0 = not evaluated

    bool operator==(const AgentVersion&) const = default;
};

// What an agent retrains ON. Empty corpus = the built-in base-agent corpus.
struct TrainingSpec {
    std::string corpus;              // corpus id or path
    std::string base_checkpoint;     // what to start from; empty = the agent's current model
    int steps = 0;                   // 0 = the driver's default (one epoch)
    RetrainCadence cadence = RetrainCadence::Manual;

    bool operator==(const TrainingSpec&) const = default;
};

// A stable hash of the world the agent is trained FOR: its tools, its goals (statement +
// importance + resolved checks), and its corpus. Order-insensitive over tools and goals, so
// reordering a list is not a change.
std::string environment_fingerprint(const std::vector<std::string>& tools,
                                    const std::vector<Goal>& goals, const std::string& corpus);

// Is the newest version stale — trained for a different environment than the one in force?
//
// Answers false when there are no versions at all: an agent that has never been trained is not
// stale, it is untrained, and conflating the two would have every fresh agent nagging to retrain.
bool needs_retraining(const std::vector<AgentVersion>& versions,
                      const std::string& current_fingerprint);

// Is a scheduled retrain due? `now` and `last` are ISO-8601; the comparison is lexicographic,
// which is valid for that format and keeps a clock out of the core.
//
// OnChange is due exactly when the fingerprint moved — a schedule that fires on nothing changing
// burns a GPU to reproduce the checkpoint it already has.
bool retrain_due(const TrainingSpec& spec, const std::vector<AgentVersion>& versions,
                 const std::string& current_fingerprint, const std::string& now);

// Is this agent stale, and why — the answer a UI shows next to a Retrain button.
//
// One call rather than three, because "should I retrain?" is one question and answering it from
// scattered predicates is how a console ends up disagreeing with a scheduler.
struct RetrainStatus {
    bool trained = false;        // has it ever produced a version?
    bool stale = false;          // was the newest version trained for a different environment?
    bool due = false;            // does its cadence say retrain now?
    std::string fingerprint;     // the environment in force
    std::string reason;          // one line, in the vocabulary a user reads
};

// `now` is ISO-8601, supplied by the caller — zetta-core holds no clock.
RetrainStatus retrain_status(const TrainingSpec& spec, const std::vector<AgentVersion>& versions,
                             const std::vector<std::string>& tools, const std::vector<Goal>& goals,
                             const std::string& now);

nlohmann::json to_json(const AgentVersion& version);
nlohmann::json to_json(const TrainingSpec& spec);
std::optional<AgentVersion> agent_version_from_json(const nlohmann::json& value,
                                                    std::string* error = nullptr);
std::optional<TrainingSpec> training_spec_from_json(const nlohmann::json& value,
                                                    std::string* error = nullptr);
const nlohmann::json& training_spec_schema();

}  // namespace zetta::agent
