#include "zetta/k8s_provisioner.hpp"

#include <utility>

namespace zetta {

namespace {

// A string field on a JSON object: must be a string when present. `required`
// fields must be present and non-empty. Returns false (out untouched) on a
// missing-required / wrong-typed / empty-required field — the boundary check.
// (Same helper shape as oci_provisioner.cpp; kept local so the parser is
// self-contained.)
bool read_string(const nlohmann::json& object, const char* key, bool required,
                 std::string& out) {
    if (!object.contains(key)) return !required;
    const nlohmann::json& value = object[key];
    if (!value.is_string()) return false;
    std::string text = value.get<std::string>();
    if (required && text.empty()) return false;
    out = std::move(text);
    return true;
}

}  // namespace

nlohmann::json K8sConfig::to_json() const {
    nlohmann::json document{{"image", image}, {"spot", spot}};
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly. The JSON key is `namespace` (the C++ field is
    // namespace_name to avoid the keyword).
    if (!namespace_name.empty()) document["namespace"] = namespace_name;
    if (!deployment_name.empty()) document["deployment_name"] = deployment_name;
    if (!context.empty()) document["context"] = context;
    if (!service_account.empty()) document["service_account"] = service_account;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    return document;
}

std::optional<K8sConfig> K8sConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    K8sConfig config;
    if (!read_string(value, "image", /*required=*/true, config.image) ||
        !read_string(value, "namespace", /*required=*/false, config.namespace_name) ||
        !read_string(value, "deployment_name", /*required=*/false,
                     config.deployment_name) ||
        !read_string(value, "context", /*required=*/false, config.context) ||
        !read_string(value, "service_account", /*required=*/false,
                     config.service_account) ||
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer)) {
        return std::nullopt;
    }
    if (value.contains("spot")) {
        if (!value["spot"].is_boolean()) return std::nullopt;
        config.spot = value["spot"].get<bool>();
    }
    return config;
}

K8sProvisioner::K8sProvisioner(K8sControlPlane& control_plane, K8sConfig config,
                               IdentityHooks identity)
    : control_plane_(control_plane),
      config_(std::move(config)),
      identity_(std::move(identity)) {}

ProvisionResult K8sProvisioner::provision(const ProvisionRequest& request) {
    // A count of 0 is a satisfied no-op (the seam contract): touch neither the
    // control plane nor the identity hooks.
    if (request.count == 0) {
        ProvisionResult empty;
        empty.satisfied = true;
        empty.detail = "no nodes requested";
        return empty;
    }

    ProvisionResult result = control_plane_.bring_up(config_, request);

    // Mint bulk burst identities for the pods that came up so they can authenticate
    // and join the cluster (#375). Best-effort backends may return zero pods (no
    // capacity) — only mint when something actually came up.
    if (!result.nodes.empty() && identity_.mint) {
        identity_.mint(result.nodes);
    }
    return result;
}

bool K8sProvisioner::teardown(const std::string& node_id) {
    const bool released = control_plane_.destroy(config_, node_id);

    // Bulk-revoke the job's identities when this teardown drains the pool — the
    // "bulk revoke on teardown" half of the burst-identity lifecycle. Reading the
    // control plane (the source of truth) avoids a parallel copy of fleet state;
    // revoke is idempotent, so firing it exactly on the transition to empty is safe
    // even if a later teardown finds the pool already empty.
    if (released && identity_.revoke && control_plane_.list(config_).empty()) {
        identity_.revoke();
    }
    return released;
}

std::vector<ProvisionedNode> K8sProvisioner::provisioned() const {
    return control_plane_.list(config_);
}

}  // namespace zetta
