// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/governance/capability.hpp"

namespace zetta {
class Node;
}

// Internal: what the object capability's REST routes share with its registry
// entries (#1005, docs/data/object-store.md). The byte routes — PUT, PATCH and
// GET /v1/objects/<bucket>/<key> — move raw bodies, so they are served by the
// HTTP adapter (http_server.cpp) rather than as registry calls; everything they
// decide (status codes, error details, the authorization and its audit row) is
// here, beside the capabilities, so the two renderings answer alike.
namespace zetta::api {

// The rights: objects:read, objects:write (the owner lease too), objects:manage.
inline const zetta::governance::Operation kObjectsRead{
    zetta::governance::CapabilityDomain::Data, "objects_read"};
inline const zetta::governance::Operation kObjectsWrite{
    zetta::governance::CapabilityDomain::Data, "objects_write"};
inline const zetta::governance::Operation kObjectsManage{
    zetta::governance::CapabilityDomain::Orchestration, "objects_manage"};

// The registry error code for a store status (rest_status maps it to HTTP).
std::string object_error_code(zetta::data::ObjectStatus status);

// A failed outcome as a registry result, with what the caller needs to act on
// it in `error.details`: `size` on a conflict or a range outside the object, the
// current `fence` when fenced, the holder's lease when held.
ApiResult object_failure(const zetta::data::ObjectOutcome& outcome);

// The success value of a write (put, put range, remove), with the incarnation
// the write was decided under (ObjectOutcome::incarnation, lake#543).
nlohmann::json object_write_value(const std::string& bucket,
                                  const zetta::data::ObjectOutcome& outcome);

// Authorize one byte-route call and record it in the audit trail, like a
// registry capability: `operation` held flat, or ON BUCKET <prefix> covering
// `bucket`. `route` names it in the audit row (`api:<route>`). Call on the
// node's owner thread. An empty result means allowed.
std::optional<ApiResult> authorize_object_route(Node& node, const std::string& acting_key,
                                                bool acting_is_user, std::string_view route,
                                                const zetta::governance::Operation& operation,
                                                const std::string& bucket);

}  // namespace zetta::api
