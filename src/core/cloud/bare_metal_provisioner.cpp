// SPDX-License-Identifier: MIT

#include "zetta/cloud/bare_metal_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

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

const nlohmann::json& bare_metal_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/bare_metal_config.schema.json",
  "title": "BareMetalConfig",
  "description": "Configuration for the bare-metal cloud-burst Provisioner backend (ARC-13 / ADR-20, #436): how to burst ephemeral worker nodes on the local host. Mirrors the Azure reference (azure_config.schema.json) / GCP / OCI / Kubernetes via the CLI pattern. Mirrored by zetta::BareMetalConfig in include/zetta/bare_metal_provisioner.hpp and enforced by BareMetalConfig::from_json() in src/core/bare_metal_provisioner.cpp — keep them in sync. No container SDK lives behind the provisioner; this config is interpreted by a BareMetalControlPlane (the reference BareMetalCliControlPlane shells out to the host container runtime — `docker` or `podman` — running each worker as a local container, the cross-OS synchronous-launch mechanism). Bare metal is the local host, not a remote cloud/cluster. A malformed config is rejected at the boundary so a misconfigured burst fails fast instead of bringing up the wrong (or no) fleet.",
  "type": "object",
  "required": ["image"],
  "additionalProperties": false,
  "properties": {
    "image": {
      "description": "The container image each worker runs (e.g. 'ghcr.io/oramasearch/zetta:latest'). Required — a burst has nothing to run without it.",
      "type": "string",
      "minLength": 1
    },
    "runtime": {
      "description": "The host container runtime CLI to drive — 'docker' or 'podman'. Defaults to 'docker'.",
      "type": "string"
    },
    "pool_label": {
      "description": "The container label applied to every burst worker so list + teardown can find the pool (the stable pool key — like a scale-set name). Defaults to 'zetta-burst'.",
      "type": "string"
    },
    "name_prefix": {
      "description": "The container name prefix for burst workers (a UUID is appended for uniqueness). Defaults to 'zetta-burst'.",
      "type": "string"
    },
    "network": {
      "description": "The container network to attach workers to (e.g. 'host' so a worker can reach the cluster on the host network). Empty uses the runtime's default network.",
      "type": "string"
    },
    "gpus": {
      "description": "Give burst workers access to the host's GPUs (the runtime's `--gpus all`). Defaults to false (CPU-only).",
      "type": "boolean"
    },
    "seed_peer": {
      "description": "The cluster seed address (host:port) injected as the ZETTA_SEED_PEER env var so each worker auto-joins the existing peer cluster at boot. Empty when the join address is supplied another way.",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud