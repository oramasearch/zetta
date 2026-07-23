#include "zetta/agent/action.hpp"

#include <utility>

#include "zetta/ir/serialization.hpp"

namespace zetta::agent {

nlohmann::json to_json(const StudioAction& action) {
    nlohmann::json tool_calls = nlohmann::json::array();
    for (const ToolCall& call : action.tool_calls) {
        tool_calls.push_back({{"tool", call.tool}, {"arguments", call.arguments}});
    }
    return nlohmann::json{{"message", action.message},
                          {"tool_calls", std::move(tool_calls)},
                          {"execution_graph", zetta::ir::to_json(action.execution_graph)}};
}

std::optional<StudioAction> action_from_json(const nlohmann::json& document,
                                             std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<StudioAction> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!document.is_object()) return fail("action: not a JSON object");

    StudioAction action;

    if (const auto message = document.find("message"); message != document.end()) {
        if (!message->is_string()) return fail("action.message: must be a string");
        action.message = message->get<std::string>();
    }  // message is optional (a graph-only action carries no user text)

    if (const auto calls = document.find("tool_calls"); calls != document.end()) {
        if (!calls->is_array()) return fail("action.tool_calls: must be an array");
        for (std::size_t index = 0; index < calls->size(); ++index) {
            const nlohmann::json& entry = (*calls)[index];
            if (!entry.is_object() || !entry.contains("tool") || !entry.at("tool").is_string()) {
                return fail("action.tool_calls[" + std::to_string(index) +
                            "]: needs a string `tool`");
            }
            nlohmann::json arguments = nlohmann::json::object();
            if (const auto args = entry.find("arguments"); args != entry.end()) {
                if (!args->is_object()) {
                    return fail("action.tool_calls[" + std::to_string(index) +
                                "].arguments: must be an object");
                }
                arguments = *args;
            }
            action.tool_calls.emplace_back(entry.at("tool").get<std::string>(),
                                           std::move(arguments));
        }
    }

    if (const auto graph = document.find("execution_graph"); graph != document.end()) {
        std::string reason;
        std::optional<zetta::ir::ExecutionGraph> parsed =
            zetta::ir::from_json(*graph, &reason);
        if (!parsed) return fail("action.execution_graph: " + reason);
        action.execution_graph = *std::move(parsed);
    }  // graph is optional (a message-only turn — e.g. a clarifying question — carries none)

    return action;
}

const nlohmann::json& studio_action_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json tool_item;
        tool_item["type"] = "object";
        tool_item["required"] = nlohmann::json::array({"tool"});
        tool_item["properties"]["tool"] = {{"type", "string"}, {"minLength", 1}};
        tool_item["properties"]["arguments"] = {{"type", "object"}};

        nlohmann::json graph;
        graph["type"] = "object";
        graph["description"] = "the programmatic pipeline (the ir::ExecutionGraph — nodes + edges)";
        graph["properties"]["nodes"] = {{"type", "array"}};
        graph["properties"]["edges"] = {{"type", "array"}};

        nlohmann::json document;
        document["type"] = "object";
        document["additionalProperties"] = false;
        document["properties"]["message"] = {
            {"type", "string"},
            {"description", "what the agent shows the user (plain language, no jargon)"}};
        document["properties"]["tool_calls"] = {
            {"type", "array"},
            {"description", "pipeline-setup actions; each may call other models"},
            {"items", tool_item}};
        document["properties"]["execution_graph"] = graph;
        return document;
    }();
    return schema;
}

}  // namespace zetta::agent
