#include "zetta/oci_provisioner.hpp"

#include "json_boundary.hpp"

namespace zetta {

using detail::read_string;
using detail::read_uint32;

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

}  // namespace zetta
