#include "zetta/cloud/k8s_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta {

using detail::read_string;

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

}  // namespace zetta
