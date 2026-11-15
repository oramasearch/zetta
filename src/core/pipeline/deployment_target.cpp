#include "zetta/pipeline/deployment_target.hpp"

#include <array>

#include "zetta/connect/connection_store.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/deployment_credentials.hpp"

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
    const nlohmann::json& value, std::string* error) {
    const auto refuse = [error](std::string reason) -> std::optional<DeploymentTarget> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object() || !value.contains("kind") ||
        !value["kind"].is_string()) {
        return refuse("target: expected an object with a string kind");
    }
    const std::optional<DeploymentKind> kind =
        deployment_kind_from_name(value["kind"].get<std::string>());
    if (!kind) return refuse("target.kind: unknown placement kind");
    if (value.contains("binding") && !value["binding"].is_string()) {
        return refuse("target.binding: expected a string");
    }
    if (value.contains("credentials_ref") && !value["credentials_ref"].is_string()) {
        return refuse("target.credentials_ref: expected a string");
    }
    DeploymentTarget target;
    target.kind = *kind;
    target.binding = value.value("binding", std::string{});
    target.credentials_ref = value.value("credentials_ref", std::string{});
    // A reference, never the secret (#1315): it must be a saved-connection
    // name. The reason names the rule, never the value, which may be a secret.
    if (!target.credentials_ref.empty()) {
        if (const std::string rejection =
                connect::connection_name_rejection(target.credentials_ref);
            !rejection.empty()) {
            return refuse("target.credentials_ref must name a saved connection, not "
                          "hold a credential: " + rejection);
        }
    }
    return target;
}

DeploymentCredentials resolve_deployment_credentials(Node& node,
                                                     const DeploymentTarget& target,
                                                     const RunPrincipal& principal) {
    DeploymentCredentials result;
    const std::string& name = target.credentials_ref;
    if (name.empty()) return result;  // nothing to present
    // The boundary check again: a target built in code skips from_json.
    if (const std::string rejection = connect::connection_name_rejection(name);
        !rejection.empty()) {
        result.error = "credentials_ref must name a saved connection: " + rejection;
        return result;
    }
    std::optional<connect::SavedConnection> saved = connect::find_connection(node, name);
    if (!saved) {
        result.error = "credentials_ref '" + name + "' names no saved connection";
        return result;
    }
    // Knowing a connection's name is not being able to use it (#1339).
    if (!connect::may_access_connection(node, *saved, principal.acting_id,
                                        principal.acting_is_user,
                                        connect::ConnectionAccess::Use)) {
        result.error = "credentials_ref '" + name +
                       "': no connections:use grant on it (GRANT connections:use ON "
                       "CONNECTION " + name + "), and the deploying principal did not "
                       "create it";
        result.unauthorized = true;
        zetta::governance::AuditEventValue event;
        event.action = zetta::governance::AuditAction::DataAccess;
        event.actor = principal.local() ? std::string("local") : principal.acting_id;
        event.target = "connection:" + name;
        event.allowed = false;
        event.detail = "deploy: " + result.error;
        node.record_audit_event(std::move(event));
        return result;
    }
    switch (saved->credential) {
        case connect::CredentialState::Set:
        case connect::CredentialState::Plaintext:  // opened; resealed at start
            break;
        case connect::CredentialState::None:
            result.error = "credentials_ref '" + name +
                           "' names a saved connection that holds no credential";
            return result;
        case connect::CredentialState::Unreadable:
            result.error = "credentials_ref '" + name +
                           "': its credential does not open on this node (" +
                           saved->credential_error + ")";
            return result;
    }
    result.connection = std::move(saved);
    return result;
}

}  // namespace zetta::pipeline