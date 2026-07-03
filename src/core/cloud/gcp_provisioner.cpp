#include "zetta/cloud/gcp_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

using detail::read_string;

nlohmann::json GcpConfig::to_json() const {
    nlohmann::json document{
        {"project_id", project_id},
        {"zone", zone},
        {"spot", spot},
    };
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly.
    if (!pool_name.empty()) document["pool_name"] = pool_name;
    if (!instance_template.empty()) document["instance_template"] = instance_template;
    if (!machine_type.empty()) document["machine_type"] = machine_type;
    if (!image.empty()) document["image"] = image;
    if (!network.empty()) document["network"] = network;
    if (!subnetwork.empty()) document["subnetwork"] = subnetwork;
    if (!service_account.empty()) document["service_account"] = service_account;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    return document;
}

std::optional<GcpConfig> GcpConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    GcpConfig config;
    if (!read_string(value, "project_id", /*required=*/true, config.project_id) ||
        !read_string(value, "zone", /*required=*/true, config.zone) ||
        !read_string(value, "pool_name", /*required=*/false, config.pool_name) ||
        !read_string(value, "instance_template", /*required=*/false,
                     config.instance_template) ||
        !read_string(value, "machine_type", /*required=*/false, config.machine_type) ||
        !read_string(value, "image", /*required=*/false, config.image) ||
        !read_string(value, "network", /*required=*/false, config.network) ||
        !read_string(value, "subnetwork", /*required=*/false, config.subnetwork) ||
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

const nlohmann::json& gcp_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/gcp_config.schema.json",
  "title": "GcpConfig",
  "description": "Configuration for the GCP cloud-burst Provisioner backend (ARC-13 / ADR-20, #378): where on Google Cloud to burst ephemeral compute nodes, how to size and price them, and how each node joins the cluster. Mirrors the Azure reference (azure_config.schema.json) via the CLI pattern. Mirrored by zetta::GcpConfig in include/zetta/gcp_provisioner.hpp and enforced by GcpConfig::from_json() in src/core/gcp_provisioner.cpp — keep them in sync. No cloud SDK lives behind the provisioner; this config is interpreted by a GcpControlPlane (the reference GcpCliControlPlane shells out to the `gcloud` CLI, the Managed Instance Group + Spot shape). A malformed config is rejected at the boundary so a misconfigured burst fails fast instead of bringing up the wrong (or no) fleet.",
  "type": "object",
  "required": ["project_id", "zone"],
  "additionalProperties": false,
  "properties": {
    "project_id": {
      "description": "The GCP project the burst pool is created in.",
      "type": "string",
      "minLength": 1
    },
    "zone": {
      "description": "The Compute Engine zone the Managed Instance Group runs in (e.g. 'us-central1-a').",
      "type": "string",
      "minLength": 1
    },
    "pool_name": {
      "description": "Name of the burst pool (the Managed Instance Group). Stable so create + delete are idempotent. Defaults to 'zetta-burst' when omitted.",
      "type": "string"
    },
    "instance_template": {
      "description": "The Compute Engine instance template the MIG instantiates. Defaults to 'zetta-burst-template'; the control plane ensures it exists (machine type / image / network / service account / Spot / startup-script) before creating the MIG.",
      "type": "string"
    },
    "machine_type": {
      "description": "The Compute Engine machine type each node uses (e.g. 'a2-highgpu-1g', 'e2-standard-4'). Empty lets the control plane map the requested NodeCapability spec to a machine type.",
      "type": "string"
    },
    "image": {
      "description": "The image family burst nodes boot from (e.g. 'debian-12'). Empty uses the control plane's default image.",
      "type": "string"
    },
    "network": {
      "description": "The VPC network burst nodes attach to (e.g. 'default'). Empty uses the project default.",
      "type": "string"
    },
    "subnetwork": {
      "description": "The subnetwork within the region burst nodes attach to. Empty uses the auto subnet for the network.",
      "type": "string"
    },
    "service_account": {
      "description": "The service account email granted to burst nodes (the cloud-native side of the per-node burst identity).",
      "type": "string"
    },
    "spot": {
      "description": "Burst Spot (preemptible) capacity — cheaper, interruptible — rather than standard on-demand nodes (gcloud --provisioning-model=SPOT).",
      "type": "boolean"
    },
    "seed_peer": {
      "description": "The cluster seed address (host:port) injected into each node's startup-script metadata so it auto-joins the existing peer cluster at boot. Empty when the join address is supplied another way.",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud