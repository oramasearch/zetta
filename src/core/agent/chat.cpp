#include "zetta/agent/chat.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::agent {
namespace {

constexpr std::string_view kOpen = "<tool_call>";
constexpr std::string_view kClose = "</tool_call>";

std::optional<ToolCall> tool_call_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    std::string name;
    if (value.contains("tool") && value.at("tool").is_string()) {
        name = value.at("tool").get<std::string>();
    } else if (value.contains("name") && value.at("name").is_string()) {
        name = value.at("name").get<std::string>();
    }
    if (name.empty()) return std::nullopt;
    nlohmann::json arguments = nlohmann::json::object();
    if (value.contains("arguments") && value.at("arguments").is_object()) {
        arguments = value.at("arguments");
    } else if (value.contains("args") && value.at("args").is_object()) {
        arguments = value.at("args");
    }
    return ToolCall{name, std::move(arguments)};
}

std::string apply_status_word(ApplyStatus status) {
    switch (status) {
        case ApplyStatus::Ok:
            return "ok";
        case ApplyStatus::UnknownTool:
            return "unknown_tool";
        case ApplyStatus::ToolFailed:
            return "failed";
    }
    return "unknown";
}

}  // namespace

std::vector<ToolCall> parse_tool_calls(const std::string& text) {
    std::vector<ToolCall> calls;
    std::size_t position = 0;
    bool found_block = false;
    for (;;) {
        const std::size_t begin = text.find(kOpen, position);
        if (begin == std::string::npos) break;
        const std::size_t inner_begin = begin + kOpen.size();
        const std::size_t end = text.find(kClose, inner_begin);
        if (end == std::string::npos) break;
        found_block = true;
        const nlohmann::json value = nlohmann::json::parse(
            text.substr(inner_begin, end - inner_begin), nullptr, /*allow_exceptions=*/false);
        if (!value.is_discarded()) {
            if (std::optional<ToolCall> call = tool_call_from_json(value)) {
                calls.push_back(*std::move(call));
            }
        }
        position = end + kClose.size();
    }
    if (found_block) return calls;

    // No explicit blocks: try a single embedded JSON object with a tool key.
    const std::size_t begin = text.find('{');
    const std::size_t end = text.rfind('}');
    if (begin != std::string::npos && end != std::string::npos && begin < end) {
        const nlohmann::json value = nlohmann::json::parse(
            text.substr(begin, end - begin + 1), nullptr, /*allow_exceptions=*/false);
        if (!value.is_discarded()) {
            if (std::optional<ToolCall> call = tool_call_from_json(value)) {
                calls.push_back(*std::move(call));
            }
        }
    }
    return calls;
}

ChatResult run_chat_turn(const GenerateFn& generate, Sandbox& sandbox,
                         std::string conversation, int max_rounds) {
    ChatResult result;
    for (int round = 0; round < max_rounds; ++round) {
        const std::string completion = generate(conversation);
        result.rounds = round + 1;

        const std::vector<ToolCall> calls = parse_tool_calls(completion);
        if (calls.empty()) {  // the model answered
            result.reply = completion;
            return result;
        }

        // Run each call, feeding its outcome back into the conversation as a tool result.
        conversation += "\n" + completion + "\n";
        for (const ToolCall& call : calls) {
            const ApplyOutcome outcome = sandbox.apply(call);
            result.invocations.push_back({call, outcome.status, outcome.reason});
            nlohmann::json feedback;
            feedback["tool"] = call.tool;
            feedback["status"] = apply_status_word(outcome.status);
            if (!outcome.reason.empty()) feedback["detail"] = outcome.reason;
            conversation += "<tool_result>" + feedback.dump() + "</tool_result>\n";
        }
    }
    // Round cap reached with calls still pending: one last generation is the best-effort reply.
    result.reply = generate(conversation);
    result.rounds += 1;
    return result;
}

}  // namespace zetta::agent
