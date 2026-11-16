// SPDX-License-Identifier: MIT

#include "zetta/agent/conversation.hpp"

#include <utility>

namespace zetta::agent {

std::string_view to_string(TurnAuthor author) {
    switch (author) {
        case TurnAuthor::Person: return "person";
        case TurnAuthor::Agent: return "agent";
        case TurnAuthor::Tool: return "tool";
    }
    return "person";
}

std::optional<TurnAuthor> turn_author_from_string(std::string_view name) {
    if (name == "person") return TurnAuthor::Person;
    if (name == "agent") return TurnAuthor::Agent;
    if (name == "tool") return TurnAuthor::Tool;
    return std::nullopt;
}

std::string Conversation::validation_error() const {
    if (id.empty()) return "conversation.id: required";
    for (const Turn& turn : turns) {
        if (turn.id.empty()) return "conversation.turns[]: every turn needs an id";
        if (turn.author_id.empty()) {
            return "conversation.turns[]: every turn needs an author_id — an unattributed turn "
                   "cannot be built into history, and in a multi-party thread it cannot even be "
                   "read";
        }
    }
    return {};
}

std::vector<PriorTurn> history_for(const Conversation& conversation, const std::string& for_agent,
                                   std::size_t limit) {
    // Pair each person turn with the reply THIS agent gave it. A tool turn folds into the agent
    // turn it belongs to, because a tool result is not something anyone said — it is what happened
    // while the agent was answering.
    std::vector<PriorTurn> history;
    std::optional<PriorTurn> pending;

    for (const Turn& turn : conversation.turns) {
        switch (turn.author) {
            case TurnAuthor::Person:
                // A new question closes whatever came before, even if nothing answered it — an
                // unanswered ask is still part of the exchange and dropping it would silently
                // rewrite what was said.
                if (pending) history.push_back(*std::move(pending));
                pending = PriorTurn{turn.text, {}, {}};
                break;

            case TurnAuthor::Agent:
                // Only THIS agent's replies. In a multi-agent thread, attributing a colleague's
                // answer to itself would teach it to imitate rather than answer — and to claim
                // work it did not do.
                if (!for_agent.empty() && turn.author_id != for_agent) break;
                if (!pending) pending = PriorTurn{};
                pending->reply = turn.text;
                for (const ToolOutcome& outcome : turn.tool_outcomes) {
                    pending->tools.push_back(outcome);
                }
                break;

            case TurnAuthor::Tool:
                if (!pending) pending = PriorTurn{};
                pending->tools.push_back({turn.author_id, turn.kind, turn.text});
                break;
        }
    }
    if (pending) history.push_back(*std::move(pending));

    // Keep the most RECENT `limit` exchanges: a long thread will not fit a context window, and a
    // follow-up refers to what was just said, not to the opening.
    if (limit > 0 && history.size() > limit) {
        history.erase(history.begin(),
                      history.begin() + static_cast<std::ptrdiff_t>(history.size() - limit));
    }
    return history;
}

nlohmann::json to_json(const Turn& turn) {
    nlohmann::json calls = nlohmann::json::array();
    for (const ToolCall& call : turn.tool_calls) {
        calls.push_back({{"tool", call.tool}, {"arguments", call.arguments}});
    }
    nlohmann::json outcomes = nlohmann::json::array();
    for (const ToolOutcome& outcome : turn.tool_outcomes) {
        outcomes.push_back({{"tool", outcome.tool},
                            {"status", outcome.status},
                            {"detail", outcome.detail}});
    }
    return nlohmann::json{{"id", turn.id},
                          {"author", std::string(to_string(turn.author))},
                          {"author_id", turn.author_id},
                          {"text", turn.text},
                          {"created_at", turn.created_at},
                          {"kind", turn.kind},
                          {"tool_calls", std::move(calls)},
                          {"tool_outcomes", std::move(outcomes)},
                          {"grounding", turn.grounding},
                          {"grounded", turn.grounded},
                          {"outcome", turn.outcome}};
}

nlohmann::json to_json(const Conversation& conversation) {
    nlohmann::json turns = nlohmann::json::array();
    for (const Turn& turn : conversation.turns) turns.push_back(to_json(turn));
    return nlohmann::json{{"id", conversation.id},
                          {"title", conversation.title},
                          {"participants", conversation.participants},
                          {"agents", conversation.agents},
                          {"turns", std::move(turns)},
                          {"created_at", conversation.created_at},
                          {"updated_at", conversation.updated_at}};
}

std::optional<Turn> turn_from_json(const nlohmann::json& value, std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<Turn> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("turn: not a JSON object");

    Turn turn;
    const auto text_of = [&value](const char* key) -> std::string {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>()
                                                          : std::string();
    };
    turn.id = text_of("id");
    if (turn.id.empty()) return fail("turn.id: required, must be a non-empty string");

    const auto author = value.find("author");
    if (author != value.end()) {
        if (!author->is_string()) return fail("turn.author: must be a string");
        const std::optional<TurnAuthor> parsed = turn_author_from_string(author->get<std::string>());
        if (!parsed) {
            return fail("turn.author: unknown value '" + author->get<std::string>() +
                        "' (person, agent, tool)");
        }
        turn.author = *parsed;
    }
    turn.author_id = text_of("author_id");
    if (turn.author_id.empty()) return fail("turn.author_id: required — a turn must say who said it");
    turn.text = text_of("text");
    turn.created_at = text_of("created_at");
    turn.kind = text_of("kind");
    turn.grounding = text_of("grounding");
    turn.outcome = text_of("outcome");
    if (const auto grounded = value.find("grounded"); grounded != value.end()) {
        if (!grounded->is_boolean()) return fail("turn.grounded: must be a boolean");
        turn.grounded = grounded->get<bool>();
    }
    if (const auto calls = value.find("tool_calls"); calls != value.end()) {
        if (!calls->is_array()) return fail("turn.tool_calls: must be an array");
        for (const nlohmann::json& call : *calls) {
            if (!call.is_object() || !call.contains("tool") || !call.at("tool").is_string()) {
                return fail("turn.tool_calls[]: needs a string `tool`");
            }
            turn.tool_calls.emplace_back(
                call.at("tool").get<std::string>(),
                call.contains("arguments") ? call.at("arguments") : nlohmann::json::object());
        }
    }
    if (const auto outcomes = value.find("tool_outcomes"); outcomes != value.end()) {
        if (!outcomes->is_array()) return fail("turn.tool_outcomes: must be an array");
        for (const nlohmann::json& outcome : *outcomes) {
            if (!outcome.is_object()) return fail("turn.tool_outcomes[]: must be objects");
            turn.tool_outcomes.push_back({outcome.value("tool", std::string()),
                                          outcome.value("status", std::string()),
                                          outcome.value("detail", std::string())});
        }
    }
    return turn;
}

std::optional<Conversation> conversation_from_json(const nlohmann::json& value,
                                                   std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<Conversation> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("conversation: not a JSON object");

    Conversation conversation;
    const auto text_of = [&value](const char* key) -> std::string {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>()
                                                          : std::string();
    };
    conversation.id = text_of("id");
    if (conversation.id.empty()) return fail("conversation.id: required");
    conversation.title = text_of("title");
    conversation.created_at = text_of("created_at");
    conversation.updated_at = text_of("updated_at");

    const auto strings = [&](const char* key, std::vector<std::string>& into) -> bool {
        const auto found = value.find(key);
        if (found == value.end()) return true;
        if (!found->is_array()) return false;
        for (const nlohmann::json& entry : *found) {
            if (entry.is_string()) into.push_back(entry.get<std::string>());
        }
        return true;
    };
    if (!strings("participants", conversation.participants)) {
        return fail("conversation.participants: must be an array of strings");
    }
    if (!strings("agents", conversation.agents)) {
        return fail("conversation.agents: must be an array of strings");
    }

    if (const auto turns = value.find("turns"); turns != value.end()) {
        if (!turns->is_array()) return fail("conversation.turns: must be an array");
        for (const nlohmann::json& entry : *turns) {
            std::string reason;
            std::optional<Turn> turn = turn_from_json(entry, &reason);
            if (!turn) return fail("conversation.turns: " + reason);
            conversation.turns.push_back(*std::move(turn));
        }
    }
    return conversation;
}

const nlohmann::json& conversation_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["required"] = nlohmann::json::array({"id"});
        document["properties"]["id"] = {{"type", "string"}, {"minLength", 1}};
        document["properties"]["title"] = {{"type", "string"}};
        document["properties"]["participants"] = {
            {"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "principals in this thread — a conversation is not limited to one"}};
        document["properties"]["agents"] = {{"type", "array"}, {"items", {{"type", "string"}}}};
        document["properties"]["turns"] = {{"type", "array"}, {"items", {{"type", "object"}}}};
        return document;
    }();
    return schema;
}

}  // namespace zetta::agent
