#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/goal.hpp"

// A served, named AGENT: the first-class entity a user lists, selects, and chats with.
// It binds a MODEL (a local checkpoint dir, a `model:<uuid>`, or a HuggingFace repo id
// `zetta pull` can fetch) to an optional SYSTEM PROMPT and the TOOLS it may call, so the
// same engine can host many specialized agents (the base agent, a customer's churn fork,
// ...). Stored as a replicated `agent:<uuid>` entity (catalog.hpp); the CRUD surface, the
// MCP/REST endpoints, and the GUI/web chat panels all read this record.
//
// Parsing is a non-throwing boundary (std::optional + a path-qualified reason), per the
// zetta-core rule. #849 (talk to an agent), #555 (the base agent it serves).
namespace zetta::agent {

struct AgentRecord {
    std::string id;             // uuid; the `agent:<id>` CRDT key
    std::string name;           // display name (required, non-empty)
    std::string description;    // optional, human-readable
    std::string model;          // required: checkpoint dir, `model:<uuid>`, or HF repo id
    std::string system_prompt;  // optional, prepended to the conversation
    std::vector<std::string> tools;  // tool names the agent may call (the sandbox subset)
    // What this agent is trained to be good at, and how much each matters (goal.hpp).
    //
    // The reward RLVR optimizes comes from HERE, not from code: a regulated lender weights "state
    // nothing we cannot evidence" highest, an ops team weights "nothing acts without approval",
    // and both are the same engine with different records. Empty = the built-in rubric, which is
    // what every agent used before goals existed.
    std::vector<Goal> goals;
    std::string created_at;     // ISO-8601, set by the caller (no clock in the core)
    std::string updated_at;

    // Empty when the record is well-formed enough to serve; else the reason it is not
    // (name + model are the load-bearing fields).
    std::string validation_error() const;
};

// The draft-07 boundary schema for an agent record (schema-as-code source of truth for
// the REST/OpenAPI surface).
const nlohmann::json& agent_schema();

nlohmann::json to_json(const AgentRecord& agent);

// Parse + validate; std::nullopt (+ *error, a path-qualified reason) on malformed input.
std::optional<AgentRecord> agent_from_json(const nlohmann::json& value, std::string* error);

}  // namespace zetta::agent
