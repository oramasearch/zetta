#include "zetta/oci_provisioner.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace zetta {

namespace {

// A string field on a JSON object: must be a string when present. `required`
// fields must be present and non-empty. Returns false (out untouched) on a
// missing-required / wrong-typed / empty-required field — the boundary check.
// (Same helper shape as gcp_provisioner.cpp; kept local so the parser is
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

// An optional non-negative uint32 field: absent leaves `out` untouched. Accepts a
// JSON integer whether stored signed or unsigned; rejects a non-integer, a negative
// value, or one above the uint32 range — the boundary check for a count/size field.
bool read_uint32(const nlohmann::json& object, const char* key,
                 std::uint32_t& out) {
    if (!object.contains(key)) return true;  // absent is fine (keep the default)
    const nlohmann::json& value = object[key];
    std::int64_t signed_value = 0;
    if (value.is_number_unsigned()) {
        const std::uint64_t raw = value.get<std::uint64_t>();
        if (raw > std::numeric_limits<std::uint32_t>::max()) return false;
        out = static_cast<std::uint32_t>(raw);
        return true;
    }
    if (value.is_number_integer()) {
        signed_value = value.get<std::int64_t>();
        if (signed_value < 0) return false;
        if (signed_value > std::numeric_limits<std::uint32_t>::max()) return false;
        out = static_cast<std::uint32_t>(signed_value);
        return true;
    }
    return false;  // not an integer
}

}  // namespace

nlohmann::json OciConfig::to_json() const {
    nlohmann::json document{
        {"compartment_id", compartment_id},
        {"availability_domain", availability_domain},
        {"subnet_id", subnet_id},
        {"ocpus", ocpus},
        {"memory_in_gbs", memory_in_gbs},
        {"spot", spot},
    };
    // Optional string fields ride along only when set, so a round-trip is stable
    // and a minimal config reads cleanly.
    if (!region.empty()) document["region"] = region;
    if (!pool_name.empty()) document["pool_name"] = pool_name;
    if (!shape.empty()) document["shape"] = shape;
    if (!image_id.empty()) document["image_id"] = image_id;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    return document;
}

std::optional<OciConfig> OciConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    OciConfig config;
    if (!read_string(value, "compartment_id", /*required=*/true,
                     config.compartment_id) ||
        !read_string(value, "availability_domain", /*required=*/true,
                     config.availability_domain) ||
        !read_string(value, "subnet_id", /*required=*/true, config.subnet_id) ||
        !read_string(value, "region", /*required=*/false, config.region) ||
        !read_string(value, "pool_name", /*required=*/false, config.pool_name) ||
        !read_string(value, "shape", /*required=*/false, config.shape) ||
        !read_string(value, "image_id", /*required=*/false, config.image_id) ||
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer)) {
        return std::nullopt;
    }
    if (!read_uint32(value, "ocpus", config.ocpus) ||
        !read_uint32(value, "memory_in_gbs", config.memory_in_gbs)) {
        return std::nullopt;
    }
    if (value.contains("spot")) {
        if (!value["spot"].is_boolean()) return std::nullopt;
        config.spot = value["spot"].get<bool>();
    }
    return config;
}

OciProvisioner::OciProvisioner(OciControlPlane& control_plane, OciConfig config,
                               IdentityHooks identity)
    : control_plane_(control_plane),
      config_(std::move(config)),
      identity_(std::move(identity)) {}

ProvisionResult OciProvisioner::provision(const ProvisionRequest& request) {
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

bool OciProvisioner::teardown(const std::string& node_id) {
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

std::vector<ProvisionedNode> OciProvisioner::provisioned() const {
    return control_plane_.list(config_);
}

}  // namespace zetta
