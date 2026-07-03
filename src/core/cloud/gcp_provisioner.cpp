#include "zetta/cloud/gcp_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

using detail::read_string;

nlohmann::json GcpConfig::to_json() const {
    nlohmann::json document{
        {"project_id", project_id},
        {"zone", zone},
        {"spot", spot},
    };
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly.
    if (!pool_name.empty()) document["pool_name"] = pool_name;
    if (!instance_template.empty()) document["instance_template"] = instance_template;
    if (!machine_type.empty()) document["machine_type"] = machine_type;
    if (!image.empty()) document["image"] = image;
    if (!network.empty()) document["network"] = network;
    if (!subnetwork.empty()) document["subnetwork"] = subnetwork;
    if (!service_account.empty()) document["service_account"] = service_account;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    return document;
}

std::optional<GcpConfig> GcpConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    GcpConfig config;
    if (!read_string(value, "project_id", /*required=*/true, config.project_id) ||
        !read_string(value, "zone", /*required=*/true, config.zone) ||
        !read_string(value, "pool_name", /*required=*/false, config.pool_name) ||
        !read_string(value, "instance_template", /*required=*/false,
                     config.instance_template) ||
        !read_string(value, "machine_type", /*required=*/false, config.machine_type) ||
        !read_string(value, "image", /*required=*/false, config.image) ||
        !read_string(value, "network", /*required=*/false, config.network) ||
        !read_string(value, "subnetwork", /*required=*/false, config.subnetwork) ||
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

}  // namespace zetta::cloud