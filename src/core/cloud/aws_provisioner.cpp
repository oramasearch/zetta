#include "zetta/cloud/aws_provisioner.hpp"

#include "../json_boundary.hpp"

namespace zetta::cloud {

using detail::read_string;

nlohmann::json AwsConfig::to_json() const {
    nlohmann::json document{
        {"region", region},
        {"launch_template", launch_template},
        {"subnets", subnets},
        {"spot", spot},
        {"max_price", max_price},
    };
    // Optional fields ride along only when set, so a round-trip is stable and a
    // minimal config reads cleanly.
    if (!pool_name.empty()) document["pool_name"] = pool_name;
    if (!instance_type.empty()) document["instance_type"] = instance_type;
    if (!seed_peer.empty()) document["seed_peer"] = seed_peer;
    if (!profile.empty()) document["profile"] = profile;
    return document;
}

std::optional<AwsConfig> AwsConfig::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    AwsConfig config;
    if (!read_string(value, "region", /*required=*/true, config.region) ||
        !read_string(value, "launch_template", /*required=*/true,
                     config.launch_template) ||
        !read_string(value, "subnets", /*required=*/true, config.subnets) ||
        !read_string(value, "pool_name", /*required=*/false, config.pool_name) ||
        !read_string(value, "instance_type", /*required=*/false,
                     config.instance_type) ||
        !read_string(value, "seed_peer", /*required=*/false, config.seed_peer) ||
        !read_string(value, "profile", /*required=*/false, config.profile)) {
        return std::nullopt;
    }

    if (value.contains("spot")) {
        if (!value["spot"].is_boolean()) return std::nullopt;
        config.spot = value["spot"].get<bool>();
    }
    if (value.contains("max_price")) {
        if (!value["max_price"].is_number()) return std::nullopt;
        config.max_price = value["max_price"].get<double>();
        if (config.max_price < 0.0) return std::nullopt;  // AWS: 0 = up to on-demand
    }
    return config;
}

const nlohmann::json& aws_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/aws_config.schema.json",
  "title": "AwsConfig",
  "description": "Configuration for the AWS cloud-burst Provisioner backend (ARC-13 / ADR-20, #377), the AWS sibling of the reference AzureConfig: where on AWS to burst ephemeral compute nodes, how to size and price them, and how each node joins the cluster. Mirrored by zetta::AwsConfig in include/zetta/aws_provisioner.hpp and enforced by AwsConfig::from_json() in src/core/aws_provisioner.cpp — keep them in sync. No cloud SDK lives behind the provisioner; this config is interpreted by an AwsControlPlane (the reference AwsCliControlPlane shells out to the `aws` CLI, driving an EC2 Auto Scaling Group). A malformed config is rejected at the boundary so a misconfigured burst fails fast instead of bringing up the wrong (or unbounded) fleet.",
  "type": "object",
  "required": ["region", "launch_template", "subnets"],
  "additionalProperties": false,
  "properties": {
    "region": {
      "description": "The AWS region the burst nodes launch into (e.g. 'us-west-2', 'eu-west-1').",
      "type": "string",
      "minLength": 1
    },
    "launch_template": {
      "description": "The EC2 launch template name the Auto Scaling Group launches instances from — it defines the AMI, the default instance type, the IAM instance profile (the per-node AWS identity), and the user-data (the cluster-join bootstrap).",
      "type": "string",
      "minLength": 1
    },
    "subnets": {
      "description": "Comma-separated VPC subnet ids the burst nodes launch into (the ASG's --vpc-zone-identifier).",
      "type": "string",
      "minLength": 1
    },
    "pool_name": {
      "description": "Name of the burst pool (the EC2 Auto Scaling Group). Stable so create + scale + delete are idempotent. Defaults to 'zetta-burst' when omitted.",
      "type": "string"
    },
    "instance_type": {
      "description": "An EC2 instance type override (e.g. 'g5.xlarge', 'p4d.24xlarge') applied via the ASG's mixed-instances policy. Empty uses the launch template's instance type.",
      "type": "string"
    },
    "spot": {
      "description": "Burst Spot capacity (cheaper, interruptible) rather than On-Demand, via the ASG's mixed-instances policy.",
      "type": "boolean"
    },
    "max_price": {
      "description": "Spot max price per node-hour (USD). AWS convention: 0 (or omitted) places no explicit cap, so Spot is capped at the On-Demand price. Applies only when spot is true.",
      "type": "number",
      "minimum": 0
    },
    "seed_peer": {
      "description": "The cluster seed address (host:port) handed to each node's bootstrap so it auto-joins the existing peer cluster at boot. Empty when the join address is supplied another way (e.g. baked into the launch template's user-data).",
      "type": "string"
    },
    "profile": {
      "description": "Optional AWS CLI named profile (which credentials/config the `aws` commands run under). Empty uses the default profile / ambient credentials.",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud