#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/solutions/action_integration.hpp"

namespace zetta::solutions {

const nlohmann::json& solution_foundation_schema();
std::string validate_solution_binding(const nlohmann::json& binding);
api::ApiResult create_composed_solution(const nlohmann::json& input,
                                        api::ApiContext& context);
api::ApiResult run_composed_solution(const nlohmann::json& input,
                                     api::ApiContext& context);

// A report the run still has to deliver: prepared (persisted, ledger-recorded,
// request built) on the Node owner by finish_solution_run, then sent OFF the
// owner by the run's coroutine, each attempt recorded back on the owner
// (lake#801). `prepared` is empty when the solution publishes no report or the
// report is already settled.
struct DeferredReportDelivery {
    std::optional<PreparedReportPublication> prepared;
    // The run's failure reason when delivery is not acknowledged; the last
    // attempt's reason is appended.
    std::string failure;
};

// Called on the Node owner before/after the registered methodology stages.
// With `deferred` null, finish_solution_run delivers the report itself,
// synchronously; with it set, it only prepares the report and leaves the
// sending to the caller, so the owner is never held by the network.
api::ApiResult prepare_solution_snapshot(Node& node, const nlohmann::json& binding);
api::ApiResult finish_solution_run(Node& node, const nlohmann::json& binding,
                                   const std::string& run_id,
                                   const std::vector<pipeline::RunOutput>& scores,
                                   const std::string& actor,
                                   DeferredReportDelivery* deferred = nullptr);

} // namespace zetta::solutions
