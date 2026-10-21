#include "zetta/pipeline/step_authority.hpp"

#include <memory>
#include <string>
#include <utility>

#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::pipeline {
namespace {

using zetta::governance::AccessDecision;
using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

std::string right_name(const Operation& operation) {
    return std::string(zetta::governance::to_string(operation.domain)) + ":" +
           operation.action;
}

std::string string_attribute(const ir::Node& step, const char* key) {
    if (!step.attributes.is_object()) return {};
    const auto found = step.attributes.find(key);
    return found != step.attributes.end() && found->is_string() ? found->get<std::string>()
                                                                : std::string();
}

}  // namespace

Operation step_operation(ir::StepKind kind) {
    switch (kind) {
        case ir::StepKind::Connect: return {CapabilityDomain::Data, "read"};
        case ir::StepKind::Clean: return {CapabilityDomain::Data, "write"};
        case ir::StepKind::Model: return {CapabilityDomain::Orchestration, "train_model"};
        case ir::StepKind::Index: return {CapabilityDomain::Orchestration, "create_index"};
        case ir::StepKind::Serve: return {CapabilityDomain::Orchestration, "expose_model"};
        case ir::StepKind::Act:
            return {CapabilityDomain::Orchestration, "deliver_action_task"};
    }
    // An unknown kind maps to a right no role holds, so it is denied.
    return {CapabilityDomain::Orchestration, "unknown_step_kind"};
}

std::optional<std::pair<std::string, std::string>> step_table(const ir::Node& step) {
    std::string source = string_attribute(step, "source");
    std::string table = string_attribute(step, "table");
    if (source.empty() || table.empty()) return std::nullopt;
    return std::make_pair(std::move(source), std::move(table));
}

AccessDecision decide_step(const Node& node, const RunPrincipal& principal,
                           const ir::Node& step) {
    if (principal.local()) return AccessDecision{true, "trusted local session"};
    const Operation operation = step_operation(step.kind);
    AccessDecision decision = principal.acting_is_user
                                  ? node.authorize_user(principal.acting_id, operation)
                                  : node.authorize(principal.acting_id, operation);
    if (decision) return decision;
    // A user whose read right is scoped to objects (sql:read ON TABLE x) holds no
    // flat data:read, but may still read the one table its connect step names.
    if (principal.acting_is_user && step.kind == ir::StepKind::Connect) {
        if (const auto table = step_table(step)) {
            const zetta::governance::ColumnAccess access = node.column_scope(
                principal.acting_id, /*acting_is_user=*/true, operation, table->first,
                table->second);
            if (access.table_allowed) {
                return AccessDecision{true, "sql:read granted on " + table->first + "." +
                                                table->second};
            }
            decision.reason += " (and no grant on " + table->first + "." +
                               table->second + ")";
        }
    }
    return decision;
}

AccessDecision authorize_step(Node& node, const RunPrincipal& principal,
                              const ir::Node& step, const std::string& run_id) {
    AccessDecision decision = decide_step(node, principal, step);
    if (principal.local()) return decision;
    const Operation operation = step_operation(step.kind);
    std::string detail = std::string(ir::to_string(step.kind)) + " step '" +
                         step_label(step) + "' needs " + right_name(operation);
    if (const auto table = step_table(step);
        table && step.kind == ir::StepKind::Connect) {
        detail += " on " + table->first + "." + table->second;
    }
    if (!run_id.empty()) detail += " · run " + run_id;
    detail += decision ? " · granted" : " · denied: " + decision.reason;

    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DataAccess;
    event.actor = principal.acting_id;
    event.target = "step:" + std::string(ir::to_string(step.kind)) + ":" + step_label(step);
    event.allowed = decision.allowed;
    event.detail = std::move(detail);
    event.time = zetta::util::now_iso8601();
    node.record_audit_event(std::move(event));
    return decision;
}

GraphAuthorization authorize_graph_steps(Node& node, const RunPrincipal& principal,
                                         const ir::ExecutionGraph& graph,
                                         const std::string& run_id) {
    GraphAuthorization result;
    if (principal.local()) return result;
    for (const std::shared_ptr<ir::Node>& step : graph.nodes()) {
        if (!step) continue;
        const AccessDecision decision = authorize_step(node, principal, *step, run_id);
        if (!decision) {
            result.allowed = false;
            result.step_id = step->id;
            result.reason = std::string(ir::to_string(step->kind)) + " step '" +
                            step_label(*step) + "' denied: " + decision.reason;
            return result;
        }
    }
    return result;
}

}  // namespace zetta::pipeline
