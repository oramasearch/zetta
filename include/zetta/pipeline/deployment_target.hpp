#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::pipeline {

// Where a deployment is placed — the "placement dial" (ENT-6 / FEAT-4, ADR-7).
// Declaration order is the dial's local→cloud progression. R1 ships **Local**
// only; Kubernetes / Vpc / Cloud are named here so every layer refers to the
// same taxonomy, but they are not yet functional (their bindings + provisioning
// land in later slices — like ClusterRole, which declares roles before they are
// built).
enum class DeploymentKind {
    Local,       // this node / this machine (the only functional kind in R1)
    Kubernetes,  // a customer-managed Kubernetes cluster (planned)
    Vpc,         // a customer VPC / private cloud (planned)
    Cloud,       // a managed cloud (AWS / Azure / GCP) (planned)
};

// The lowercase canonical name of a placement kind ("local", "kubernetes", …).
// A naming/display helper (labels, query output) — not graph serialization.
std::string to_string(DeploymentKind kind);

// Parse a canonical placement-kind name back to the enum; std::nullopt if
// unknown. A naming helper for building targets from named kinds.
std::optional<DeploymentKind> deployment_kind_from_name(std::string_view name);

// A placement the orchestrator deploys onto (ENT-6). It carries the Cloud
// Interface **binding** (which concrete cluster/host/account to target) and a
// **credentials reference** (a key into stored credentials — never the secret
// itself). For a Local target both are empty: the binding is implicitly this
// node and no credentials are needed. k8s/cloud bindings fill in later slices.
// `credentials_ref` names a saved connection (#1315); the deploy path resolves
// it through pipeline/deployment_credentials.hpp.
struct DeploymentTarget {
    DeploymentKind kind = DeploymentKind::Local;
    std::string binding;          // Cloud Interface binding; empty = this node
    std::string credentials_ref;  // credentials key; empty for Local

    // The local node — the only target R1 can actually place onto.
    static DeploymentTarget local() { return DeploymentTarget{}; }

    bool is_local() const { return kind == DeploymentKind::Local; }

    // The replicated/on-disk value shape: { kind, binding, credentials_ref }.
    nlohmann::json to_json() const;

    // Parse a target from its JSON shape, **validating at the boundary**:
    // std::nullopt unless `value` is an object whose `kind` is a known name
    // and whose `credentials_ref`, when present and not empty, is a valid
    // saved-connection name (connect::connection_name_rejection, #1315), so a
    // secret pasted into it is refused rather than replicated. Missing binding
    // / credentials_ref default to empty (a Local target round-trips
    // minimally). `error`, when given, says why a value was refused.
    static std::optional<DeploymentTarget> from_json(const nlohmann::json& value,
                                                     std::string* error = nullptr);
};

}  // namespace zetta::pipeline