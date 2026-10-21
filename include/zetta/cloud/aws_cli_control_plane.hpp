#pragma once

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

class AwsCliControlPlane : public AwsControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real `aws` exec). A test
    // injects a fake that asserts on the argv and returns canned JSON.
    explicit AwsCliControlPlane(CommandRunner runner = {});

    // Scale the burst pool to `request.count` nodes: create the Auto Scaling Group
    // at that desired capacity, falling back to `set-desired-capacity` when it
    // already exists (the stable pool name makes this idempotent), then list the
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