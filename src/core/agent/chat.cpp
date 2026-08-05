#include "zetta/agent/chat.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/action.hpp"
#include "zetta/agent/base_prompt.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/interaction.hpp"
#include "zetta/agent/model_cache.hpp"
#include "zetta/agent/inference_runner.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/tokenizer/bpe.hpp"

namespace zetta::agent {
namespace {

constexpr std::string_view kOpen = "<tool_call>";
constexpr std::string_view kClose = "</tool_call>";

// A small process-wide tokenizer cache. Encode/decode need the tokenizer every
// turn, but it is cheap next to the model — loading it separately lets the fast MLX
// serving path skip loading the CPU autograd model entirely, avoiding a wasted second
// full-checkpoint load on the first turn.
std::shared_ptr<const zetta::tokenizer::BpeTokenizer> cached_tokenizer(
    const std::string& path, std::string* error) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const zetta::tokenizer::BpeTokenizer>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = cache.find(path);
    if (found == cache.end()) {
        std::optional<zetta::tokenizer::BpeTokenizer> loaded =
            zetta::tokenizer::BpeTokenizer::from_directory(path, error);
        if (!loaded) return nullptr;
        found = cache.emplace(path, std::make_shared<const zetta::tokenizer::BpeTokenizer>(
                                        std::move(*loaded)))
                    .first;
    }
    return found->second;
}

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
            // A read tool answers with a value; give it back so the next round can
            // reason over the node's real state instead of guessing (#904).
            if (!outcome.data.is_null()) feedback["result"] = outcome.data;
            conversation += "<tool_result>" + feedback.dump() + "</tool_result>\n";
        }
    }
    // Round cap reached with calls still pending: one last generation is the best-effort reply.
    result.reply = generate(conversation);
    result.rounds += 1;
    return result;
}

std::string build_chat_prompt(const AgentRecord& agent, const std::string& message,
                              const std::string& grounding,
                              const std::vector<AgentToolSpec>& tool_specs) {
    // The trained frame (#916/#918). `base_agent_system_prompt()` is baked into EVERY SFT example
    // by `render_action_example`, and `render_interaction_prompt` is the renderer they share — so
    // serving MUST frame the turn identically or the model is prompted off-distribution. It was
    // not: this path prepended the record's own prompt (nothing in the serving path referenced
    // the trained one at all) and appended "User:"/"Assistant:" turn labels that appear in ZERO
    // training examples. That skew is a sufficient explanation for the raw-JSON replies, the
    // ignored instructions, and the invented numbers, each of which had been getting patched as
    // a separate defect.
    const std::string_view system_prompt = agent.system_prompt.empty()
                                               ? base_agent_system_prompt()
                                               : std::string_view(agent.system_prompt);
    // Everything the model needs rides INSIDE the ask, within the frame — never as foreign
    // blocks wrapped around it. `grounding` is placed by the shared renderer itself.
    std::string ask;
    // Name the live tools and the call format, so the model can ask the node about its real
    // state instead of guessing (#904). Omitted when none are wired, so a plain chat keeps the
    // exact prompt it trained on.
    if (!tool_specs.empty()) {
        ask += "You can call these tools to read this node's real state:\n";
        for (const AgentToolSpec& spec : tool_specs) {
            ask += "- " + spec.name + ": " + spec.description + "\n";
        }
        ask +=
            "To call one, emit <tool_call>{\"tool\": \"<name>\", \"arguments\": {…}}</tool_call> "
            "and stop. The result comes back as <tool_result>; use it to answer.\n\n";
    }
    ask += message;
    return render_interaction_prompt(system_prompt, grounding, ask);
}

