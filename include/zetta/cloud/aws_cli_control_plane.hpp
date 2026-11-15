#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zetta/cloud/aws_provisioner.hpp"
#include "zetta/cloud/command_runner.hpp"  // CommandRunner / CommandResult (shared, vendor-neutral)

namespace zetta::cloud {

// AwsCliControlPlane — the **reference** AwsControlPlane binding (#377): it drives
// AWS by shelling out to the `aws` CLI (no AWS C++ SDK, honoring the "no cloud SDK
// behind the provisioner interface" rule). It translates the burst lifecycle into EC2 Auto Scaling
// Group commands — create/scale the burst pool, list its instances, terminate an
// instance — runs them through an injected CommandRunner, and parses the JSON the
// CLI returns at the validated ProvisionedNode boundary.
//
// This mirrors the reference AzCliControlPlane (#376) with the AWS *shape*: a
// stable, named EC2 Auto Scaling Group is the burst pool, scaling it up (set its
// desired capacity) is the burst, terminating instances (or deleting the group) is
// teardown — the AWS analog of Azure's VM Scale Set. Spot capacity rides on the
// ASG's mixed-instances policy; per-node IAM identity + the cluster-join bootstrap
// live in the launch template. A production deployment installs the `aws` CLI and
// authenticates it (`aws configure` / SSO / an instance role); CI never runs it,
// so the command construction + JSON parsing are what the unit tests cover via a
// fake runner. GCP (#378) mirrors this with its own `gcloud` binding.
// The SSM Parameter Store name the cluster join token is stored under for a
// burst pool (`/zetta/<pool>/cluster-token`, a SecureString). A node's instance
// profile needs ssm:GetParameter on it (and kms:Decrypt on its key); the
// `ZETTA_CLUSTER_TOKEN_SSM_PARAMETER` tag tells the node which name to read
// (#770). The token itself is never a tag.
std::string cluster_token_parameter_name(const AwsConfig& config);

// The node boot script (node_bootstrap_script.hpp) a burst pool's launch template
// version carries as user-data (#1338): the seed, the cluster's public identity
// root and enrollment CA (`ZETTA_IDENTITY_ROOT`, `ZETTA_CLUSTER_CA`), and on a
// secure cluster only the SSM parameter's *name*, read at boot with the instance
// profile. Empty when there is nothing to hand over; nullopt (with `reason`) when
// a value cannot be rendered. Never holds the token.
std::optional<std::string> aws_node_boot_script(const AwsConfig& config,
                                                std::string* reason = nullptr);

class AwsCliControlPlane : public AwsControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real `aws` exec). A test
    // injects a fake that asserts on the argv and returns canned JSON.
    explicit AwsCliControlPlane(CommandRunner runner = {});

    // Scale the burst pool to `request.count` nodes: store the join token in SSM,
    // create a launch template version from the operator's default version with
    // the node boot script as user-data (`create-launch-template-version`, #1338),
    // create the Auto Scaling Group from that version at that desired capacity,
    // or, when it already exists (the stable pool name makes this idempotent),
    // point it at the version and `set-desired-capacity`, then list the
    // instances. The `aws ec2 describe-instances` output is parsed with
    // ProvisionedNode::from_json; malformed entries are skipped. Best-effort:
    // returns what came up with `satisfied` / `detail` set.
    ProvisionResult bring_up(const AwsConfig& config,
                             const ProvisionRequest& request) override;

    // Terminate one instance by id, decrementing the group's desired capacity
    // (`aws autoscaling terminate-instance-in-auto-scaling-group`). Returns true
    // iff the command succeeded; a non-zero exit (e.g. unknown id) is the
    // idempotent no-op → false.
    bool destroy(const AwsConfig& config,
                 const std::string& node_id) override;

    // List the burst pool's running instances (`aws ec2 describe-instances`
    // filtered to the Auto Scaling Group), parsed at the ProvisionedNode boundary.
    // An empty / failed listing yields an empty vector.
    std::vector<ProvisionedNode> list(const AwsConfig& config) const override;

private:
    CommandRunner runner_;
};

}  // namespace zetta::cloud