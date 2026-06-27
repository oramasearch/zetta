#include "zetta/azure_provisioner.hpp"

#include "json_boundary.hpp"

namespace zetta {

using detail::read_string;

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

}  // namespace zetta
