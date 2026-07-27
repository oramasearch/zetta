#include "zetta/agent/goal.hpp"

#include "zetta/agent/action.hpp"
#include "zetta/agent/response_facts.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace zetta::agent {
namespace {

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

bool mentions(const std::string& haystack, std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&haystack](const char* needle) {
        return haystack.find(needle) != std::string::npos;
    });
}

}  // namespace

const std::vector<GoalCheck>& goal_check_catalogue() {
    // Every entry here is something `response_solution_facts` can decide from a response. Adding a
    // check means teaching the projection to see it first — otherwise the goal is unrewardable and
    // training would optimize a constant.
    static const std::vector<GoalCheck> catalogue = {
        {"numbers_from_context",
         "every figure it states was actually retrieved, not estimated or rounded"},
        {"grounded", "it answered from the supplied facts rather than from training priors"},
        {"leakage_free", "features see only what was known at the time"},
        {"point_in_time", "the data is cut at the scoring date"},
        {"temporal_validation", "it is validated on later data than it trained on"},
        {"calibrated", "the probabilities mean what they say"},
        {"approval_gated", "nothing acts on the world without an approval"},
        {"preserved_other_steps", "a change leaves the rest of the Pipeline alone"},
        {"built_nothing", "it declines or asks rather than building something unasked"},
        {"asked_one_question", "when it needs to clarify, it asks exactly one thing"},
        {"capability_exists", "it only names capabilities that actually exist"},
        {"code_runs", "the code it writes executes"},
    };
    return catalogue;
}

std::vector<std::string> interpret_goal(const std::string& statement) {
    // Deterministic keyword mapping. Crude on purpose: it is the fallback that runs with no model
    // available, and being crude-but-inspectable is the right failure mode for something that
    // decides what training optimizes. A GoalInterpreter backed by a model replaces the mapping,
    // never the checks.
    const std::string text = lowered(statement);
    std::vector<std::string> checks;
    const auto add = [&checks](const char* name) {
        if (std::find(checks.begin(), checks.end(), name) == checks.end()) checks.emplace_back(name);
    };

    if (mentions(text, {"fabricat", "make up", "made up", "invent", "hallucinat", "evidence",
                        "cannot evidence", "number", "figure", "cite"})) {
        add("numbers_from_context");
        add("grounded");
    }
    if (mentions(text, {"leak", "point-in-time", "point in time", "future"})) {
        add("leakage_free");
        add("point_in_time");
    }
    if (mentions(text, {"validat", "holdout", "hold-out", "later data", "backtest", "honest "})) {
        add("temporal_validation");
    }
    if (mentions(text, {"calibrat", "probabilit", "confidence"})) {
        add("calibrated");
    }
    if (mentions(text, {"approval", "approve", "without asking", "human in the loop",
                        "act on the world", "auto-block", "permission"})) {
        add("approval_gated");
    }
    if (mentions(text, {"don't break", "do not break", "preserve", "leave the rest",
                        "only change"})) {
        add("preserved_other_steps");
    }
    if (mentions(text, {"decline", "refuse", "out of scope", "don't guess", "do not guess",
                        "say i don't know", "admit"})) {
        add("built_nothing");
    }
    if (mentions(text, {"clarif", "ask when", "ambiguous", "one question"})) {
        add("asked_one_question");
        add("built_nothing");
    }
    if (mentions(text, {"real capabilit", "exist", "oversell", "overpromis", "only offer"})) {
        add("capability_exists");
    }
    if (mentions(text, {"code", "python", "sql", "runs", "executab"})) {
        add("code_runs");
    }
    if (mentions(text, {"ground", "from the data", "from our data", "retrieved"})) {
        add("grounded");
    }
    return checks;
}

void interpret_goals(std::vector<Goal>& goals) {
    for (Goal& goal : goals) {
        if (goal.checks.empty()) goal.checks = interpret_goal(goal.statement);
    }
}

