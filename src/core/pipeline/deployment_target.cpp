// SPDX-License-Identifier: MIT

#include "zetta/pipeline/deployment_target.hpp"

#include <array>

namespace zetta::pipeline {

std::string to_string(DeploymentKind kind) {
    switch (kind) {
        case DeploymentKind::Local:      return "local";
        case DeploymentKind::Kubernetes: return "kubernetes";
        case DeploymentKind::Vpc:        return "vpc";
        case DeploymentKind::Cloud:      return "cloud";
    }
    return "unknown";
}

std::optional<DeploymentKind> deployment_kind_from_name(std::string_view name) {
    constexpr std::array<DeploymentKind, 4> kinds{
        DeploymentKind::Local, DeploymentKind::Kubernetes, DeploymentKind::Vpc,
        DeploymentKind::Cloud};
    for (const DeploymentKind kind : kinds) {
        if (to_string(kind) == name) return kind;
    }
    return std::nullopt;
}

nlohmann::json DeploymentTarget::to_json() const {
    return nlohmann::json{{"kind", to_string(kind)},
                          {"binding", binding},
                          {"credentials_ref", credentials_ref}};
}

std::optional<DeploymentTarget> DeploymentTarget::from_json(
    const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("kind") ||
        !value["kind"].is_string()) {
        return std::nullopt;
    }
    const std::optional<DeploymentKind> kind =
        deployment_kind_from_name(value["kind"].get<std::string>());
    if (!kind) return std::nullopt;
    DeploymentTarget target;
    target.kind = *kind;
    target.binding = value.value("binding", std::string{});
    target.credentials_ref = value.value("credentials_ref", std::string{});
    return target;
}

}  // namespace zetta::pipeline