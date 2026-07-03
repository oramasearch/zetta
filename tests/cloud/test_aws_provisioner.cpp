#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/aws_cli_control_plane.hpp"
#include "zetta/cloud/aws_provisioner.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::cloud::AwsCliControlPlane;
using zetta::cloud::AwsConfig;
using zetta::cloud::AwsControlPlane;
using zetta::cloud::AwsProvisioner;
using zetta::cloud::CommandResult;
using zetta::cloud::CommandRunner;
using zetta::cloud::Gpu;
using zetta::cloud::GpuKind;
using zetta::cloud::NodeCapability;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

AwsConfig sample_config() {
    AwsConfig config;
    config.region = "us-west-2";
    config.launch_template = "zetta-burst-lt";
    config.subnets = "subnet-aaa,subnet-bbb";
    return config;
}

NodeCapability gpu_spec() {
    NodeCapability capability;
    capability.cores = 8;
    capability.memory_bytes = 64 * kGiB;
    capability.gpu = Gpu{GpuKind::Cuda, 312.0, "A100"};
    return capability;
}

// A fake AwsControlPlane modelling a burst pool: bring_up scales the pool to the
// requested count (bounded by `capacity`, to exercise best-effort), destroy
// removes one node, list returns what is up. (Mirrors the Azure test's fake.)
class FakeControlPlane : public AwsControlPlane {
public:
    std::size_t capacity = 1000;        // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;  // what is currently up
    int bring_up_calls = 0;

    ProvisionResult bring_up(const AwsConfig& /*config*/,
                             const ProvisionRequest& request) override {
        ++bring_up_calls;
        const std::size_t target = std::min(request.count, capacity);
        pool.clear();
        for (std::size_t index = 0; index < target; ++index) {
            ProvisionedNode node;
            node.node_id = "burst-" + std::to_string(index);
            node.address = "10.1.0." + std::to_string(index) + ":7777";
            node.capability = request.spec;
            pool.push_back(node);
        }
        ProvisionResult result;
        result.nodes = pool;
        result.satisfied = pool.size() >= request.count;
        result.detail = result.satisfied ? "scaled" : "best-effort short";
        return result;
    }

    bool destroy(const AwsConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const AwsConfig& /*config*/) const override {
        return pool;
    }
};

// Does argv contain `name` immediately followed by `value`?
bool has_flag(const std::vector<std::string>& argv, const std::string& name,
              const std::string& value) {
    for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
        if (argv[index] == name && argv[index + 1] == value) return true;
    }
    return false;
}

bool has_token(const std::vector<std::string>& argv, const std::string& token) {
    return std::find(argv.begin(), argv.end(), token) != argv.end();
}

