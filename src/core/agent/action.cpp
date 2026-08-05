#include "zetta/agent/action.hpp"

#include <utility>

#include "zetta/ir/serialization.hpp"

namespace zetta::agent {

nlohmann::json to_json(const StudioAction& action) {
    nlohmann::json tool_calls = nlohmann::json::array();
    for (const ToolCall& call : action.tool_calls) {
        tool_calls.push_back({{"tool", call.tool}, {"arguments", call.arguments}});
    }
    return nlohmann::json{{"kind", to_string(action.kind)},
                          {"message", action.message},
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

    // The declared mode (#918). Optional and defaulting to Action, so every pre-#918 document
    // parses exactly as it did before; an unknown value is a boundary error, not a silent Action
    // — the harness switches on this, so guessing here would defeat the point of declaring it.
    if (const auto kind = document.find("kind"); kind != document.end()) {
        if (!kind->is_string()) return fail("action.kind: must be a string");
        const std::optional<InteractionKind> parsed =
            interaction_kind_from_string(kind->get<std::string>());
        if (!parsed) {
            return fail("action.kind: unknown mode '" + kind->get<std::string>() +
                        "' (expected action, answer, clarify, or abstain)");
        }
        action.kind = *parsed;
    }

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

std::optional<StudioAction> parse_studio_action(const std::string& generation) {
    // The action is the JSON object the model emitted; take the first `{` to the last `}` (the
    // model is trained to emit one object). A balanced scan isn't needed for a single object,
    // and non-JSON preamble/suffix is tolerated.
    const std::size_t begin = generation.find('{');
    const std::size_t end = generation.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || begin >= end) {
        return std::nullopt;
    }
    const nlohmann::json document = nlohmann::json::parse(
        generation.substr(begin, end - begin + 1), nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) return std::nullopt;
    return action_from_json(document);
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
        document["properties"]["kind"] = {
            {"type", "string"},
            {"enum", nlohmann::json::array({"action", "answer", "clarify", "abstain"})},
            {"description",
             "the mode this response declares: action builds or changes a Pipeline; answer "
             "answers from the supplied context; clarify asks one question back; abstain "
             "declines and builds nothing"}};
        document["properties"]["message"] = {
            {"type", "string"},
            {"description", "what the agent shows the user (markdown, plain language, no jargon)"}};
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
