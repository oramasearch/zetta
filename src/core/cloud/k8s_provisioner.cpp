// SPDX-License-Identifier: MIT

#include "zetta/cloud/k8s_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

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

const nlohmann::json& k8s_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/k8s_config.schema.json",
  "title": "K8sConfig",
  "description": "Configuration for the Kubernetes cloud-burst Provisioner backend (ARC-13 / ADR-20, #434): where in a Kubernetes cluster to burst ephemeral worker pods, what image they run, and how each joins the cluster. Mirrors the Azure reference (azure_config.schema.json) / GCP / OCI via the CLI pattern. Mirrored by zetta::K8sConfig in include/zetta/k8s_provisioner.hpp and enforced by K8sConfig::from_json() in src/core/k8s_provisioner.cpp — keep them in sync. No Kubernetes client library lives behind the provisioner; this config is interpreted by a K8sControlPlane (the reference K8sCliControlPlane shells out to the `kubectl` CLI, the Deployment + scale shape). Unlike the cloud backends, the cluster already exists — it is the kubeconfig context, configured out-of-band. A malformed config is rejected at the boundary so a misconfigured burst fails fast instead of bringing up the wrong (or no) fleet.",
  "type": "object",
  "required": ["image"],
  "additionalProperties": false,
  "properties": {
    "image": {
      "description": "The container image the burst worker pods run (e.g. 'ghcr.io/oramasearch/zetta:latest'). Required — a burst has nothing to run without it.",
      "type": "string",
      "minLength": 1
    },
    "namespace": {
      "description": "The Kubernetes namespace the burst Deployment + pods live in. Defaults to 'default' when omitted.",
      "type": "string"
    },
    "deployment_name": {
      "description": "Name of the burst Deployment (the pool). Stable so create + scale are idempotent. Defaults to 'zetta-burst' when omitted.",
      "type": "string"
    },
    "context": {
      "description": "The kubeconfig context (cluster + user) to target. Empty uses the current context. The cluster itself is provisioned out-of-band (e.g. eks/gke/oke get-credentials).",
      "type": "string"
    },
    "service_account": {
      "description": "The Kubernetes ServiceAccount the burst pods run as (their in-cluster identity, distinct from the zetta-side Principal/AccessKey the BurstIdentityManager mints). Empty uses the namespace default.",
      "type": "string"
    },
    "spot": {
      "description": "Schedule the burst pods onto spot/preemptible nodes (a nodeSelector). Defaults to false (any node).",
      "type": "boolean"
    },
    "seed_peer": {
      "description": "The cluster seed address (host:port) injected as the ZETTA_SEED_PEER env var so each pod auto-joins the existing peer cluster at boot. Empty when the join address is supplied another way.",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud