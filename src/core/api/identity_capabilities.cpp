// Cluster identity administration on the control surface
// (docs/governance/identity-seal.md, docs/cluster/security.md):
//
//   rotate_identity_root  action  hand identity to the root a new admin token
//                                 derives (#1342)
//
// Every capability here needs cluster:admin (api:cluster_admin) and is audited
// per call by the registry.

#include <utility>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required)}};
}

const nlohmann::json& rotate_identity_root_schema() {
    static const nlohmann::json schema = object_schema(
        {{"new_admin_token",
          {{"type", "string"},
           {"minLength", 16},
           {"maxLength", 4096},
           {"description",
            "the new owner admin token; the new root derives from it. Never "
            "returned, logged or stored"}}},
         {"revoke_tokens",
          {{"type", "boolean"},
           {"description",
            "remove every token instead of re-sealing it (a rotation after a "
            "compromise); the owner gets a token for the new admin token either "
            "way. Default false"}}}},
        {"new_admin_token"});
    return schema;
}

const nlohmann::json& rotate_identity_root_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"old_root", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
         {"new_root", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
         {"resealed", {{"type", "integer"}, {"minimum", 0}}},
         {"revoked", {{"type", "integer"}, {"minimum", 0}}},
         {"owner_token_id", {{"type", "string"}}}},
        {"old_root", "new_root", "resealed", "revoked", "owner_token_id"});
    return schema;
}

ApiResult handle_rotate_identity_root(const nlohmann::json& input, ApiContext& context) {
    const Node::IdentityRotationResult rotation = context.node.rotate_identity_root(
        input.at("new_admin_token").get<std::string>(), input.value("revoke_tokens", false));
    if (!rotation.ok) return ApiResult::failure("conflict", rotation.reason);
    return ApiResult::success({{"old_root", rotation.old_root},
                               {"new_root", rotation.new_root},
                               {"resealed", rotation.resealed},
                               {"revoked", rotation.revoked},
                               {"owner_token_id", rotation.owner_token_id}});
}

}  // namespace

void register_identity_capabilities(Registry& registry) {
    registry.add(Capability{
        "rotate_identity_root",
        "Rotate the identity root",
        "Hand the cluster's identity to the root a new owner admin token derives "
        "(#1342). Runs on a node that holds the current root: it writes a "
        "succession sealed by the current root, re-seals every identity record "
        "under the new one, issues the owner a token for the new admin token, "
        "and re-pins. Every node moves its pin when the succession reaches it; "
        "records the old root seals afterwards are refused. Requires "
        "cluster:admin. Mutates replicated governance state.",
        rotate_identity_root_schema(),
        rotate_identity_root_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Api, "cluster_admin"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_rotate_identity_root});
}

}  // namespace zetta::api
