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

namespace zetta::api {

namespace {

nlohmann::json backup_error(std::string description) {
    return {{"description", std::move(description)},
            {"content",
             {{"application/json",
               {{"schema", {{"$ref", "#/components/schemas/error_envelope"}}}}}}}};
}

nlohmann::json summary_schema() {
    return {{"type", "object"},
            {"additionalProperties", {{"type", "integer"}}},
            {"description",
             "counts by stable name: table:<db>.<table> (rows), bucket:<name> (objects), "
             "collection:<name> (records), catalogue:<family> (entries), bodies, "
             "bodies:missing"}};
}

// The envelope with the success value's snapshot and summary at its top level.
nlohmann::json backup_reply(std::string description, bool with_snapshot) {
    nlohmann::json properties{{"ok", {{"type", "boolean"}, {"const", true}}},
                              {"value", {{"type", "object"}}},
                              {"summary", summary_schema()},
                              {"trace_id", {{"type", "string"}}},
                              {"span_id", {{"type", "string"}}},
                              {"duration_ms", {{"type", "number"}}}};
    if (with_snapshot) properties["snapshot"] = {{"type", "string"}};
    return {{"description", std::move(description)},
            {"content",
             {{"application/json",
               {{"schema",
                 {{"type", "object"},
                  {"required", {"ok", "value", "summary"}},
                  {"properties", std::move(properties)}}}}}}}};
}

nlohmann::json octets() {
    return {{"application/octet-stream",
             {{"schema", {{"type", "string"}, {"format", "binary"}}}}}};
}

nlohmann::json tagged(nlohmann::json operation) {
    operation["tags"] = nlohmann::json::array({"backups"});
    return operation;
}

}  // namespace

nlohmann::json backup_route_paths() {
    const nlohmann::json id{{"name", "id"},
                            {"in", "path"},
                            {"required", true},
                            {"description", "the snapshot's id"},
                            {"schema", {{"type", "string"}}}};
    nlohmann::json paths = nlohmann::json::object();
    paths["/v1/backup_snapshot"]["post"] = tagged({
        {"operationId", "backup_snapshot"},
        {"summary", "Take a backup snapshot"},
        {"description",
         "Pause record and object writes on every member, capture one consistent snapshot, "
         "and write it as one archive kept for 6 hours. Requires api:cluster_admin "
         "(cluster:admin)."},
        {"responses",
         {{"200", backup_reply("the snapshot's id and what it counted", true)},
          {"409", backup_error("a body is held only by other members and none gave it whole")},
          {"503", backup_error("another snapshot holds the writes, a member did not pause, or "
                               "writes did not drain (`busy`): retry")},
          {"4XX", backup_error("the authorization is at fault")}}}});
    nlohmann::json& item = paths["/v1/backup_snapshot/{id}"];
    item["get"] = tagged({
        {"operationId", "get_backup_snapshot"},
        {"summary", "Stream a backup snapshot's archive"},
        {"description", "The zetta-backup/1 archive, streamed. Requires api:cluster_admin."},
        {"parameters", {id}},
        {"responses",
         {{"200", {{"description", "the archive"}, {"content", octets()}}},
          {"404", backup_error("released or expired")},
          {"4XX", backup_error("the authorization is at fault")}}}});
    item["delete"] = tagged({
        {"operationId", "release_backup_snapshot"},
        {"summary", "Release a backup snapshot"},
        {"description", "Remove the snapshot's archive. Requires api:cluster_admin."},
        {"parameters", {id}},
        {"responses",
         {{"200", backup_reply("released", true)},
          {"404", backup_error("released or expired")},
          {"4XX", backup_error("the authorization is at fault")}}}});
    paths["/v1/restore_snapshot"]["post"] = tagged({
        {"operationId", "restore_snapshot"},
        {"summary", "Restore a backup snapshot"},
        {"description",
         "Restore the archive in the body into this node, which must hold no user data, and "
         "verify it against the archive's manifest. Requires api:cluster_admin."},
        {"requestBody", {{"required", true}, {"content", octets()}}},
        {"responses",
         {{"200", backup_reply("what the node holds now, equal to the manifest's counts", true)},
          {"409", backup_error("the node holds user data")},
          {"422", backup_error("the restored counts are not the manifest's "
                               "(`verification_failed`: details.differences)")},
          {"4XX", backup_error("the archive is truncated or damaged, or the authorization is "
                               "at fault")}}}});
    paths["/v1/restore_snapshot:discard"]["post"] = tagged({
        {"operationId", "discard_restored_snapshot"},
        {"summary", "Empty a restored scratch target"},
        {"description",
         "Remove what a restore wrote. Refused unless the node was started with "
         "--restore-target, holds a restore record, the body names it and its snapshot, and "
         "nothing was written since. Requires api:cluster_admin."},
        {"requestBody",
         {{"required", true},
          {"content",
           {{"application/json",
             {{"schema",
               {{"type", "object"},
                {"required", {"node_id", "snapshot"}},
                {"properties",
                 {{"node_id", {{"type", "string"}}}, {"snapshot", {{"type", "string"}}}}}}}}}}}}},
        {"responses",
         {{"200", backup_reply("emptied", true)},
          {"409", backup_error("not a restore target, not restored, or changed since")},
          {"4XX", backup_error("the body or the authorization is at fault")}}}});
    return paths;
}

}  // namespace zetta::api
