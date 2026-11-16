// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/solutions/solution_templates.hpp"

// The solution registry (#1102): catalogue modules are configurations over the
// methodology registry and the shared production foundation.
//
// See docs/solutions/README.md.
namespace zetta::ml {

struct SolutionMethodology {
    std::string id;
    std::string role;
    int minimum_level = 1;
};

struct SolutionDataRole {
    std::string role;
    bool required = false;
    std::string description;
};

struct SolutionAutonomyDefault {
    std::string action_class;
    std::string autonomy;
};

struct SolutionCapabilityLevel {
    int level = 1;
    std::string name;
    std::string description;
};

struct MissingSolutionComponent {
    std::string id;
    std::string name;
    std::string issue;
};

struct Solution {
    std::string id;
    std::string name;
    std::string promise;
    std::string use_case;
    std::vector<SolutionMethodology> composition;
    std::vector<std::string> pipeline_order;
    std::vector<std::string> action_patterns;
    std::vector<std::string> latency_classes;
    std::vector<SolutionAutonomyDefault> autonomy_defaults;
    std::vector<SolutionDataRole> required_data_roles;
    std::string decision_policy_template;
    std::string measurement_design;
    std::vector<std::string> guardrails;
    zetta::solutions::SolutionTemplates templates;
    std::vector<SolutionCapabilityLevel> capability_levels;
    std::vector<MissingSolutionComponent> missing_components;
    std::string status;
    bool runnable = false;
    std::string issue;
    std::string promotion_condition;
};

struct ComposedSolution {
    ir::ExecutionGraph graph;
    std::vector<std::string> methodologies;
    std::vector<MissingSolutionComponent> missing_components;
    bool preview = false;
    std::string error;

    bool ok() const {
        return error.empty();
    }
};

const std::vector<Solution>& solutions();
const Solution* find_solution(std::string_view id);
nlohmann::json solution_json(const Solution& solution);

ComposedSolution compose_solution(const Solution& solution, int level,
                                  const nlohmann::json& sources,
                                  const nlohmann::json& snapshot = {},
                                  const nlohmann::json& dataset = {});
ir::ExecutionGraph solution_stage_graph(const ir::ExecutionGraph& graph,
                                        int stage);

} // namespace zetta::ml
