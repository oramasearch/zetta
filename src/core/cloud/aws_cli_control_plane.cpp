#include "zetta/cloud/aws_cli_control_plane.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../base64.hpp"
#include "zetta/cloud/command_runner.hpp"
#include "zetta/cloud/node_bootstrap_script.hpp"
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
std::string mixed_instances_policy(const AwsConfig& config, const std::string& version) {
    std::string policy = "LaunchTemplate={LaunchTemplateSpecification={LaunchTemplateName=" +
                         config.launch_template;
    if (!version.empty()) policy += ",Version=" + version;
    policy += "}";
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

// The launch template the group launches from: the operator's, at the version
// that carries the boot script when one was created.
std::string launch_template_spec(const AwsConfig& config, const std::string& version) {
    std::string spec = "LaunchTemplateName=" + config.launch_template;
    if (!version.empty()) spec += ",Version=" + version;
    return spec;
}

std::string command_output(const CommandResult& result) {
    return result.stderr_text.empty() ? result.stdout_text : result.stderr_text;
}

}  // namespace

std::optional<std::string> aws_node_boot_script(const AwsConfig& config, std::string* reason) {
    NodeBootstrapScriptOptions options;
    options.seed_peer = config.seed_peer;
    options.identity_root = config.identity_root;
    options.cluster_ca = config.cluster_ca;
    if (!config.join_token.empty()) {
        options.token.store = ClusterTokenStore::aws_ssm_parameter;
        options.token.secret_name = cluster_token_parameter_name(config);
        options.token.region = config.region;
    } else if (config.seed_peer.empty() && config.identity_root.empty() &&
               config.cluster_ca.empty()) {
        return std::string{};  // nothing to hand over: the template runs as it is
    }
    return render_node_bootstrap_script(options, reason);
}

