#include "zetta/azure_provisioner.hpp"

#include <utility>

namespace zetta {

namespace {

// A string field on a JSON object: must be a string when present. `required`
// fields must be present and non-empty. Returns false (out untouched) on a
// missing-required / wrong-typed / empty-required field — the boundary check.
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

nlohmann::json AzureConfig::to_json() const {
    nlohmann::json document{
        {"subscription_id", subscription_id},
        {"resource_group", resource_group},
        {"location", location},
        {"spot", spot},
        {"max_price", max_price},
    };
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly.
    if (!pool_name.empty()) document["pool_name"] = pool_name;
    if (!vm_size.empty()) document["vm_size"] = vm_size;
    if (!image.empty()) document["image"] = image;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    if (!managed_identity.empty()) document["managed_identity"] = managed_identity;
    if (!tenant_id.empty()) document["tenant_id"] = tenant_id;
    return document;
}

std::optional<AzureConfig> AzureConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    AzureConfig config;
    if (!read_string(value, "subscription_id", /*required=*/true,
                     config.subscription_id) ||
        !read_string(value, "resource_group", /*required=*/true,
                     config.resource_group) ||
        !read_string(value, "location", /*required=*/true, config.location) ||
        !read_string(value, "pool_name", /*required=*/false, config.pool_name) ||
        !read_string(value, "vm_size", /*required=*/false, config.vm_size) ||
        !read_string(value, "image", /*required=*/false, config.image) ||
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer) ||
        !read_string(value, "managed_identity", /*required=*/false,
                     config.managed_identity) ||
        !read_string(value, "tenant_id", /*required=*/false, config.tenant_id)) {
        return std::nullopt;
    }

    if (value.contains("spot")) {
        if (!value["spot"].is_boolean()) return std::nullopt;
        config.spot = value["spot"].get<bool>();
    }
    if (value.contains("max_price")) {
        if (!value["max_price"].is_number()) return std::nullopt;
        config.max_price = value["max_price"].get<double>();
        if (config.max_price < -1.0) return std::nullopt;  // Azure floor: -1 = on-demand
    }
    return config;
}

AzureProvisioner::AzureProvisioner(AzureControlPlane& control_plane,
                                   AzureConfig config, IdentityHooks identity)
    : control_plane_(control_plane),
      config_(std::move(config)),
      identity_(std::move(identity)) {}

ProvisionResult AzureProvisioner::provision(const ProvisionRequest& request) {
    // A count of 0 is a satisfied no-op (the seam contract): touch neither the
    // control plane nor the identity hooks.
    if (request.count == 0) {
        ProvisionResult empty;
        empty.satisfied = true;
        empty.detail = "no nodes requested";
        return empty;
    }

    ProvisionResult result = control_plane_.bring_up(config_, request);

    // Mint bulk burst identities for the nodes that came up so they can
    // authenticate and join the cluster (#375). Best-effort backends may return
    // zero nodes (no capacity) — only mint when something actually came up.
    if (!result.nodes.empty() && identity_.mint) {
        identity_.mint(result.nodes);
    }
    return result;
}

bool AzureProvisioner::teardown(const std::string& node_id) {
    const bool released = control_plane_.destroy(config_, node_id);

    // Bulk-revoke the job's identities when this teardown drains the pool — the
    // "bulk revoke on teardown" half of the burst-identity lifecycle. Reading the
    // control plane (the source of truth) avoids a parallel copy of fleet state;
    // revoke is idempotent, so firing it exactly on the transition to empty is
    // safe even if a later teardown finds the pool already empty.
    if (released && identity_.revoke && control_plane_.list(config_).empty()) {
        identity_.revoke();
    }
    return released;
}

std::vector<ProvisionedNode> AzureProvisioner::provisioned() const {
    return control_plane_.list(config_);
}

}  // namespace zetta
