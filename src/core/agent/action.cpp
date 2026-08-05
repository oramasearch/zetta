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

// The first BALANCED JSON object in `text` — depth-counted and string-aware, so a quote or brace
// inside a message never ends the scan early. Empty when there is none.
//
// Spanning first-`{` to last-`}` instead was wrong in the one case that matters: a model that does
// not emit a stop token keeps going and writes a SECOND action after the first. That span then
// covers `{...}{...}`, which is not valid JSON, so a perfectly good first action was thrown away
// and the turn reported as unparsed. Taking the first complete object also makes trailing junk of
// any kind — a repeated action, a truncated tail, stray prose — harmless.
std::string first_balanced_object(const std::string& text) {
    const std::size_t begin = text.find('{');
    if (begin == std::string::npos) return {};
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = begin; index < text.size(); ++index) {
        const char character = text[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}') {
            if (--depth == 0) return text.substr(begin, index - begin + 1);
        }
    }
    return {};  // unterminated — a truncated generation
}

std::optional<StudioAction> parse_studio_action(const std::string& generation) {
    const std::string object = first_balanced_object(generation);
    if (object.empty()) return std::nullopt;
    nlohmann::json document = nlohmann::json::parse(object, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) return std::nullopt;
    if (std::optional<StudioAction> action = action_from_json(document)) return action;
    // Retry without the graph. The harness builds the Pipeline itself by running the model's tool
    // calls, so the model's own graph is advisory — and a checkpoint that writes colliding node
    // ids would otherwise take the whole action down with it, discarding tool calls that were
    // perfectly good. Dropping a graph we were never going to use beats losing the turn.
    // `action_from_json` stays strict: it is a boundary validator, and a bad graph is a real
    // error there.
    if (document.is_object() && document.contains("execution_graph")) {
        document.erase("execution_graph");
        return action_from_json(document);
    }
    return std::nullopt;
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