AwsCliControlPlane::AwsCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult AwsCliControlPlane::bring_up(const AwsConfig& config,
                                             const ProvisionRequest& request) {
    ProvisionResult result;
    const auto fail = [&result](std::string detail) {
        result.satisfied = false;
        result.detail = std::move(detail);
        return result;
    };

    // The boot script each node runs (#1316, #1338): the seed, the cluster's
    // public identity root and enrollment CA, and on a secure cluster only the
    // *name* of the SSM parameter holding the join token. Rendered before
    // anything is created, so a value that cannot be written into a script
    // stops the burst before it starts.
    std::string script_reason;
    const std::optional<std::string> boot_script =
        aws_node_boot_script(config, &script_reason);
    if (!boot_script) return fail("cannot render the node boot script: " + script_reason);
    // The seed and the token's parameter name also ride propagating tags (#670,
    // #770), for anything that reads the instance's tags. The token never does:
    // a tag is readable by any principal holding ec2:DescribeTags and lands in
    // CloudTrail. It is stored as an SSM Parameter Store SecureString
    // (KMS-encrypted, read only by a principal granted ssm:GetParameter on it —
    // the node's instance profile). The value reaches `aws` through a private
    // file (`file://`), never through argv.
    std::string tags;
    if (!config.seed_peer.empty()) {
        tags = "Key=ZETTA_SEED_PEER,Value=" + config.seed_peer +
               ",PropagateAtLaunch=true";
    }
    if (!config.join_token.empty()) {
        const std::string parameter = cluster_token_parameter_name(config);
        std::string reason;
        const std::unique_ptr<SecretFile> token_file =
            SecretFile::create(config.join_token, &reason);
        if (!token_file) return fail("cannot stage the cluster join token for SSM: " + reason);
        std::vector<std::string> store{"aws", "ssm", "put-parameter"};
        flag(store, "--name", parameter);
        flag(store, "--type", "SecureString");
        flag(store, "--value", "file://" + token_file->path().string());
        store.push_back("--overwrite");
        append_global(store, config);
        const CommandResult stored = runner_(store);
        if (stored.exit_code != 0) {
            // Fail closed: nodes launched without a fetchable token cannot join.
            return fail("aws ssm put-parameter (cluster join token) failed: " +
                        command_output(stored));
        }
        if (!tags.empty()) tags += ' ';
        tags += "Key=ZETTA_CLUSTER_TOKEN_SSM_PARAMETER,Value=" + parameter +
                ",PropagateAtLaunch=true";
    }

    // The boot script reaches the nodes as the launch template's user-data
    // (#1338): `create-auto-scaling-group` cannot override user-data, so a new
    // version of the operator's template is created from its default version
    // with only `UserData` replaced, and the group launches from that version.
    // The source is `$Default`, the operator's baseline, never `$Latest`, which
    // may be an earlier version made here. The version carries no secret (the
    // script holds the parameter's name, not the token).
    std::string version;
    if (!boot_script->empty()) {
        const nlohmann::json data{
            {"UserData", detail::base64_encode(std::as_bytes(std::span{*boot_script}))}};
        std::string reason;
        const std::unique_ptr<SecretFile> data_file = SecretFile::create(data.dump(), &reason);
        if (!data_file) return fail("cannot stage the launch template data: " + reason);
        std::vector<std::string> make{"aws", "ec2", "create-launch-template-version"};
        flag(make, "--launch-template-name", config.launch_template);
        flag(make, "--source-version", "$Default");
        flag(make, "--version-description", "zetta node boot script for pool " + config.pool_name);
        flag(make, "--launch-template-data", "file://" + data_file->path().string());
        flag(make, "--query", "LaunchTemplateVersion.VersionNumber");
        flag(make, "--output", "text");
        append_global(make, config);
        const CommandResult made = runner_(make);
        if (made.exit_code != 0) {
            // Fail closed: a node launched without the script cannot join.
            return fail("aws ec2 create-launch-template-version (node boot script) failed: " +
                        command_output(made));
        }
        std::string text = made.stdout_text;
        std::erase_if(text, [](unsigned char character) { return std::isspace(character); });
        if (text.empty() || !std::ranges::all_of(text, [](unsigned char character) {
                return std::isdigit(character) != 0;
            })) {
            return fail("aws ec2 create-launch-template-version answered no version number: " +
                        made.stdout_text);
        }
        version = std::move(text);
    }

    // Create the Auto Scaling Group at the requested desired capacity from that
    // version. The template defines the AMI / instance type / IAM instance
    // profile; the ASG spans the configured subnets. Spot capacity and/or an
    // instance-type override ride on a mixed-instances policy (the AWS way to
    // ask an ASG for Spot), which then names the template itself.
    const bool mixed = config.spot || !config.instance_type.empty();
    std::vector<std::string> create{"aws", "autoscaling", "create-auto-scaling-group"};
    flag(create, "--auto-scaling-group-name", config.pool_name);
    if (mixed) {
        flag(create, "--mixed-instances-policy", mixed_instances_policy(config, version));
    } else {
        flag(create, "--launch-template", launch_template_spec(config, version));
    }
    flag(create, "--min-size", "0");
    flag(create, "--max-size", std::to_string(request.count));
    flag(create, "--desired-capacity", std::to_string(request.count));
    flag(create, "--vpc-zone-identifier", config.subnets);
    if (!tags.empty()) {
        flag(create, "--tags", tags);
    }
    append_global(create, config);

    const CommandResult created = runner_(create);
    if (created.exit_code != 0) {
        // The pool likely already exists (the stable name makes a re-burst
        // idempotent). Point it at the new version first, so the nodes it adds
        // run the current boot script, then scale it to the requested count. If
        // either fails, it is a real error worth surfacing.
        if (!version.empty()) {
            std::vector<std::string> update{"aws", "autoscaling", "update-auto-scaling-group"};
            flag(update, "--auto-scaling-group-name", config.pool_name);
            if (mixed) {
                flag(update, "--mixed-instances-policy", mixed_instances_policy(config, version));
            } else {
                flag(update, "--launch-template", launch_template_spec(config, version));
            }
            append_global(update, config);
            const CommandResult updated = runner_(update);
            if (updated.exit_code != 0) {
                return fail("aws autoscaling create/update failed: " + command_output(updated));
            }
        }
        std::vector<std::string> scale{"aws", "autoscaling",
                                       "set-desired-capacity"};
        flag(scale, "--auto-scaling-group-name", config.pool_name);
        flag(scale, "--desired-capacity", std::to_string(request.count));
        append_global(scale, config);

        const CommandResult scaled = runner_(scale);
        if (scaled.exit_code != 0) {
            return fail("aws autoscaling create/scale failed: " + command_output(scaled));
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