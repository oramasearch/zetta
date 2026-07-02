#include "zetta/cloud/aws_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

using detail::read_string;

nlohmann::json AwsConfig::to_json() const {
    nlohmann::json document{
        {"region", region},
        {"launch_template", launch_template},
        {"subnets", subnets},
        {"spot", spot},
        {"max_price", max_price},
    };
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly.
    if (!pool_name.empty()) document["pool_name"] = pool_name;
    if (!instance_type.empty()) document["instance_type"] = instance_type;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    if (!profile.empty()) document["profile"] = profile;
    return document;
}

std::optional<AwsConfig> AwsConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    AwsConfig config;
    if (!read_string(value, "region", /*required=*/true, config.region) ||
        !read_string(value, "launch_template", /*required=*/true,
                     config.launch_template) ||
        !read_string(value, "subnets", /*required=*/true, config.subnets) ||
        !read_string(value, "pool_name", /*required=*/false, config.pool_name) ||
        !read_string(value, "instance_type", /*required=*/false,
                     config.instance_type) ||
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer) ||
        !read_string(value, "profile", /*required=*/false, config.profile)) {
        return std::nullopt;
    }

    if (value.contains("spot")) {
        if (!value["spot"].is_boolean()) return std::nullopt;
        config.spot = value["spot"].get<bool>();
    }
    if (value.contains("max_price")) {
        if (!value["max_price"].is_number()) return std::nullopt;
        config.max_price = value["max_price"].get<double>();
        if (config.max_price < 0.0) return std::nullopt;  // AWS: 0 = up to on-demand
    }
    return config;
}

}  // namespace zetta::cloud