#include "zetta/cloud/aws_cli_control_plane.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/command_runner.hpp"
#include "zetta/cloud/secret_file.hpp"

namespace zetta::cloud {

namespace {

// Append a flag and its value to an argv vector (kept terse — the binding builds
// several of these). No shell is involved: argv is exec'd directly.
void flag(std::vector<std::string>& argv, std::string name, std::string value) {
    argv.push_back(std::move(name));
    argv.push_back(std::move(value));
}

// The flags every `aws` call shares: which region, and the named profile when
// one is configured. (The Auto Scaling Group name is per-command — create/scale
// name it directly, describe filters on it — so it is not appended here.)
void append_global(std::vector<std::string>& argv, const AwsConfig& config) {
    flag(argv, "--region", config.region);
    if (!config.profile.empty()) flag(argv, "--profile", config.profile);
}

}  // namespace

std::string cluster_token_parameter_name(const AwsConfig& config) {
    return "/zetta/" + config.pool_name + "/cluster-token";
}

namespace {

// Build the Auto Scaling Group mixed-instances policy that expresses an instance
// type override and/or Spot capacity — the AWS way to ask an ASG for Spot (and to
// right-size the SKU) rather than a single create flag. Illustrative shorthand;
// a production binding tunes the allocation strategy and instance pools.
std::string mixed_instances_policy(const AwsConfig& config) {
    std::string policy = "LaunchTemplate={LaunchTemplateSpecification={LaunchTemplateName=" +
                         config.launch_template + "}";
    if (!config.instance_type.empty()) {
        policy += ",Overrides=[{InstanceType=" + config.instance_type + "}]";
    }
    policy += "}";
    if (config.spot) {
        policy +=
            ",InstancesDistribution={OnDemandPercentageAboveBaseCapacity=0,"
            "SpotAllocationStrategy=capacity-optimized";
        if (config.max_price > 0.0) {
            policy += ",SpotMaxPrice=" + std::to_string(config.max_price);
        }
        policy += "}";
    }
    return policy;
}

// Parse an `aws ... --output json` array of ProvisionedNode-shaped objects,
// skipping anything malformed at the validated boundary. A production binding
// projects `aws ec2 describe-instances` output into this shape (instance →
// { node_id, address, capability }, correlating the capability from instance tags
// the node reports on join); the reference binding consumes that shape directly so
// its parsing stays aligned with ProvisionedNode::from_json.
std::vector<ProvisionedNode> parse_nodes(const std::string& json_text) {
    std::vector<ProvisionedNode> nodes;
    const nlohmann::json document =
        nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) return nodes;
    for (const nlohmann::json& element : document) {
        if (std::optional<ProvisionedNode> node = ProvisionedNode::from_json(element)) {
            nodes.push_back(std::move(*node));
        }
    }
    return nodes;
}

}  // namespace

