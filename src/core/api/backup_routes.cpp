#include "backup_routes.hpp"

#include <utility>

#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"

namespace zetta::api {

std::optional<ApiResult> authorize_backup_route(Node& node, const std::string& acting_key,
                                                bool acting_is_user, std::string_view route) {
    bool allowed = true;
    std::string reason;
    if (acting_is_user) {
        const auto decision = node.authorize_user(acting_key, kClusterAdmin);
        allowed = static_cast<bool>(decision);
        if (!allowed) reason = "cluster:admin is required: " + decision.reason;
    } else if (!acting_key.empty()) {
        const auto decision = node.authorize(acting_key, kClusterAdmin);
        allowed = static_cast<bool>(decision);
        if (!allowed) reason = "cluster:admin is required: " + decision.reason;
    }
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DataAccess;
    event.actor = acting_key.empty() ? "local" : acting_key;
    event.target = "api:" + std::string(route);
    event.allowed = allowed;
    event.detail = allowed ? "cluster:admin" : reason;
    node.record_audit_event(std::move(event));
    if (allowed) return std::nullopt;
    return ApiResult::failure("unauthorized", reason);
}

void audit_backup_outcome(Node& node, const std::string& acting_key, std::string_view route,
                          bool changed_data, bool ok, std::string detail) {
    zetta::governance::AuditEventValue event;
    event.action = changed_data ? zetta::governance::AuditAction::DataChanged
                                : zetta::governance::AuditAction::DataAccess;
    event.actor = acting_key.empty() ? "local" : acting_key;
    event.target = "api:" + std::string(route);
    event.allowed = ok;
    event.detail = std::move(detail);
    node.record_audit_event(std::move(event));
}

ApiResult backup_result(const zetta::data::BackupOutcome& outcome) {
    if (!outcome.ok()) {
        ApiResult result = ApiResult::failure(std::string(zetta::data::to_string(outcome.status)),
                                              outcome.message);
        if (!outcome.details.is_null() && !outcome.details.empty()) {
            result.error.details = outcome.details;
        }
        return result;
    }
    nlohmann::json value{{"summary", zetta::data::summary_to_json(outcome.summary)}};
    if (!outcome.snapshot.empty()) value["snapshot"] = outcome.snapshot;
    if (outcome.size_bytes > 0) value["size_bytes"] = outcome.size_bytes;
    if (!outcome.details.is_null() && !outcome.details.empty()) value["details"] = outcome.details;
    if (!outcome.message.empty()) value["message"] = outcome.message;
    return ApiResult::success(std::move(value));
}

}  // namespace zetta::api
