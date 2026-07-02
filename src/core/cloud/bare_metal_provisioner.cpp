#include "zetta/cloud/bare_metal_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta {

using detail::read_string;

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

}  // namespace zetta