// Find the first argv whose value at `name` contains `needle` (for matching the
// mixed-instances-policy / tag value substrings).
bool flag_value_contains(const std::vector<std::string>& argv,
                         const std::string& name, const std::string& needle) {
    for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
        if (argv[index] == name &&
            argv[index + 1].find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- AwsConfig JSON boundary -----------------------------------------------

TEST(AwsConfig, RoundTripsMinimalConfig) {
    const AwsConfig config = sample_config();
    const auto parsed = AwsConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(AwsConfig, RoundTripsFullConfig) {
    AwsConfig config = sample_config();
    config.pool_name = "churn-burst";
    config.instance_type = "p4d.24xlarge";
    config.spot = true;
    config.max_price = 12.5;
    config.seed_peer = "10.0.0.1:7777";
    config.profile = "burst-dev";

    const auto parsed = AwsConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(AwsConfig, DefaultsAreFilledIn) {
    const AwsConfig config = sample_config();
    EXPECT_EQ(config.pool_name, "zetta-burst");  // default name
    EXPECT_FALSE(config.spot);
    EXPECT_DOUBLE_EQ(config.max_price, 0.0);  // AWS: 0 = up to on-demand
}

TEST(AwsConfig, RejectsMalformedDocuments) {
    EXPECT_FALSE(AwsConfig::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(AwsConfig::from_json("not-an-object").has_value());

    // Missing each required field in turn.
    for (const char* missing : {"region", "launch_template", "subnets"}) {
        nlohmann::json document = sample_config().to_json();
        document.erase(missing);
        EXPECT_FALSE(AwsConfig::from_json(document).has_value()) << missing;
    }

    // Empty required field.
    nlohmann::json empty_required = sample_config().to_json();
    empty_required["region"] = "";
    EXPECT_FALSE(AwsConfig::from_json(empty_required).has_value());

    // Wrong-typed fields.
    nlohmann::json wrong_spot = sample_config().to_json();
    wrong_spot["spot"] = "yes";
    EXPECT_FALSE(AwsConfig::from_json(wrong_spot).has_value());

    nlohmann::json wrong_price = sample_config().to_json();
    wrong_price["max_price"] = "cheap";
    EXPECT_FALSE(AwsConfig::from_json(wrong_price).has_value());

    // Negative max_price (AWS floor is 0).
    nlohmann::json low_price = sample_config().to_json();
    low_price["max_price"] = -1.0;
    EXPECT_FALSE(AwsConfig::from_json(low_price).has_value());
}

// ---- AwsProvisioner lifecycle (against a fake control plane) ----------------

TEST(AwsProvisioner, ProvisionBringsUpNodesAndMintsIdentities) {
    FakeControlPlane control_plane;
    std::vector<ProvisionedNode> minted;
    int revoke_count = 0;

    AwsProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };
    identity.revoke = [&] { ++revoke_count; };

    AwsProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(control_plane.bring_up_calls, 1);
    // Identities minted for exactly the nodes that came up — so they can join.
    EXPECT_EQ(minted, result.nodes);
    EXPECT_EQ(revoke_count, 0);  // nothing torn down yet
}

TEST(AwsProvisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeControlPlane control_plane;
    control_plane.capacity = 2;  // the pool can only bring up two
    std::vector<ProvisionedNode> minted;

    AwsProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };

    AwsProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{5, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(minted.size(), 2u);  // mint whatever actually came up
}

TEST(AwsProvisioner, ZeroCountIsASatisfiedNoOp) {
    FakeControlPlane control_plane;
    int mint_calls = 0;

    AwsProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    AwsProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(control_plane.bring_up_calls, 0);  // control plane untouched
    EXPECT_EQ(mint_calls, 0);                    // identity untouched
}

TEST(AwsProvisioner, NoMintWhenNothingCameUp) {
    FakeControlPlane control_plane;
    control_plane.capacity = 0;  // nothing can come up
    int mint_calls = 0;

    AwsProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    AwsProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(mint_calls, 0);  // nothing to mint for
}

TEST(AwsProvisioner, TeardownRevokesIdentitiesWhenPoolDrains) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    AwsProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) {};
    identity.revoke = [&] { ++revoke_count; };

    AwsProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{2, gpu_spec()});
    ASSERT_EQ(provisioner.provisioned().size(), 2u);

    // Tear down the first node — one still up, so no bulk revoke yet.
    EXPECT_TRUE(provisioner.teardown("burst-0"));
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
    EXPECT_EQ(revoke_count, 0);

    // Tear down the last node — the pool drains, so identities are revoked once.
    EXPECT_TRUE(provisioner.teardown("burst-1"));
    EXPECT_TRUE(provisioner.provisioned().empty());
    EXPECT_EQ(revoke_count, 1);
}

TEST(AwsProvisioner, TeardownOfUnknownNodeIsAnIdempotentNoOp) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    AwsProvisioner::IdentityHooks identity;
    identity.revoke = [&] { ++revoke_count; };

    AwsProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{1, gpu_spec()});

    // An unknown id releases nothing and does not fire revoke (the pool is not
    // empty, and no node was actually released).
    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(revoke_count, 0);
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
}

TEST(AwsProvisioner, ProvisionedReflectsLiveCapacity) {
    FakeControlPlane control_plane;
    AwsProvisioner provisioner{control_plane, sample_config()};  // no identity wired
    provisioner.provision(ProvisionRequest{3, gpu_spec()});
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // A Spot interruption removes a node without any teardown call — provisioned()
    // re-reads the control plane, so it shrinks to match.
    control_plane.pool.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
}

TEST(AwsProvisioner, WorksWithNoIdentityHooks) {
    FakeControlPlane control_plane;
    AwsProvisioner provisioner{control_plane, sample_config()};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, gpu_spec()});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(provisioner.teardown("burst-0"));  // drains to one, no revoke hook
    EXPECT_TRUE(provisioner.teardown("burst-1"));  // drains to empty, still fine
}

// ---- AwsCliControlPlane reference binding (against a fake command runner) ----

namespace {

// A canned `aws ec2 describe-instances` (projected) output: two
// ProvisionedNode-shaped objects (the capability is the production tag-join).
std::string two_node_listing() {
    return R"([
      {"node_id":"i-0a","address":"10.2.0.1:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}},
      {"node_id":"i-0b","address":"10.2.0.2:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}}
    ])";
}

}  // namespace

