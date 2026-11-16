// SPDX-License-Identifier: MIT

#include "zetta/cloud/oci_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

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
    if (!vault_id.empty()) document["vault_id"] = vault_id;
    if (!key_id.empty()) document["key_id"] = key_id;
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
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer) ||
        !read_string(value, "vault_id", /*required=*/false, config.vault_id) ||
        !read_string(value, "key_id", /*required=*/false, config.key_id)) {
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

const nlohmann::json& oci_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/oci_config.schema.json",
  "title": "OciConfig",
  "description": "Configuration for the OCI (Oracle Cloud Infrastructure) cloud-burst Provisioner backend (ARC-13 / ADR-20, #432): where on OCI to burst ephemeral compute nodes, how to size and price them, and how each node joins the cluster. Mirrors the Azure reference (azure_config.schema.json) / GCP (gcp_config.schema.json) via the CLI pattern. Mirrored by zetta::OciConfig in include/zetta/oci_provisioner.hpp and enforced by OciConfig::from_json() in src/core/oci_provisioner.cpp — keep them in sync. No cloud SDK lives behind the provisioner; this config is interpreted by an OciControlPlane (the reference OciCliControlPlane shells out to the `oci` CLI, the Instance Pool + preemptible shape). A malformed config is rejected at the boundary so a misconfigured burst fails fast instead of bringing up the wrong (or no) fleet.",
  "type": "object",
  "required": ["compartment_id", "availability_domain", "subnet_id"],
  "additionalProperties": false,
  "properties": {
    "compartment_id": {
      "description": "The OCI compartment OCID the burst pool is created in.",
      "type": "string",
      "minLength": 1
    },
    "availability_domain": {
      "description": "The availability domain the instances run in (e.g. 'Uocm:PHX-AD-1').",
      "type": "string",
      "minLength": 1
    },
    "subnet_id": {
      "description": "The subnet OCID burst nodes attach their primary VNIC to.",
      "type": "string",
      "minLength": 1
    },
    "region": {
      "description": "The OCI region (e.g. 'us-ashburn-1'). Optional; omitted leaves the region to the CLI profile / config.",
      "type": "string"
    },
    "pool_name": {
      "description": "Display name of the burst pool (the Instance Pool). Stable so a re-burst resizes the existing pool (resolved by name) rather than creating a duplicate. Defaults to 'zetta-burst' when omitted.",
      "type": "string"
    },
    "shape": {
      "description": "The compute shape each node uses (e.g. 'VM.Standard.E5.Flex', 'VM.GPU.A10.1'). Defaults to 'VM.Standard.E5.Flex'.",
      "type": "string"
    },
    "ocpus": {
      "description": "OCPUs per node for a flexible shape (the shapeConfig). 0 omits the shapeConfig (for fixed shapes). Default 1.",
      "type": "integer",
      "minimum": 0
    },
    "memory_in_gbs": {
      "description": "Memory in GB per node for a flexible shape (the shapeConfig). 0 omits it. Default 16.",
      "type": "integer",
      "minimum": 0
    },
    "image_id": {
      "description": "The image OCID burst nodes boot from. Empty lets the control plane / instance configuration default apply (a production deployment supplies a current platform/custom image OCID).",
      "type": "string"
    },
    "spot": {
      "description": "Burst preemptible capacity — cheaper, interruptible — rather than on-demand nodes (the instance configuration's preemptibleInstanceConfig).",
      "type": "boolean"
    },
    "seed_peer": {
      "description": "The cluster seed address (host:port) injected into each node's cloud-init (user_data) so it auto-joins the existing peer cluster at boot. Empty when the join address is supplied another way.",
      "type": "string"
    },
    "vault_id": {
      "description": "The OCI Vault OCID the cluster join token is stored in, as the secret '<pool_name>-cluster-token'. Each node's boot script reads it with the instance principal, so the token never rides user_data. Required on a secure cluster (#1316).",
      "type": "string"
    },
    "key_id": {
      "description": "The OCID of the vault's master encryption key that encrypts the join-token secret. Required on a secure cluster (#1316).",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud