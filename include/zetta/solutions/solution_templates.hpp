#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::solutions {

struct TierHoldout {
    std::string tier;
    double fraction = 0.1;
};

struct SolutionExperimentTemplate {
    std::vector<TierHoldout> holdouts;
    std::vector<std::string> treatment_cells = {"treatment"};
    std::string seed_rule = "project_solution_v1";
};

struct SolutionGuardrail {
    std::string metric;
    double bound = 0.0;
    std::string direction = "maximum";
};

struct SolutionMeasurement {
    std::string design = "holdout";
    std::string outcome;
    std::string unit;
    int window_days = 30;
};

// Policy settings use the policy engine's validated configuration type/schema.
// These are executable defaults, not descriptions interpreted by an agent.
struct SolutionTemplates {
    nlohmann::json policy = {{"autonomy", "A0"}, {"kill_switch", false}};
    SolutionExperimentTemplate experiment;
    std::vector<SolutionGuardrail> guardrails;
    SolutionMeasurement measurement;
    bool minimal = true;
};

SolutionTemplates solution_templates(const std::string& solution_id);
nlohmann::json solution_policy_template(const std::string& solution_id,
                                        int level);
nlohmann::json to_json(const SolutionExperimentTemplate& value);
nlohmann::json to_json(const SolutionGuardrail& value);
nlohmann::json to_json(const SolutionMeasurement& value);
const nlohmann::json& solution_experiment_template_schema();
const nlohmann::json& solution_guardrail_schema();
const nlohmann::json& solution_measurement_schema();

} // namespace zetta::solutions