AwsCliControlPlane::AwsCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult AwsCliControlPlane::bring_up(const AwsConfig& config,
                                             const ProvisionRequest& request) {
    ProvisionResult result;

    // Create the Auto Scaling Group at the requested desired capacity. A launch
    // template defines the AMI / instance type / IAM instance profile / user-data
    // (the cluster-join bootstrap); the ASG spans the configured subnets.
    std::vector<std::string> create{"aws", "autoscaling",
                                    "create-auto-scaling-group"};
    flag(create, "--auto-scaling-group-name", config.pool_name);
    flag(create, "--launch-template",
         "LaunchTemplateName=" + config.launch_template);
    flag(create, "--min-size", "0");
    flag(create, "--max-size", std::to_string(request.count));
    flag(create, "--desired-capacity", std::to_string(request.count));
    flag(create, "--vpc-zone-identifier", config.subnets);
    // Spot capacity and/or an instance-type override ride on a mixed-instances
    // policy (the AWS way to ask an ASG for Spot).
    if (config.spot || !config.instance_type.empty()) {
        flag(create, "--mixed-instances-policy", mixed_instances_policy(config));
    }
    // Hand each node the cluster seed address — and, on a secure cluster, how to
    // fetch the join token (#670, #770) — so it auto-joins at boot with the
    // equivalent of `zetta --peer <seed> --join-token <token>`. The seed rides a
    // propagating tag. The token never does: a tag is readable by any principal
    // holding ec2:DescribeTags and lands in CloudTrail. It is stored as an SSM
    // Parameter Store SecureString (KMS-encrypted, read only by a principal
    // granted ssm:GetParameter on it — the node's instance profile), and the tag
    // carries only the parameter's name. The value reaches `aws` through a
    // private file (`file://`), never through argv.
    std::string tags;
    if (!config.seed_peer.empty()) {
        tags = "Key=ZETTA_SEED_PEER,Value=" + config.seed_peer +
               ",PropagateAtLaunch=true";
    }
    // The cluster's identity root public key (#1343): not a secret, so it rides a
    // propagating tag beside the seed, and the node pins the cluster's root
    // instead of making its own.
    if (!config.identity_root.empty()) {
        if (!tags.empty()) tags += ' ';
        tags += "Key=ZETTA_IDENTITY_ROOT,Value=" + config.identity_root +
                ",PropagateAtLaunch=true";
    }
    if (!config.join_token.empty()) {
        const std::string parameter = cluster_token_parameter_name(config);
        std::string reason;
        const std::unique_ptr<SecretFile> token_file =
            SecretFile::create(config.join_token, &reason);
        if (!token_file) {
            result.satisfied = false;
            result.detail = "cannot stage the cluster join token for SSM: " + reason;
            return result;
        }
        std::vector<std::string> store{"aws", "ssm", "put-parameter"};
        flag(store, "--name", parameter);
        flag(store, "--type", "SecureString");
        flag(store, "--value", "file://" + token_file->path().string());
        store.push_back("--overwrite");
        append_global(store, config);
        const CommandResult stored = runner_(store);
        if (stored.exit_code != 0) {
            // Fail closed: nodes launched without a fetchable token cannot join.
            result.satisfied = false;
            result.detail = "aws ssm put-parameter (cluster join token) failed: " +
                            (stored.stderr_text.empty() ? stored.stdout_text
                                                        : stored.stderr_text);
            return result;
        }
        if (!tags.empty()) tags += ' ';
        tags += "Key=ZETTA_CLUSTER_TOKEN_SSM_PARAMETER,Value=" + parameter +
                ",PropagateAtLaunch=true";
    }
    if (!tags.empty()) {
        flag(create, "--tags", tags);
    }
    append_global(create, config);

    const CommandResult created = runner_(create);
    if (created.exit_code != 0) {
        // The pool likely already exists (the stable name makes a re-burst
        // idempotent) — scale the existing group to the requested count instead.
        // If that also fails, it is a real error worth surfacing.
        std::vector<std::string> scale{"aws", "autoscaling",
                                       "set-desired-capacity"};
        flag(scale, "--auto-scaling-group-name", config.pool_name);
        flag(scale, "--desired-capacity", std::to_string(request.count));
        append_global(scale, config);

        const CommandResult scaled = runner_(scale);
        if (scaled.exit_code != 0) {
            result.satisfied = false;
            result.detail = "aws autoscaling create/scale failed: " +
                            (scaled.stderr_text.empty() ? scaled.stdout_text
                                                        : scaled.stderr_text);
            return result;
        }
    }

    // The pool is scaling; list what is up and report best-effort against the
    // requested count.
    result.nodes = list(config);
    result.satisfied = result.nodes.size() >= request.count;
    if (result.satisfied) {
        result.detail = "burst pool '" + config.pool_name + "' scaled to " +
                        std::to_string(request.count) + " node(s)";
    } else {
        result.detail = "best-effort: " + std::to_string(result.nodes.size()) +
                        " of " + std::to_string(request.count) +
                        " requested nodes up in pool '" + config.pool_name + "'";
    }
    return result;
}

bool AwsCliControlPlane::destroy(const AwsConfig& config,
                                 const std::string& node_id) {
    std::vector<std::string> remove{"aws", "autoscaling",
                                    "terminate-instance-in-auto-scaling-group"};
    flag(remove, "--instance-id", node_id);
    // Drop the desired capacity with the instance so the ASG does not relaunch a
    // replacement — the burst is shrinking, not self-healing.
    remove.push_back("--should-decrement-desired-capacity");
    append_global(remove, config);
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> AwsCliControlPlane::list(
    const AwsConfig& config) const {
    std::vector<std::string> query{"aws", "ec2", "describe-instances"};
    // Filter to the burst pool's running instances (the ASG tags every instance
    // it launches with its group name).
    query.push_back("--filters");
    query.push_back("Name=tag:aws:autoscaling:groupName,Values=" +
                    config.pool_name);
    query.push_back("Name=instance-state-name,Values=running");
    // Project each instance into the ProvisionedNode shape. The reference query
    // pulls id + address; a production query also joins the capability the node
    // reports as instance tags so the parse matches ProvisionedNode::from_json.
    query.push_back("--query");
    query.push_back(
        "Reservations[].Instances[].{node_id:InstanceId,address:PrivateIpAddress}");
    flag(query, "--output", "json");
    append_global(query, config);

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta::cloud