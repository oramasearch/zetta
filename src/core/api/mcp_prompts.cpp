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

// The model_from_source prompt: the DETERMINISTIC path, end to end, with no
// agent and no checkpoint. `build_and_run` drives agent_run, which needs an
// installed agent — and a fresh node has none (#977), so the one prompt a new
// node advertised was the one it could not run. This is the path that works on
// a node five minutes old, and it carries the two column rules that are
// otherwise learned one step too late, from a failure (#994).
nlohmann::json model_from_source_messages(const nlohmann::json& arguments) {
    const std::string connection =
        argument_or(arguments, "connection", "<the saved connection name>");
    const std::string ask =
        argument_or(arguments, "ask",
                    "predict which accounts are likely to churn, with "
                    "calibrated scores and explanations");
    const std::string database = argument_or(arguments, "database", "analytics");

    std::string text;
    text += "Train a model on data that lives in an external source, using this "
            "Zetta node's tools. No agent or checkpoint is needed \u2014 every step "
            "below is a deterministic capability call.\n\n";
    text += "Ask: \"" + ask + "\"\nConnection: " + connection + "\n\n";

    text += "1. Call `list_source_tables` { connection: \"" + connection +
            "\" } to see what is in the source. It lands nothing. Narrow with "
            "`match` (e.g. match: \"score\" for scoring tables, \"account\" for "
            "account tables) and read the columns and types it returns \u2014 you "
            "need them for step 2.\n\n";

    text += "2. Write ONE `SELECT` that produces a model-ready frame, and pull it "
            "with `create_dataset_from_query` { connection, query, database: \"" +
            database + "\", table_name }. Do the joins, aggregates and windows "
            "HERE \u2014 the query runs on the source, so anything the source's SQL "
            "supports is available, and this is the only feature-engineering step "
            "there is.\n"
            "   THREE RULES the frame must satisfy, all enforced later and all "
            "fixed here:\n"
            "   - The ENTITY column must be a 64-bit INTEGER. A text/UUID business "
            "id is refused. Derive one, e.g. for Postgres:\n"
            "       ('x'||substr(md5(account_id),1,8))::bit(32)::bigint AS entity_key\n"
            "   - The LABEL must be numeric {0,1}. A boolean lands fine and then "
            "fails mid-run. Cast it: churned::int AS churned\n"
            "   - Every FEATURE must be numeric. Text columns are dropped "
            "silently, so encode what matters: (plan = 'free')::int AS is_free\n"
            "   Keep the original business id in a SECOND dataset if you will need "
            "to join results back to readable names \u2014 do not carry it in the "
            "training frame.\n\n";

    text += "3. Build the pipeline: `create_project` { name }, then "
            "`capture_intent` { project, text: the ask }, then `compile_intent` "
            "{ project }. Read the steps it compiled back to the user.\n\n";

    text += "4. `approve_plan` { project, graph }, then `gate` { project, graph }. "
            "A freshly compiled pipeline is `proposed` and will NOT run until "
            "approved \u2014 the error is `not_approved`, which reads like a bug and "
            "is not one. Read the gate's REASONS, not just its verdict: a check "
            "can pass because there was nothing to check.\n\n";

    text += "5. `run` { project, graph, dataset_path: \"" + database +
            ".<your table>\", dataset: { entity_column, label_column }, "
            "holdout_fraction: 0.3 }.\n"
            "   ALWAYS set holdout_fraction. Without it every metric is measured "
            "on the rows the model trained on and they flatter badly. Poll "
            "`get_run` until succeeded or failed.\n\n";

    text += "6. Report with `get_run_report` { run_id }. Quote `held_out_auc`, "
            "NEVER `in_sample_auc` \u2014 both are returned and the gap between them "
            "is the overfitting signal. Repeat any `caveats` verbatim; the report "
            "volunteers when it should not be trusted.\n\n";

    text += "7. For the ranked list afterwards use `score` { model, dataset_path, "
            "dataset }, and for WHY one account scored as it did use `explain` "
            "{ model, entity, dataset_path, dataset } \u2014 which returns a signed "
            "contribution per feature. `score`'s drivers are the population "
            "ranking and are identical for every row; only `explain` answers the "
            "per-account question. Note both take the dataset contract again \u2014 "
            "they do not inherit it from the run.\n\n";

    text += "If a step fails, read the engine's reason verbatim and fix the cause "
            "\u2014 do not retry the same call unchanged.";

    nlohmann::json messages = nlohmann::json::array();
    messages.push_back(
        {{"role", "user"},
         {"content", {{"type", "text"}, {"text", std::move(text)}}}});
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
    // The deterministic path — the one that works on a node with no agent
    // installed, which is every fresh node (#977). `build_and_run` above drives
    // agent_run and cannot run there, so this is the prompt to reach for unless
    // an agent is known to be present.
    prompts.push_back(
        {{"name", "model_from_source"},
         {"title", "Train a model on data in an external source"},
         {"description",
          "End to end with NO agent and no checkpoint: browse a connected source, pull a "
          "model-ready frame with one SELECT, compile + approve + gate the pipeline, run "
          "it with a holdout, and read the report. Carries the column rules (integer "
          "entity, numeric label) that are otherwise learned from a mid-run failure."},
         {"arguments",
          nlohmann::json::array(
              {{{"name", "connection"},
                {"description",
                 "the saved connection to read from (see list_connections)"},
                {"required", true}},
               {{"name", "ask"},
                {"description",
                 "the plain-language problem, e.g. 'predict which accounts are likely to "
                 "churn, with calibrated scores and explanations'"},
                {"required", false}},
               {{"name", "database"},
                {"description",
                 "the database to land the frame in (optional; defaults to 'analytics')"},
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
    if (name == "model_from_source") {
        return nlohmann::json{
            {"description",
             "Train a model on data in an external source, deterministically \u2014 no "
             "agent required."},
            {"messages", model_from_source_messages(arguments)}};
    }
    if (error != nullptr) *error = "no MCP prompt named '" + name + "'";
    return std::nullopt;
}

}  // namespace zetta::api