TEST(AwsCliControlPlane, BringUpBuildsCreateAsgThenListsInstances) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "describe-instances")) result.stdout_text = two_node_listing();
        return result;  // exit_code 0 by default
    };

    AwsCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 2u);
    EXPECT_EQ(result.nodes[0].node_id, "i-0a");

    // The create call carries the pool identity, launch template, sizing, subnets.
    ASSERT_FALSE(calls.empty());
    const std::vector<std::string>& create = calls.front();
    EXPECT_TRUE(has_token(create, "create-auto-scaling-group"));
    EXPECT_TRUE(has_flag(create, "--auto-scaling-group-name", "zetta-burst"));
    EXPECT_TRUE(has_flag(create, "--launch-template", "LaunchTemplateName=zetta-burst-lt"));
    EXPECT_TRUE(has_flag(create, "--max-size", "2"));
    EXPECT_TRUE(has_flag(create, "--desired-capacity", "2"));
    EXPECT_TRUE(has_flag(create, "--vpc-zone-identifier", "subnet-aaa,subnet-bbb"));
    EXPECT_TRUE(has_flag(create, "--region", "us-west-2"));
    EXPECT_FALSE(has_token(create, "--mixed-instances-policy"));  // not a spot config
}

TEST(AwsCliControlPlane, SpotConfigAddsMixedInstancesPolicyAndSeedTag) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "describe-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    AwsConfig config = sample_config();
    config.spot = true;
    config.max_price = 1.25;
    config.instance_type = "g5.xlarge";
    config.seed_peer = "10.0.0.1:7777";
    config.profile = "burst-dev";

    control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    const std::vector<std::string>& create = calls.front();
    EXPECT_TRUE(has_token(create, "--mixed-instances-policy"));
    EXPECT_TRUE(flag_value_contains(create, "--mixed-instances-policy", "InstancesDistribution"));
    EXPECT_TRUE(flag_value_contains(create, "--mixed-instances-policy", "SpotMaxPrice"));
    EXPECT_TRUE(flag_value_contains(create, "--mixed-instances-policy", "g5.xlarge"));
    EXPECT_TRUE(flag_value_contains(create, "--tags", "Key=ZETTA_SEED_PEER"));
    EXPECT_TRUE(flag_value_contains(create, "--tags", "Value=10.0.0.1:7777"));
    EXPECT_TRUE(has_flag(create, "--profile", "burst-dev"));
}

TEST(AwsCliControlPlane, BringUpScalesWhenPoolAlreadyExists) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        // The group already exists: create fails, set-desired-capacity succeeds.
        if (has_token(argv, "create-auto-scaling-group")) {
            result.exit_code = 254;
            result.stderr_text = "AlreadyExists";
        }
        if (has_token(argv, "describe-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    // It fell back to set-desired-capacity to scale the existing group.
    bool scaled = false;
    for (const auto& call : calls) {
        if (has_token(call, "set-desired-capacity") &&
            has_flag(call, "--desired-capacity", "2")) {
            scaled = true;
        }
    }
    EXPECT_TRUE(scaled);
}

TEST(AwsCliControlPlane, BringUpIsBestEffortWhenFewerInstancesComeUp) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "describe-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    // Ask for three but the listing only reports two.
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
}

TEST(AwsCliControlPlane, BringUpReportsCreateAndScaleFailure) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "create-auto-scaling-group")) result.exit_code = 254;
        if (has_token(argv, "set-desired-capacity")) {
            result.exit_code = 254;
            result.stderr_text = "quota exceeded";
        }
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_NE(result.detail.find("quota exceeded"), std::string::npos);
}

TEST(AwsCliControlPlane, DestroyBuildsTerminateInstanceAndReportsOutcome) {
    std::vector<std::string> seen;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        seen = argv;
        CommandResult result;
        // Fail the terminate for one specific id to exercise the idempotent false.
        if (has_flag(argv, "--instance-id", "missing")) result.exit_code = 2;
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.destroy(sample_config(), "i-0a"));
    EXPECT_TRUE(has_token(seen, "terminate-instance-in-auto-scaling-group"));
    EXPECT_TRUE(has_flag(seen, "--instance-id", "i-0a"));
    EXPECT_TRUE(has_token(seen, "--should-decrement-desired-capacity"));

    EXPECT_FALSE(control_plane.destroy(sample_config(), "missing"));  // non-zero → false
}

TEST(AwsCliControlPlane, ListParsesInstancesAndSkipsMalformed) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        // One valid node, one missing node_id (rejected at the boundary).
        result.stdout_text = R"([
          {"node_id":"i-0a","address":"10.2.0.1:7777",
           "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}},
          {"address":"10.2.0.2:7777",
           "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}}
        ])";
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    const std::vector<ProvisionedNode> nodes = control_plane.list(sample_config());
    ASSERT_EQ(nodes.size(), 1u);  // the malformed entry is skipped
    EXPECT_EQ(nodes.front().node_id, "i-0a");
}

TEST(AwsCliControlPlane, ListReturnsEmptyOnCommandFailure) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        result.exit_code = 1;  // `aws` failed
        return result;
    };

    AwsCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.list(sample_config()).empty());
}
