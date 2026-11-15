#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "zetta/api/registry.hpp"
#include "zetta/data/backup_snapshot.hpp"
#include "zetta/governance/capability.hpp"

namespace zetta {
class Node;
}

// Internal: the authorization, audit and result mapping the backup routes
// share (zetta#1518, docs/data/backup-snapshot.md). The routes move raw
// archives, so the HTTP adapter (http_server.cpp) serves them rather than the
// registry, like the object byte routes; what they decide is here.
//
//   POST   /v1/backup_snapshot                take a snapshot: {snapshot, summary}
//   GET    /v1/backup_snapshot/<id>           stream its archive
//   DELETE /v1/backup_snapshot/<id>           release it
//   POST   /v1/restore_snapshot               restore an archive (the body): {summary}
//   POST   /v1/restore_snapshot:discard       empty a restored scratch target
namespace zetta::api {

// Every backup route needs cluster:admin.
inline const zetta::governance::Operation kClusterAdmin{
    zetta::governance::CapabilityDomain::Api, "cluster_admin"};

// Authorize one backup-route call and record the decision in the audit trail
// (`api:<route>`). Call on the node's owner thread. Empty means allowed.
std::optional<ApiResult> authorize_backup_route(Node& node, const std::string& acting_key,
                                                bool acting_is_user, std::string_view route);

// Record what a backup-route call did: a data change for a restore or a
// discard that wrote, a data access otherwise. Call on the node's owner thread.
void audit_backup_outcome(Node& node, const std::string& acting_key, std::string_view route,
                          bool changed_data, bool ok, std::string detail);

// A backup outcome as a registry result: the success value carries `snapshot`
// (when there is one) and `summary`; a refusal its code, message and details.
ApiResult backup_result(const zetta::data::BackupOutcome& outcome);

// The OpenAPI rendering of the backup routes (zetta#1520), merged into the
// document beside the object routes (rest.cpp).
nlohmann::json backup_route_paths();

}  // namespace zetta::api
