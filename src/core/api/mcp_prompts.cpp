#include "zetta/api/mcp_prompts.hpp"

#include <string>

namespace zetta::api {
namespace {

// A user-role text message, the one shape these prompts emit.
nlohmann::json user_text(std::string text) {
    return {{"role", "user"},
            {"content", {{"type", "text"}, {"text", std::move(text)}}}};
}

// Read a string argument, or a default when absent/blank. MCP prompt arguments arrive as
// strings; treat a missing or empty value as "not supplied".
std::string argument_or(const nlohmann::json& arguments, const std::string& key,
                        const std::string& fallback) {
    if (!arguments.is_object()) return fallback;
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_string()) return fallback;
    const std::string value = found->get<std::string>();
    return value.empty() ? fallback : value;
}

// The build_and_run prompt body: a workflow over agent_run + get_run. Kept as one
// instruction message so a host expands it into a single, self-contained turn.
nlohmann::json build_and_run_messages(const nlohmann::json& arguments) {
    const std::string ask = argument_or(
        arguments, "ask",
        "predict which customers are about to churn, with calibrated risk scores and "
        "explanations, then serve a ranked at-risk list");
    const std::string agent = argument_or(arguments, "agent", "");

    std::string text;
    text += "Use this Zetta node's tools to have an agent BUILD and RUN an execution graph "
            "for the following ask, then report what happened.\n\n";
    text += "Ask: \"" + ask + "\"\n\n";
    text += "Steps:\n";
    if (agent.empty()) {
        text += "1. Call `list_agents` and pick the base agent (the one whose model is a "
                "base-agent checkpoint; if there is only one agent, use it). If there are no "
                "agents, tell the user to create one in the Agents view (or with `create_agent`) "
                "and stop.\n";
    } else {
        text += "1. Use the agent \"" + agent + "\" (its id or name).\n";
    }
    text += "2. Call `agent_run` with { agent: <the agent id>, message: the ask above, "
            "top_k: 10 }. This makes the agent recognize the use case, the engine compile a "
            "real execution graph from the ask, and the graph run through the gate + the "
            "engine — all in one call. Do NOT build the graph by hand; agent_run does it.\n";
    text += "3. From the result, note the agent's `reply` (its recognition of the ask), the "
            "`shape` (e.g. churn), the `steps` (the connect -> clean -> model -> index -> serve "
            "-> act graph), and `run.run_id`.\n";
    text += "4. Poll `get_run` with that run_id until `status` is \"succeeded\" or \"failed\" "
            "(a churn run finishes in a few seconds; re-read every second or two).\n";
    text += "5. Report back: what the agent recognized, the graph it built (list the steps), "
            "and the run outcome — on success, the trained model and the served outputs "
            "(the ranked at-risk list + retention playbook from the run's `outputs`); on "
            "failure, the failing step and reason from the run's logs.\n\n";
    text += "Keep it concrete and grounded in the actual tool results — this is a real run on "
            "the node, not a plan.";

    nlohmann::json messages = nlohmann::json::array();
    messages.push_back(user_text(std::move(text)));
    return messages;
}

}  // namespace

nlohmann::json mcp_prompt_definitions() {
    nlohmann::json prompts = nlohmann::json::array();
    prompts.push_back(
        {{"name", "build_and_run"},
         {"title", "Build & run a use case"},
         {"description",
          "Have an agent build AND run a real execution graph from a plain-language ask "
          "(e.g. churn), then report the built graph + the run outcome. Drives the "
          "agent_run + get_run tools; the base agent needs no fine-tune for this."},
         {"arguments",
          nlohmann::json::array(
              {{{"name", "ask"},
                {"description",
                 "the plain-language problem to solve, e.g. 'predict which customers will "
                 "churn, with calibrated scores and explanations'"},
                {"required", true}},
               {{"name", "agent"},
                {"description",
                 "the agent id or name to use (optional; defaults to the node's base agent)"},
                {"required", false}}})}});
    return prompts;
}

std::optional<nlohmann::json> render_mcp_prompt(const std::string& name,
                                                const nlohmann::json& arguments,
                                                std::string* error) {
    if (name == "build_and_run") {
        return nlohmann::json{
            {"description",
             "Build + run an execution graph from a plain-language ask via agent_run."},
            {"messages", build_and_run_messages(arguments)}};
    }
    if (error != nullptr) *error = "no MCP prompt named '" + name + "'";
    return std::nullopt;
}

}  // namespace zetta::api
