#include "zetta/bare_metal_provisioner.hpp"

#include <utility>

namespace zetta {

namespace {

// A string field on a JSON object: must be a string when present. `required`
// fields must be present and non-empty. Returns false (out untouched) on a
// missing-required / wrong-typed / empty-required field — the boundary check.
// (Same helper shape as k8s_provisioner.cpp; kept local so the parser is
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

nlohmann::json BareMetalConfig::to_json() const {
    nlohmann::json document{{"image", image}, {"gpus", gpus}};
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly.
    if (!runtime.empty()) document["runtime"] = runtime;
    if (!pool_label.empty()) document["pool_label"] = pool_label;
    if (!name_prefix.empty()) document["name_prefix"] = name_prefix;
    if (!network.empty()) document["network"] = network;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    return document;
}

std::optional<BareMetalConfig> BareMetalConfig::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    BareMetalConfig config;
    if (!read_string(value, "image", /*required=*/true, config.image) ||
        !read_string(value, "runtime", /*required=*/false, config.runtime) ||
        !read_string(value, "pool_label", /*required=*/false, config.pool_label) ||
        !read_string(value, "name_prefix", /*required=*/false, config.name_prefix) ||
        !read_string(value, "network", /*required=*/false, config.network) ||
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer)) {
        return std::nullopt;
    }
    if (value.contains("gpus")) {
        if (!value["gpus"].is_boolean()) return std::nullopt;
        config.gpus = value["gpus"].get<bool>();
    }
    return config;
}

BareMetalProvisioner::BareMetalProvisioner(BareMetalControlPlane& control_plane,
                                           BareMetalConfig config,
                                           IdentityHooks identity)
    : control_plane_(control_plane),
      config_(std::move(config)),
      identity_(std::move(identity)) {}

ProvisionResult BareMetalProvisioner::provision(const ProvisionRequest& request) {
    // A count of 0 is a satisfied no-op (the seam contract): touch neither the
    // control plane nor the identity hooks.
    if (request.count == 0) {
        ProvisionResult empty;
        empty.satisfied = true;
        empty.detail = "no nodes requested";
        return empty;
    }

    ProvisionResult result = control_plane_.bring_up(config_, request);

    // Mint bulk burst identities for the workers that came up so they can
    // authenticate and join the cluster (#375). Best-effort backends may return zero
    // workers (no capacity) — only mint when something actually came up.
    if (!result.nodes.empty() && identity_.mint) {
        identity_.mint(result.nodes);
    }
    return result;
}

bool BareMetalProvisioner::teardown(const std::string& node_id) {
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

std::vector<ProvisionedNode> BareMetalProvisioner::provisioned() const {
    return control_plane_.list(config_);
}

}  // namespace zetta
