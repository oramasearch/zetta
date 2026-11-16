// SPDX-License-Identifier: MIT

#include "zetta/cloud/azure_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

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
    if (!key_vault.empty()) document["key_vault"] = key_vault;
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
        !read_string(value, "tenant_id", /*required=*/false, config.tenant_id) ||
        !read_string(value, "key_vault", /*required=*/false, config.key_vault)) {
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

const nlohmann::json& azure_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/azure_config.schema.json",
  "title": "AzureConfig",
  "description": "Configuration for the reference cloud-burst Provisioner backend (ARC-13 / ADR-20, #376): where on Azure to burst ephemeral compute nodes, how to size and price them, and how each node joins the cluster. Mirrored by zetta::AzureConfig in include/zetta/azure_provisioner.hpp and enforced by AzureConfig::from_json() in src/core/azure_provisioner.cpp — keep them in sync. No cloud SDK lives behind the provisioner; this config is interpreted by an AzureControlPlane (the reference AzCliControlPlane shells out to the `az` CLI). A malformed config is rejected at the boundary so a misconfigured burst fails fast instead of bringing up the wrong (or unbounded) fleet.",
  "type": "object",
  "required": ["subscription_id", "resource_group", "location"],
  "additionalProperties": false,
  "properties": {
    "subscription_id": {
      "description": "The Azure subscription the burst pool is created in.",
      "type": "string",
      "minLength": 1
    },
    "resource_group": {
      "description": "The resource group that owns the burst pool — the unit deleted on full teardown (stable name → idempotent create/delete).",
      "type": "string",
      "minLength": 1
    },
    "location": {
      "description": "The Azure region the nodes burst into (e.g. 'eastus', 'westeurope').",
      "type": "string",
      "minLength": 1
    },
    "pool_name": {
      "description": "Name of the burst pool (VMSS / Batch pool). Stable so create + delete are idempotent. Defaults to 'zetta-burst' when omitted.",
      "type": "string"
    },
    "vm_size": {
      "description": "The Azure VM SKU each node uses (e.g. 'Standard_NC24ads_A100_v4'). Empty lets the control plane map the requested NodeCapability spec to a SKU.",
      "type": "string"
    },
    "image": {
      "description": "The VM image reference burst nodes boot from (e.g. 'Ubuntu2204'). Empty uses the control plane's default image.",
      "type": "string"
    },
    "spot": {
      "description": "Burst Spot / low-priority capacity (cheaper, evictable) rather than regular on-demand nodes.",
      "type": "boolean"
    },
    "max_price": {
      "description": "Spot max price per node-hour (USD). Azure's convention: -1 pays up to the standard on-demand price (never evicted on price). Applies only when spot is true.",
      "type": "number",
      "minimum": -1
    },
    "seed_peer": {
      "description": "The cluster seed address (host:port) injected into each node's bootstrap so it auto-joins the existing peer cluster at boot. Empty when the join address is supplied another way.",
      "type": "string"
    },
    "managed_identity": {
      "description": "User-assigned managed identity resource id granted to burst nodes (the cloud-native side of the per-node burst identity). On a secure cluster each node reads the cluster join token from key_vault with it, so it needs the Key Vault Secrets User role (or a 'get' secret access policy) there; required then (#1316).",
      "type": "string"
    },
    "key_vault": {
      "description": "Name of the Azure Key Vault the cluster join token is stored in, as the secret '<pool_name>-cluster-token'. Each node's boot script fetches it with managed_identity, so the token never rides cloud-init custom-data. Required on a secure cluster (#1316).",
      "type": "string"
    },
    "tenant_id": {
      "description": "Optional Azure AD tenant id, when the credential needs it explicitly.",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud