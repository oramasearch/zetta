#include "zetta/agent/agent_record.hpp"

#include <string>

namespace zetta::agent {

std::string AgentRecord::validation_error() const {
    if (name.empty()) return "agent.name must be a non-empty string";
    if (model.empty()) return "agent.model must be a non-empty string";
    return std::string();
}

const nlohmann::json& agent_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSON({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "$id": "https://oramasearch.dev/schemas/agent.schema.json",
      "title": "Agent",
      "description": "A served, named agent (zetta#849): a model + system prompt + tools a user can list, select, and chat with.",
      "type": "object",
      "additionalProperties": false,
      "required": ["id", "name", "model"],
      "properties": {
        "id": { "type": "string", "minLength": 1 },
        "name": { "type": "string", "minLength": 1 },
        "description": { "type": "string" },
        "model": { "type": "string", "minLength": 1, "description": "checkpoint dir, model:<uuid>, or a HuggingFace repo id" },
        "system_prompt": { "type": "string" },
        "tools": { "type": "array", "items": { "type": "string" } },
        "training": { "type": "object", "description": "how and when this agent retrains" },
        "versions": { "type": "array", "items": { "type": "object" },
                      "description": "every checkpoint this agent has produced, oldest first" },
        "goals": { "type": "array", "items": { "type": "object" },
                   "description": "what this agent is trained to be good at, and how much each matters; the RLVR reward is derived from these" },
        "created_at": { "type": "string" },
        "updated_at": { "type": "string" }
      }
    })JSON");
    return schema;
}

nlohmann::json to_json(const AgentRecord& agent) {
    nlohmann::json value;
    value["id"] = agent.id;
    value["name"] = agent.name;
    value["description"] = agent.description;
    value["model"] = agent.model;
    value["system_prompt"] = agent.system_prompt;
    value["tools"] = agent.tools;
    nlohmann::json goals = nlohmann::json::array();
    for (const Goal& goal : agent.goals) goals.push_back(to_json(goal));
    value["goals"] = std::move(goals);
    value["training"] = to_json(agent.training);
    nlohmann::json versions = nlohmann::json::array();
    for (const AgentVersion& version : agent.versions) versions.push_back(to_json(version));
    value["versions"] = std::move(versions);
    value["created_at"] = agent.created_at;
    value["updated_at"] = agent.updated_at;
    return value;
}

namespace {

// A string field, or empty if absent/not-a-string (validation catches required ones).
std::string string_field(const nlohmann::json& value, const char* key) {
    const auto found = value.find(key);
    if (found != value.end() && found->is_string()) return found->get<std::string>();
    return std::string();
}

}  // namespace

std::optional<AgentRecord> agent_from_json(const nlohmann::json& value, std::string* error) {
    const auto fail = [error](const std::string& reason) -> std::optional<AgentRecord> {
        if (error != nullptr) *error = reason;
        return std::nullopt;
    };
    if (!value.is_object()) return fail("agent must be a JSON object");

    AgentRecord agent;
    agent.id = string_field(value, "id");
    agent.name = string_field(value, "name");
    agent.description = string_field(value, "description");
    agent.model = string_field(value, "model");
    agent.system_prompt = string_field(value, "system_prompt");
    agent.created_at = string_field(value, "created_at");
    agent.updated_at = string_field(value, "updated_at");

    const auto tools = value.find("tools");
    if (tools != value.end()) {
        if (!tools->is_array()) return fail("agent.tools must be an array of strings");
        for (const nlohmann::json& tool : *tools) {
            if (!tool.is_string()) return fail("agent.tools entries must be strings");
            agent.tools.push_back(tool.get<std::string>());
        }
    }

    if (const auto goals = value.find("goals"); goals != value.end()) {
        if (!goals->is_array()) return fail("agent.goals must be an array");
        for (const nlohmann::json& entry : *goals) {
            std::string reason;
            std::optional<Goal> goal = goal_from_json(entry, &reason);
            if (!goal) return fail("agent.goals: " + reason);
            agent.goals.push_back(*std::move(goal));
        }
        // Resolve plain-language goals to their checks on the way in, so a stored record always
        // says what it is actually training on rather than leaving it to be inferred later.
        interpret_goals(agent.goals);
    }

    if (const auto training = value.find("training"); training != value.end()) {
        std::string reason;
        std::optional<TrainingSpec> spec = training_spec_from_json(*training, &reason);
        if (!spec) return fail("agent.training: " + reason);
        agent.training = *std::move(spec);
    }
    if (const auto versions = value.find("versions"); versions != value.end()) {
        if (!versions->is_array()) return fail("agent.versions must be an array");
        for (const nlohmann::json& entry : *versions) {
            std::string reason;
            std::optional<AgentVersion> version = agent_version_from_json(entry, &reason);
            if (!version) return fail("agent.versions: " + reason);
            agent.versions.push_back(*std::move(version));
        }
    }

    if (agent.id.empty()) return fail("agent.id must be a non-empty string");
    if (const std::string reason = agent.validation_error(); !reason.empty()) {
        return fail(reason);
    }
    return agent;
}

}  // namespace zetta::agent