std::vector<RubricTerm> goal_rubric_terms(const std::vector<Goal>& goals, const EndState& facts) {
    std::vector<RubricTerm> terms;
    terms.reserve(goals.size());
    for (const Goal& goal : goals) {
        RubricTerm term;
        term.name = goal.statement;
        term.weight = goal.importance < 0.0 ? 0.0 : goal.importance;

        int engaged = 0;
        int satisfied = 0;
        for (const std::string& check : goal.checks) {
            // A check the response never engaged is not a failure — the turn simply had no bearing
            // on it. Counting it as zero would punish an agent for a goal the question never
            // raised, which is how a reward starts pushing behaviour nobody asked for.
            const auto found = facts.facts().find(check);
            if (found == facts.facts().end()) continue;
            ++engaged;
            if (found->second.is_boolean() ? found->second.get<bool>() : true) ++satisfied;
        }
        term.active = engaged > 0;
        term.score = engaged > 0 ? static_cast<double>(satisfied) / static_cast<double>(engaged)
                                 : 0.0;
        terms.push_back(std::move(term));
    }
    return terms;
}

double goal_reward(const std::vector<Goal>& goals, const EndState& facts) {
    return rubric_reward(goal_rubric_terms(goals, facts));
}

std::vector<std::string> unmapped_goals(const std::vector<Goal>& goals) {
    std::vector<std::string> unmapped;
    for (const Goal& goal : goals) {
        if (goal.checks.empty()) unmapped.push_back(goal.statement);
    }
    return unmapped;
}

double goal_reward_for_generation(const std::vector<Goal>& goals, const std::string& generation,
                                  const std::string& grounding) {
    // A generation the harness cannot parse is worth nothing, whatever it says. Scoring it on the
    // goals it happens to mention would reward text that reads well and does not run — which is
    // precisely the failure RLVR exists to avoid.
    const std::optional<StudioAction> action = parse_studio_action(generation);
    if (!action) return 0.0;
    return goal_reward(goals, response_solution_facts(*action, grounding));
}

nlohmann::json to_json(const Goal& goal) {
    return nlohmann::json{{"statement", goal.statement},
                          {"importance", goal.importance},
                          {"checks", goal.checks}};
}

std::optional<Goal> goal_from_json(const nlohmann::json& value, std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<Goal> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("goal: not a JSON object");

    Goal goal;
    const auto statement = value.find("statement");
    if (statement == value.end() || !statement->is_string() ||
        statement->get<std::string>().empty()) {
        return fail("goal.statement: required, must be a non-empty string");
    }
    goal.statement = statement->get<std::string>();

    if (const auto importance = value.find("importance"); importance != value.end()) {
        if (!importance->is_number()) return fail("goal.importance: must be a number");
        goal.importance = importance->get<double>();
        if (goal.importance < 0.0) return fail("goal.importance: must not be negative");
    }
    if (const auto checks = value.find("checks"); checks != value.end()) {
        if (!checks->is_array()) return fail("goal.checks: must be an array");
        for (const nlohmann::json& check : *checks) {
            if (!check.is_string()) return fail("goal.checks[]: must be strings");
            goal.checks.push_back(check.get<std::string>());
        }
    }
    return goal;
}

const nlohmann::json& goal_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["required"] = nlohmann::json::array({"statement"});
        document["additionalProperties"] = false;
        document["properties"]["statement"] = {
            {"type", "string"},
            {"minLength", 1},
            {"description", "what the agent should be good at, in plain language"}};
        document["properties"]["importance"] = {
            {"type", "number"},
            {"minimum", 0},
            {"description",
             "how much this goal matters relative to the others; 0 states it without training on "
             "it"}};
        document["properties"]["checks"] = {
            {"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "the verifiable checks this goal resolves to; derived from the statement when omitted"}};
        return document;
    }();
    return schema;
}

}  // namespace zetta::agent
