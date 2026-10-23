#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/goal.hpp"
#include "zetta/agent/training_policy.hpp"

// A served, named AGENT: the first-class entity a user lists, selects, and chats with.
// It binds a MODEL (an operator-registered checkpoint selector; see checkpoint.hpp, #1411)
// to an optional SYSTEM PROMPT and the TOOLS it may call, so the
// same engine can host many specialized agents (the base agent, a customer's churn fork,
// ...). Stored as a replicated `agent:<uuid>` entity (catalog.hpp); the CRUD surface, the
// MCP/REST endpoints, and the GUI/web chat panels all read this record.
//
// Parsing is a non-throwing boundary (std::optional + a path-qualified reason), per the
// zetta-core rule. #849 (talk to an agent), #555 (the base agent it serves).
namespace zetta::agent {

// Where an agent's work area reaches (the Studio agent shape): the whole workspace, one
// project, or the named datasets. Every tool call the agent makes is narrowed to it.
struct WorkArea {
    std::string kind = "workspace";  // workspace | project | dataset
    std::string detail;              // the project name, or the dataset names
};

// An approved Pipeline granted to an agent as a tool (lake#238). `version` pins the approval it
// was granted at (the graph's approved_at): a graph changed and re-approved since is a different
// version, and the agent keeps the one it was granted until someone moves the grant.
struct PipelineGrant {
    std::string project;
    std::string graph;
    std::string version;
};

// One file of a Python agent's code, stored inline on the record.
struct DefinitionFile {
    std::string path;
    std::string contents;
};

// What `agent-runtime` loads for a Python agent (zetta#1015): its instructions and code, either
// inline here (`source: zetta`) or in a lake repository at a pinned revision (`source: lake`).
struct AgentDefinition {
    std::string source;        // zetta | lake
    std::string instructions;  // markdown; the manifesto and goal expand into it
    std::vector<DefinitionFile> files;
    std::string repository_id;  // source: lake
    std::string revision;       // source: lake; never a moving branch
    std::string entrypoint;     // module:function the runtime calls
};

// Inline definition files replicate to every node, so their total is capped. Larger code belongs
// in a lake repository.
inline constexpr std::size_t kMaxInlineDefinitionBytes = 1024 * 1024;

struct AgentRecord {
    std::string id;             // uuid; the `agent:<id>` CRDT key
    std::string name;           // display name (required, non-empty)
    std::string description;    // optional, human-readable
    std::string model;          // native: exact operator-registered checkpoint selector
    std::string system_prompt;  // optional, prepended to the conversation
    std::vector<std::string> tools;  // tool names the agent may call (the sandbox subset)
    // What this agent is trained to be good at, and how much each matters (goal.hpp).
    //
    // The reward RLVR optimizes comes from HERE, not from code: a regulated lender weights "state
    // nothing we cannot evidence" highest, an ops team weights "nothing acts without approval",
    // and both are the same engine with different records. Empty = the built-in rubric, which is
    // what every agent used before goals existed.
    std::vector<Goal> goals;
    // How and when this agent retrains, and every checkpoint it has produced (training_policy.hpp).
    //
    // Versions are kept rather than replaced because retraining is the operation most likely to
    // make an agent worse: a corpus change lands, the new checkpoint regresses on the thing the
    // customer cared about, and the only acceptable answer is pointing back at the one that worked.
    TrainingSpec training;
    std::vector<AgentVersion> versions;  // oldest first; the last is what `model` should point at
    // Which family the agent belongs to and where it runs (zetta#1015, the lake Agents epic).
    // `orama` agents are the specialized ones Orama ships (the Agent Library); `custom` agents
    // are built by people. `native` runs in this engine (chat, agent_run); `python` runs on
    // agent-runtime from `definition`; `service` is a specialized agent with its own image,
    // reached at `endpoint`.
    std::string kind = "custom";   // custom | orama
    std::string runtime = "native";  // native | python | service
    // The Studio agent shape.
    std::string purpose;
    WorkArea work_area;
    std::string manifesto;
    std::string goal;
    std::vector<std::string> skills;  // one of agent_skills()
    std::string trigger;              // empty = conversation only
    std::string status = "idle";      // idle | running | waiting | completed | failed
    std::string model_name;           // shown to people; `model` is what loads
    std::string model_version;
    // The Ask Orama contract: whether it takes a conversation, and whether it is the default.
    bool chat_capable = true;
    bool is_default = false;
    std::vector<PipelineGrant> pipelines;
    AgentDefinition definition;  // runtime: python
    std::string endpoint;        // runtime: service
    std::string created_at;     // ISO-8601, set by the caller (no clock in the core)
    std::string updated_at;

    // Empty when the record is well-formed enough to serve; else the reason it is not.
    // A native agent needs a model; a python agent a definition; a service agent an endpoint.
    std::string validation_error() const;

    bool native() const { return runtime == "native"; }
};

// The draft-07 boundary schema for an agent record (schema-as-code source of truth for
// the REST/OpenAPI surface).
const nlohmann::json& agent_schema();

// The skills a person may grant an agent, in the order Studio and lake offer them. agent-runtime
// gives an agent the tools each one names (oramasearch/agent-runtime `skills.py`): the original
// five reach the workspace engine, the rest reach lake's drives, transcripts, repositories,
// datasets, streams, pipeline design and usage (agent-runtime#7).
const std::vector<std::string>& agent_skills();

// The pattern of a skill that grants one of the workspace's MCP connections in lake (lake#652):
// `mcp:` then the connection's prefix as lake makes it, a letter then letters, digits and
// underscores, at most 24.
inline constexpr const char* kMcpConnectionSkillPattern = "^mcp:[a-z][a-z0-9_]{0,23}$";

// The JSON Schema of one entry of `skills`, for every capability input that takes them: one of
// agent_skills(), or an MCP connection's skill.
nlohmann::json agent_skill_schema();

nlohmann::json to_json(const AgentRecord& agent);

// Overwrite the fields added by zetta#1015 (kind, runtime, the Studio shape, pipelines, the Python
// definition, the service endpoint) with the ones present in `value`. Returns the reason on a
// malformed field. Shared by the record parser and the create/update capabilities.
std::optional<std::string> apply_agent_fields(const nlohmann::json& value, AgentRecord& agent);

// Parse + validate; std::nullopt (+ *error, a path-qualified reason) on malformed input.
std::optional<AgentRecord> agent_from_json(const nlohmann::json& value, std::string* error);

}  // namespace zetta::agent
