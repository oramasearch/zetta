// Cluster identity administration on the control surface
// (docs/governance/identity-seal.md, docs/cluster/security.md):
//
//   rotate_identity_root  action  hand identity to the root a new admin token
//                                 derives (#1342)
//   get_identity_epoch    read    the identity epoch this node holds (#1360)
//   revoke_node           action  revoke a node's enrolled key, remove it (#1320)
//   list_node_revocations read    every node revocation this node trusts (#1320)
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

const nlohmann::json& get_identity_epoch_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& get_identity_epoch_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"epoch", {{"type", "integer"}, {"minimum", 0}}},
         {"entries", {{"type", "integer"}, {"minimum", 0}}},
         {"digest", {{"type", "string"}}},
         {"min_epoch", {{"type", "integer"}, {"minimum", 0}}},
         {"floor_met", {{"type", "boolean"}}}},
        {"epoch", "entries", "digest", "min_epoch", "floor_met"});
    return schema;
}

ApiResult handle_get_identity_epoch(const nlohmann::json& /*input*/, ApiContext& context) {
    const Node::IdentityEpochStatus status = context.node.identity_epoch_status();
    return ApiResult::success({{"epoch", status.epoch},
                               {"entries", status.entries},
                               {"digest", status.digest},
                               {"min_epoch", status.min_epoch},
                               {"floor_met", status.floor_met}});
}

const nlohmann::json& revoke_node_schema() {
    static const nlohmann::json schema = object_schema(
        {{"node_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
         {"reason", {{"type", "string"}, {"maxLength", 1024}}},
         {"allow_rekey",
          {{"type", "boolean"},
           {"description",
            "let the node id enroll a NEW key afterwards (re-keying a node); "
            "default false, which keeps the id out for good"}}},
         {"public_key_sha256",
          {{"type", "string"},
           {"pattern", "^[0-9a-f]{64}$"},
           {"description",
            "the key to revoke; defaults to the one the node id's node_cert "
            "record names"}}}},
        {"node_id"});
    return schema;
}

nlohmann::json revocation_schema() {
    return object_schema({{"public_key_sha256", {{"type", "string"}}},
                          {"node_id", {{"type", "string"}}},
                          {"serial", {{"type", "string"}}},
                          {"revoked_by", {{"type", "string"}}},
                          {"revoked_at", {{"type", "string"}}},
                          {"reason", {{"type", "string"}}},
                          {"allow_rekey", {{"type", "boolean"}}}},
                         {"public_key_sha256", "node_id", "serial", "revoked_by",
                          "revoked_at", "reason", "allow_rekey"});
}

nlohmann::json revocation_json(const Node::NodeRevocationEntry& entry) {
    return {{"public_key_sha256", entry.public_key_sha256},
            {"node_id", entry.node_id},
            {"serial", entry.serial},
            {"revoked_by", entry.revoked_by},
            {"revoked_at", entry.revoked_at},
            {"reason", entry.reason},
            {"allow_rekey", entry.allow_rekey}};
}

const nlohmann::json& revoke_node_output_schema() {
    static const nlohmann::json schema = revocation_schema();
    return schema;
}

const nlohmann::json& list_node_revocations_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_node_revocations_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"revocations", {{"type", "array"}, {"items", revocation_schema()}}}},
        {"revocations"});
    return schema;
}

ApiResult handle_revoke_node(const nlohmann::json& input, ApiContext& context) {
    const Node::NodeRevocationResult revocation = context.node.revoke_node(
        input.at("node_id").get<std::string>(), input.value("reason", std::string{}),
        input.value("allow_rekey", false), context.acting_key,
        input.value("public_key_sha256", std::string{}));
    if (!revocation.ok) return ApiResult::failure("conflict", revocation.error);
    return ApiResult::success(revocation_json(revocation.revocation));
}

ApiResult handle_list_node_revocations(const nlohmann::json& /*input*/, ApiContext& context) {
    nlohmann::json revocations = nlohmann::json::array();
    for (const Node::NodeRevocationEntry& entry : context.node.node_revocations()) {
        revocations.push_back(revocation_json(entry));
    }
    return ApiResult::success({{"revocations", std::move(revocations)}});
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

    registry.add(Capability{
        "get_identity_epoch",
        "Get the identity epoch",
        "Read the identity epoch this node holds (#1360): its number, how many "
        "identity records it commits to, its digest, and the --identity-min-epoch "
        "floor. Record the number to start a joining node with "
        "--identity-min-epoch. Requires cluster:admin. No side effects.",
        get_identity_epoch_schema(),
        get_identity_epoch_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "cluster_admin"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_identity_epoch});

    registry.add(Capability{
        "revoke_node",
        "Revoke a cluster node",
        "Revoke one node's enrolled key and remove it from membership (#1320). "
        "The revocation is sealed by the identity root, so only a node holding "
        "the root can write it. Every node then refuses that key at the "
        "handshake and closes a live link presenting it; without allow_rekey the "
        "node id cannot come back under any key. Requires cluster:admin. "
        "Mutates replicated governance state.",
        revoke_node_schema(),
        revoke_node_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Api, "cluster_admin"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_revoke_node});

    registry.add(Capability{
        "list_node_revocations",
        "List node revocations",
        "Read every node revocation this node trusts (#1320): the revoked key, "
        "the node id, the certificate serial, who revoked it and when, and "
        "whether the id may re-key. Requires cluster:admin. No side effects.",
        list_node_revocations_schema(),
        list_node_revocations_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "cluster_admin"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_node_revocations});
}

}  // namespace zetta::api
