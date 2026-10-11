#include "zetta/agent/agent_record.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace zetta::agent {

namespace {

template <std::size_t Size>
bool one_of(const std::array<std::string_view, Size>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

constexpr std::array<std::string_view, 2> kKinds{"custom", "orama"};
constexpr std::array<std::string_view, 3> kRuntimes{"native", "python", "service"};
constexpr std::array<std::string_view, 3> kWorkAreas{"workspace", "project", "dataset"};
constexpr std::array<std::string_view, 5> kSkills{"search", "generate-data", "train", "summarize",
                                                  "call-api"};
constexpr std::array<std::string_view, 5> kStatuses{"idle", "running", "waiting", "completed",
                                                    "failed"};

// `mcp:<prefix>` grants the allowed tools of one of the workspace's MCP connections (lake#652): the
// prefix Lake gives a connection, a letter then letters, digits and underscores, at most 24. Lake
// checks the skill on every call of a connected tool; the engine only keeps it.
bool mcp_connection_skill(std::string_view skill) {
    constexpr std::string_view prefix = "mcp:";
    if (!skill.starts_with(prefix)) return false;
    const std::string_view name = skill.substr(prefix.size());
    if (name.empty() || name.size() > 24 || name.front() < 'a' || name.front() > 'z') return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

std::string definition_error(const AgentDefinition& definition) {
    if (definition.source == "zetta") {
        if (definition.instructions.empty()) {
            return "agent.definition.instructions must be a non-empty string";
        }
        std::size_t total = definition.instructions.size();
        for (const DefinitionFile& file : definition.files) {
            if (file.path.empty() || file.path.front() == '/' ||
                file.path.find("..") != std::string::npos) {
                return "agent.definition.files: '" + file.path +
                       "' must be a relative path inside the definition";
            }
            total += file.contents.size();
        }
        if (total > kMaxInlineDefinitionBytes) {
            return "agent.definition is larger than 1 MiB; store the code in a lake repository "
                   "(source: lake) instead";
        }
        return std::string();
    }
    if (definition.source == "lake") {
        if (definition.repository_id.empty()) {
            return "agent.definition.repository_id is required for source: lake";
        }
        if (definition.revision.empty()) {
            return "agent.definition.revision is required for source: lake (a pinned revision, "
                   "never a branch)";
        }
        return std::string();
    }
    return "agent.definition.source must be 'zetta' or 'lake'";
}

}  // namespace

std::string AgentRecord::validation_error() const {
    if (name.empty()) return "agent.name must be a non-empty string";
    if (!one_of(kKinds, kind)) return "agent.kind must be 'custom' or 'orama'";
    if (!one_of(kRuntimes, runtime)) {
        return "agent.runtime must be 'native', 'python' or 'service'";
    }
    if (runtime == "native" && model.empty()) return "agent.model must be a non-empty string";
    if (runtime == "python") {
        if (const std::string reason = definition_error(definition); !reason.empty()) {
            return reason;
        }
    }
    if (runtime == "service" && endpoint.empty()) {
        return "agent.endpoint is required for runtime: service";
    }
    if (!one_of(kWorkAreas, work_area.kind)) {
        return "agent.work_area.kind must be 'workspace', 'project' or 'dataset'";
    }
    for (const std::string& skill : skills) {
        if (!one_of(kSkills, skill) && !mcp_connection_skill(skill)) {
            return "agent.skills: '" + skill +
                   "' is not a skill (search, generate-data, train, summarize, call-api, or "
                   "mcp:<connection prefix>)";
        }
    }
    if (!one_of(kStatuses, status)) {
        return "agent.status must be idle, running, waiting, completed or failed";
    }
    for (const PipelineGrant& grant : pipelines) {
        if (grant.project.empty() || grant.graph.empty()) {
            return "agent.pipelines entries need a project and a graph";
        }
    }
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
      "required": ["id", "name"],
      "properties": {
        "id": { "type": "string", "minLength": 1 },
        "name": { "type": "string", "minLength": 1 },
        "description": { "type": "string" },
        "model": { "type": "string", "description": "native agents: checkpoint dir, model:<uuid>, or a HuggingFace repo id" },
        "kind": { "type": "string", "enum": ["custom", "orama"] },
        "runtime": { "type": "string", "enum": ["native", "python", "service"] },
        "purpose": { "type": "string" },
        "work_area": { "type": "object" },
        "manifesto": { "type": "string" },
        "goal": { "type": "string" },
        "skills": { "type": "array", "items": { "type": "string" } },
        "trigger": { "type": "string" },
        "status": { "type": "string" },
        "model_name": { "type": "string" },
        "model_version": { "type": "string" },
        "chat_capable": { "type": "boolean" },
        "is_default": { "type": "boolean" },
        "pipelines": { "type": "array", "items": { "type": "object" } },
        "definition": { "type": "object" },
        "endpoint": { "type": "string" },
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
    value["kind"] = agent.kind;
    value["runtime"] = agent.runtime;
    value["purpose"] = agent.purpose;
    value["work_area"] = {{"kind", agent.work_area.kind}, {"detail", agent.work_area.detail}};
    value["manifesto"] = agent.manifesto;
    value["goal"] = agent.goal;
    value["skills"] = agent.skills;
    value["trigger"] = agent.trigger;
    value["status"] = agent.status;
    value["model_name"] = agent.model_name;
    value["model_version"] = agent.model_version;
    value["chat_capable"] = agent.chat_capable;
    value["is_default"] = agent.is_default;
    nlohmann::json pipelines = nlohmann::json::array();
    for (const PipelineGrant& grant : agent.pipelines) {
        pipelines.push_back(
            {{"project", grant.project}, {"graph", grant.graph}, {"version", grant.version}});
    }
    value["pipelines"] = std::move(pipelines);
    if (agent.runtime == "python") {
        nlohmann::json definition = {{"source", agent.definition.source},
                                     {"entrypoint", agent.definition.entrypoint}};
        if (agent.definition.source == "lake") {
            definition["repository_id"] = agent.definition.repository_id;
            definition["revision"] = agent.definition.revision;
        } else {
            definition["instructions"] = agent.definition.instructions;
            nlohmann::json files = nlohmann::json::array();
            for (const DefinitionFile& file : agent.definition.files) {
                files.push_back({{"path", file.path}, {"contents", file.contents}});
            }
            definition["files"] = std::move(files);
        }
        value["definition"] = std::move(definition);
    }
    value["endpoint"] = agent.endpoint;
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

bool bool_field(const nlohmann::json& value, const char* key, bool fallback) {
    const auto found = value.find(key);
    if (found != value.end() && found->is_boolean()) return found->get<bool>();
    return fallback;
}

}  // namespace

std::optional<std::string> apply_agent_fields(const nlohmann::json& value, AgentRecord& agent) {
    const auto text = [&value](const char* key, std::string& field) {
        if (value.contains(key) && value.at(key).is_string()) {
            field = value.at(key).get<std::string>();
        }
    };
    text("kind", agent.kind);
    text("runtime", agent.runtime);
    text("purpose", agent.purpose);
    text("manifesto", agent.manifesto);
    text("goal", agent.goal);
    text("trigger", agent.trigger);
    text("status", agent.status);
    text("model_name", agent.model_name);
    text("model_version", agent.model_version);
    text("endpoint", agent.endpoint);
    agent.chat_capable = bool_field(value, "chat_capable", agent.chat_capable);
    agent.is_default = bool_field(value, "is_default", agent.is_default);
    if (const auto area = value.find("work_area"); area != value.end()) {
        if (!area->is_object()) return std::string("agent.work_area must be an object");
        agent.work_area.kind = string_field(*area, "kind");
        if (agent.work_area.kind.empty()) agent.work_area.kind = "workspace";
        agent.work_area.detail = string_field(*area, "detail");
    }
    if (const auto skills = value.find("skills"); skills != value.end()) {
        if (!skills->is_array()) return std::string("agent.skills must be an array of strings");
        agent.skills.clear();
        for (const nlohmann::json& skill : *skills) {
            if (!skill.is_string()) return std::string("agent.skills entries must be strings");
            agent.skills.push_back(skill.get<std::string>());
        }
    }
    if (const auto pipelines = value.find("pipelines"); pipelines != value.end()) {
        if (!pipelines->is_array()) return std::string("agent.pipelines must be an array");
        agent.pipelines.clear();
        for (const nlohmann::json& entry : *pipelines) {
            if (!entry.is_object()) return std::string("agent.pipelines entries must be objects");
            agent.pipelines.push_back(PipelineGrant{string_field(entry, "project"),
                                                    string_field(entry, "graph"),
                                                    string_field(entry, "version")});
        }
    }
    if (const auto definition = value.find("definition"); definition != value.end()) {
        if (!definition->is_object()) return std::string("agent.definition must be an object");
        AgentDefinition parsed;
        parsed.source = string_field(*definition, "source");
        parsed.instructions = string_field(*definition, "instructions");
        parsed.repository_id = string_field(*definition, "repository_id");
        parsed.revision = string_field(*definition, "revision");
        parsed.entrypoint = string_field(*definition, "entrypoint");
        if (const auto files = definition->find("files"); files != definition->end()) {
            if (!files->is_array()) return std::string("agent.definition.files must be an array");
            for (const nlohmann::json& file : *files) {
                if (!file.is_object()) {
                    return std::string("agent.definition.files entries must be objects");
                }
                parsed.files.push_back(
                    DefinitionFile{string_field(file, "path"), string_field(file, "contents")});
            }
        }
        agent.definition = std::move(parsed);
    }
    return std::nullopt;
}

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

    if (std::optional<std::string> reason = apply_agent_fields(value, agent)) {
        return fail(*reason);
    }

    if (agent.id.empty()) return fail("agent.id must be a non-empty string");
    if (const std::string reason = agent.validation_error(); !reason.empty()) {
        return fail(reason);
    }
    return agent;
}

}  // namespace zetta::agent
