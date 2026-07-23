#include "zetta/agent/chat.hpp"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/tokenizer/bpe.hpp"

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

ChatResponse respond(const AgentRecord& agent, const std::string& message,
                     int max_new_tokens, int max_rounds) {
    ChatResponse response;
    std::string error;
    const std::optional<zetta::tokenizer::BpeTokenizer> tokenizer =
        zetta::tokenizer::BpeTokenizer::from_directory(agent.model, &error);
    if (!tokenizer) {
        response.error =
            "could not load the tokenizer from '" + agent.model + "': " + error;
        return response;
    }
    const std::optional<zetta::autograd::QwenModel> model =
        zetta::autograd::load_qwen_model(agent.model, &error);
    if (!model) {
        response.error = "could not load the model from '" + agent.model + "': " + error;
        return response;
    }

    // The agent's tools, wired into a sandbox. Real graph-building effects are #859; for now
    // each records that it ran so the tool-calling loop closes end to end.
    Sandbox sandbox;
    for (const std::string& tool : agent.tools) {
        sandbox.register_tool(tool, [tool](EndState& world, const nlohmann::json&) {
            world.set("ran_" + tool, true);
            return ToolResult::success();
        });
    }

    // The model's EOS id, so generation STOPS when an EOS-trained model emits it (a model
    // trained without a stop token repeats — see the SFT driver). Read from config.json.
    int eos_id = 151645;  // fallback: Qwen <|im_end|>
    {
        std::ifstream config_file(agent.model + "/config.json");
        if (config_file) {
            const nlohmann::json config =
                nlohmann::json::parse(config_file, nullptr, /*allow_exceptions=*/false);
            if (!config.is_discarded() && config.contains("eos_token_id") &&
                config["eos_token_id"].is_number_integer()) {
                eos_id = config["eos_token_id"].get<int>();
            }
        }
    }

    // A fresh KV-cached inference per generation (independent decode), returning new text.
    const auto generate = [&tokenizer, &model, max_new_tokens, eos_id](
                              const std::string& prompt) -> std::string {
        zetta::autograd::QwenInference inference(*model);
        return tokenizer->decode(
            inference.generate_greedy(tokenizer->encode(prompt), max_new_tokens, eos_id));
    };

    std::string conversation;
    if (!agent.system_prompt.empty()) conversation += agent.system_prompt + "\n\n";
    conversation += "User: " + message + "\nAssistant:";

    response.result = run_chat_turn(generate, sandbox, conversation, max_rounds);

    std::string& reply = response.result.reply;
    // Strip the special stop/turn markers the decoder emits as literal text.
    for (const std::string_view special : {"<|im_end|>", "<|endoftext|>", "<|im_start|>"}) {
        for (std::size_t pos = reply.find(special); pos != std::string::npos;
             pos = reply.find(special)) {
            reply.erase(pos, special.size());
        }
    }
    // A pre-EOS checkpoint (no trained stop token) repeats the "Correct solution (facts): {...}"
    // block; keep only the first. (An EOS-trained checkpoint stops on its own — this is a
    // no-op there.)
    constexpr std::string_view marker = "Correct solution (facts):";
    const std::size_t first = reply.find(marker);
    if (first != std::string::npos) {
        const std::size_t second = reply.find(marker, first + marker.size());
        if (second != std::string::npos) reply.erase(second);
    }
    const std::size_t last = reply.find_last_not_of(" \t\r\n");
    reply.erase(last == std::string::npos ? 0 : last + 1);

    response.ok = true;
    return response;
}

}  // namespace zetta::agent