ChatResponse respond(const AgentRecord& agent, const std::string& message,
                     int max_new_tokens, int max_rounds,
                     const AgentToolRunner& tools,
                     const std::vector<AgentToolSpec>& tool_specs,
                     const std::string& grounding) {
    ChatResponse response;
    std::string error;
    // The tokenizer is cheap and always needed (encode/decode); load it from a small
    // process-wide cache. The multi-GB autograd model is NOT loaded here: when the fast
    // MLX serving runner is installed it decodes without the CPU model, so we load that
    // model lazily in `generate` below only as the fallback — avoiding a wasted second
    // full-checkpoint load on the first turn.
    const std::shared_ptr<const zetta::tokenizer::BpeTokenizer> tokenizer_handle =
        cached_tokenizer(agent.model, &error);
    if (!tokenizer_handle) {
        response.error = error;
        return response;
    }
    const zetta::tokenizer::BpeTokenizer& tokenizer = *tokenizer_handle;

    // The agent's tools, wired into a sandbox. Real graph-building effects are #859; for now
    // each records that it ran so the tool-calling loop closes end to end.
    Sandbox sandbox;
    for (const std::string& tool : agent.tools) {
        sandbox.register_tool(tool, [tool](EndState& world, const nlohmann::json&) {
            world.set("ran_" + tool, true);
            return ToolResult::success();
        });
    }
    // Live tools (#904): each runs through the embedding's runner, which the API
    // layer binds to the capability registry under the CALLER's identity. These
    // are registered after the recorded tools, so a live tool of the same name
    // wins — a real result beats a marker.
    if (tools) {
        for (const AgentToolSpec& spec : tool_specs) {
            const std::string name = spec.name;
            sandbox.register_tool(name, [name, &tools](EndState& world,
                                                       const nlohmann::json& arguments) {
                ToolResult result = tools(name, arguments);
                world.set("ran_" + name, true);
                return result;
            });
        }
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

    // Decode: prefer the installed fast Metal-GPU runner (MlxQwenInference, #555) when
    // the MLX build wired one in; fall back to the native autograd CPU inference. The
    // fast path is gated to the same logits, so it returns the same greedy tokens —
    // just ~6x faster on Apple Silicon (no per-op host<->device sync).
    const std::string model_path = agent.model;
    const auto generate = [&tokenizer, model_path, max_new_tokens, eos_id](
                              const std::string& prompt) -> std::string {
        const std::vector<int> prompt_tokens = tokenizer.encode(prompt);
        if (has_qwen_inference_runner()) {
            if (auto tokens = qwen_inference_runner()(model_path, prompt_tokens,
                                                      max_new_tokens, eos_id)) {
                return tokenizer.decode(*tokens);
            }
        }
        // Fallback: load the CPU autograd model lazily (only when MLX didn't serve).
        std::string load_error;
        const std::shared_ptr<const LoadedModel> loaded =
            ModelCache::instance().get(model_path, &load_error);
        if (!loaded) return std::string();
        return tokenizer.decode(zetta::autograd::QwenInference(loaded->model)
                                    .generate_greedy(prompt_tokens, max_new_tokens, eos_id));
    };

    // One builder, shared with the contract test — see build_chat_prompt above. Live tool specs
    // only enter the prompt when a runner is actually wired, so a plain chat keeps the exact
    // prompt it trained on.
    const std::string prompt =
        build_chat_prompt(agent, message, grounding,
                          tools ? tool_specs : std::vector<AgentToolSpec>{});

    response.result = run_chat_turn(generate, sandbox, prompt, max_rounds);

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
    // The "Correct solution (facts): {…}" block is the TRAINING target format, not an
    // answer — nothing at serving time parses it, and it leaked verbatim through every
    // surface (MCP, agent_run, and any client that did not know to trim it). Strip it
    // here, once, so no caller has to know about it.
    constexpr std::string_view marker = "Correct solution (facts):";
    if (const std::size_t first = reply.find(marker); first != std::string::npos) {
        reply.erase(first);
    }
    // The model is TRAINED to emit exactly one action — {message, tool_calls, execution_graph}
    // (render_action_example is the SFT target) — so the prose a user should read is
    // `action.message` and the proposed Pipeline is `action.execution_graph`.
    // `parse_studio_action`'s own header says it is "Used by agent::respond". It was not: the
    // harness looked for <tool_call> tags, found none, concluded "the model answered", and
    // assigned the raw envelope as the reply — which is exactly the JSON blob that reached the
    // web app, MCP, and agent_run. Open the envelope instead of rendering it (#916). Assign
    // unconditionally on a parse: an action with an empty message falls through to the
    // fallback below, so the envelope is never what a user sees.
    if (std::optional<StudioAction> action = parse_studio_action(reply)) {
        reply = action->message;
        response.action = std::move(action);
    }
    const std::size_t last = reply.find_last_not_of(" \t\r\n");
    reply.erase(last == std::string::npos ? 0 : last + 1);
    // A generation that was ONLY the facts block leaves nothing behind. Say that
    // plainly rather than returning a blank the caller has to guess at — an empty
    // reply reads as a broken endpoint, which is the wrong diagnosis.
    if (reply.empty()) {
        reply = response.action
                    ? "I put together a pipeline for that but didn't describe it. Open the "
                      "Build tab to review the proposed Pipeline."
                    : "The agent produced no readable answer to that. Try rephrasing, or ask "
                      "about the Project's data, pipeline, or model.";
    }

    response.ok = true;
    return response;
}

}  // namespace zetta::agent
