#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::solutions {

const nlohmann::json& solution_foundation_schema();
std::string validate_solution_binding(const nlohmann::json& binding);
api::ApiResult create_composed_solution(const nlohmann::json& input,
                                        api::ApiContext& context);
api::ApiResult run_composed_solution(const nlohmann::json& input,
                                     api::ApiContext& context);

// Called on the Node owner before/after the registered methodology stages.
api::ApiResult prepare_solution_snapshot(Node& node, const nlohmann::json& binding);
api::ApiResult finish_solution_run(Node& node, const nlohmann::json& binding,
                                   const std::string& run_id,
                                   const std::vector<pipeline::RunOutput>& scores,
                                   const std::string& actor);

} // namespace zetta::solutions
