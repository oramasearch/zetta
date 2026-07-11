#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cloud/burst_identity.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/cloud/k8s_cli_control_plane.hpp"
#include "zetta/cloud/k8s_provisioner.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"

using zetta::cloud::BurstIdentityManager;
using zetta::governance::CapabilityDomain;
using zetta::governance::CapabilitySet;
using zetta::cloud::CommandResult;
using zetta::cloud::CommandRunner;
using zetta::cloud::Gpu;
using zetta::cloud::GpuKind;
using zetta::cloud::K8sCliControlPlane;
using zetta::cloud::K8sConfig;
using zetta::cloud::K8sControlPlane;
using zetta::cloud::K8sProvisioner;
using zetta::Node;
using zetta::cloud::NodeCapability;
using zetta::NodeConfig;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;
using zetta::governance::ResourceKind;
using zetta::governance::Scope;
using zetta::test::TempDir;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

K8sConfig sample_config() {
    K8sConfig config;
    config.image = "ghcr.io/oramasearch/zetta:latest";
    return config;
}

NodeCapability gpu_spec() {
    NodeCapability capability;
    capability.cores = 8;
    capability.memory_bytes = 64 * kGiB;
    capability.gpu = Gpu{GpuKind::Cuda, 312.0, "A100"};
    return capability;
}

// A fake K8sControlPlane modelling a burst Deployment: bring_up scales the pool to
// the requested count (bounded by `capacity`, to exercise best-effort), destroy
// removes one pod, list returns what is up.
class FakeControlPlane : public K8sControlPlane {
public:
    std::size_t capacity = 1000;          // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;    // what is currently up
    int bring_up_calls = 0;

    ProvisionResult bring_up(const K8sConfig& /*config*/,
                             const ProvisionRequest& request) override {
        ++bring_up_calls;
        const std::size_t target = std::min(request.count, capacity);
        pool.clear();
        for (std::size_t index = 0; index < target; ++index) {
            ProvisionedNode node;
            node.node_id = "zetta-burst-pod-" + std::to_string(index);
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

    bool destroy(const K8sConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const K8sConfig& /*config*/) const override {
        return pool;
    }
};

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

std::string flag_value(const std::vector<std::string>& argv,
                       const std::string& name) {
    for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
        if (argv[index] == name) return argv[index + 1];
    }
    return "";
}

// The first recorded call containing all of `tokens`.
const std::vector<std::string>* call_with(
    const std::vector<std::vector<std::string>>& calls,
    const std::vector<std::string>& tokens) {
    for (const std::vector<std::string>& argv : calls) {
        if (std::all_of(tokens.begin(), tokens.end(),
                        [&](const std::string& token) { return has_token(argv, token); })) {
            return &argv;
        }
    }
    return nullptr;
}

}  // namespace

// ---- K8sConfig JSON boundary -----------------------------------------------

TEST(K8sConfig, RoundTripsMinimalConfig) {
    const K8sConfig config = sample_config();
    const auto parsed = K8sConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(K8sConfig, RoundTripsFullConfig) {
    K8sConfig config = sample_config();
    config.namespace_name = "churn";
    config.deployment_name = "churn-burst";
    config.context = "prod-cluster";
    config.service_account = "zetta-burst-sa";
    config.spot = true;
    config.seed_peer = "10.0.0.1:7777";

    const nlohmann::json json = config.to_json();
    EXPECT_EQ(json["namespace"], "churn");  // C++ namespace_name maps to JSON "namespace"

    const auto parsed = K8sConfig::from_json(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(K8sConfig, DefaultsAreFilledIn) {
    const K8sConfig config = sample_config();
    EXPECT_EQ(config.namespace_name, "default");
    EXPECT_EQ(config.deployment_name, "zetta-burst");
    EXPECT_FALSE(config.spot);
}

TEST(K8sConfig, RejectsMalformedDocuments) {
    EXPECT_FALSE(K8sConfig::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(K8sConfig::from_json("not-an-object").has_value());

    // Missing the required image.
    EXPECT_FALSE(K8sConfig::from_json(nlohmann::json::object()).has_value());

    // Empty required image.
    EXPECT_FALSE(K8sConfig::from_json(nlohmann::json{{"image", ""}}).has_value());

    // Wrong-typed fields.
    EXPECT_FALSE(K8sConfig::from_json(nlohmann::json{{"image", 42}}).has_value());
    EXPECT_FALSE(K8sConfig::from_json(
        nlohmann::json{{"image", "x"}, {"namespace", 7}}).has_value());
    EXPECT_FALSE(K8sConfig::from_json(
        nlohmann::json{{"image", "x"}, {"spot", "yes"}}).has_value());
}

// ---- K8sProvisioner lifecycle (against a fake control plane) ----------------

TEST(K8sProvisioner, ProvisionBringsUpPodsAndMintsIdentities) {
    FakeControlPlane control_plane;
    std::vector<ProvisionedNode> minted;
    int revoke_count = 0;

    K8sProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };
    identity.revoke = [&] { ++revoke_count; };

    K8sProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(control_plane.bring_up_calls, 1);
    EXPECT_EQ(minted, result.nodes);
    EXPECT_EQ(revoke_count, 0);
}

TEST(K8sProvisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeControlPlane control_plane;
    control_plane.capacity = 2;
    std::vector<ProvisionedNode> minted;

    K8sProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };

    K8sProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{5, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(minted.size(), 2u);
}

TEST(K8sProvisioner, ZeroCountIsASatisfiedNoOp) {
    FakeControlPlane control_plane;
    int mint_calls = 0;

    K8sProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    K8sProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(control_plane.bring_up_calls, 0);
    EXPECT_EQ(mint_calls, 0);
}

TEST(K8sProvisioner, NoMintWhenNothingCameUp) {
    FakeControlPlane control_plane;
    control_plane.capacity = 0;
    int mint_calls = 0;

    K8sProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    K8sProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(mint_calls, 0);
}

TEST(K8sProvisioner, TeardownRevokesIdentitiesWhenPoolDrains) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    K8sProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) {};
    identity.revoke = [&] { ++revoke_count; };

    K8sProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{2, gpu_spec()});
    ASSERT_EQ(provisioner.provisioned().size(), 2u);

    EXPECT_TRUE(provisioner.teardown("zetta-burst-pod-0"));
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
    EXPECT_EQ(revoke_count, 0);

    EXPECT_TRUE(provisioner.teardown("zetta-burst-pod-1"));
    EXPECT_TRUE(provisioner.provisioned().empty());
    EXPECT_EQ(revoke_count, 1);
}

TEST(K8sProvisioner, TeardownOfUnknownNodeIsAnIdempotentNoOp) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    K8sProvisioner::IdentityHooks identity;
    identity.revoke = [&] { ++revoke_count; };

    K8sProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{1, gpu_spec()});

    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(revoke_count, 0);
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
}

TEST(K8sProvisioner, ProvisionedReflectsLiveCapacity) {
    FakeControlPlane control_plane;
    K8sProvisioner provisioner{control_plane, sample_config()};
    provisioner.provision(ProvisionRequest{3, gpu_spec()});
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // An evicted pod disappears without any teardown call.
    control_plane.pool.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
}

TEST(K8sProvisioner, WorksWithNoIdentityHooks) {
    FakeControlPlane control_plane;
    K8sProvisioner provisioner{control_plane, sample_config()};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, gpu_spec()});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(provisioner.teardown("zetta-burst-pod-0"));
    EXPECT_TRUE(provisioner.teardown("zetta-burst-pod-1"));
}

// ---- "with bulk identity": compose with a real BurstIdentityManager ---------

TEST(K8sProvisioner, BurstWithBulkIdentityLifecycle) {
    TempDir tmp;
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    Node node{cfg};
    node.start();
    ASSERT_EQ(node.principal_count(), 0u);

    FakeControlPlane control_plane;
    BurstIdentityManager identities{node};
    const std::string job = "job-churn-k8s";

    K8sProvisioner::IdentityHooks hooks;
    hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
        Scope scope;
        scope.grant(ResourceKind::Project, "churn");
        CapabilitySet caps;
        caps.grant(CapabilityDomain::Orchestration, "run", "2999-01-01T00:00:00.000Z");
        identities.mint(job, nodes, scope, caps);
    };
    hooks.revoke = [&] { identities.revoke(job); };

    K8sProvisioner provisioner{control_plane, sample_config(), hooks};

    const ProvisionResult burst =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});
    ASSERT_EQ(burst.nodes.size(), 3u);
    EXPECT_EQ(node.principal_count(), 3u);
    EXPECT_EQ(identities.minted(job).size(), 3u);

    for (const ProvisionedNode& up : provisioner.provisioned()) {
        EXPECT_TRUE(provisioner.teardown(up.node_id));
    }
    EXPECT_TRUE(provisioner.provisioned().empty());
    EXPECT_EQ(node.principal_count(), 0u);
    EXPECT_TRUE(identities.minted(job).empty());

    node.stop();
}

// ---- K8sCliControlPlane reference binding (against a fake command runner) ----

namespace {

// A canned `kubectl get pods --output json` output: the { "items": [ ... ] }
// envelope wrapping two ProvisionedNode-shaped objects (a production binding
// projects pods into this shape).
std::string two_pod_listing() {
    return R"({"items":[
      {"node_id":"zetta-burst-aaa","address":"10.2.0.1:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}},
      {"node_id":"zetta-burst-bbb","address":"10.2.0.2:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}}
    ]})";
}

}  // namespace

TEST(K8sCliControlPlane, BringUpCreatesScalesThenListsPods) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "get") && has_token(argv, "pods")) {
            result.stdout_text = two_pod_listing();
        }
        return result;  // exit_code 0 by default
    };

    K8sCliControlPlane control_plane{runner};
    K8sConfig config = sample_config();
    config.namespace_name = "churn";

    const ProvisionResult result =
        control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 2u);
    EXPECT_EQ(result.nodes[0].node_id, "zetta-burst-aaa");

    // The create call carries the deployment name + image + namespace.
    const std::vector<std::string>* create = call_with(calls, {"create", "deployment"});
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(has_token(*create, "zetta-burst"));
    EXPECT_TRUE(has_flag(*create, "--image", "ghcr.io/oramasearch/zetta:latest"));
    EXPECT_TRUE(has_flag(*create, "--namespace", "churn"));

    // The scale call sets the replica count on the deployment.
    const std::vector<std::string>* scale = call_with(calls, {"scale"});
    ASSERT_NE(scale, nullptr);
    EXPECT_TRUE(has_token(*scale, "deployment/zetta-burst"));
    EXPECT_TRUE(has_flag(*scale, "--replicas", "2"));

    // The list call selects the deployment's pods as JSON.
    const std::vector<std::string>* get = call_with(calls, {"get", "pods"});
    ASSERT_NE(get, nullptr);
    EXPECT_TRUE(has_flag(*get, "--selector", "app=zetta-burst"));
    EXPECT_TRUE(has_flag(*get, "--output", "json"));
}

TEST(K8sCliControlPlane, SeedServiceAccountAndSpotShapeTheDeployment) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "get") && has_token(argv, "pods")) {
            result.stdout_text = two_pod_listing();
        }
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    K8sConfig config = sample_config();
    config.seed_peer = "10.0.0.1:7777";
    // On a secure cluster the join token rides alongside the seed peer (#670),
    // both set in the one `kubectl set env` call.
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    config.service_account = "zetta-burst-sa";
    config.spot = true;

    control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    // Seed peer → set env ZETTA_SEED_PEER; join token → set env ZETTA_CLUSTER_TOKEN.
    const std::vector<std::string>* env = call_with(calls, {"set", "env"});
    ASSERT_NE(env, nullptr);
    EXPECT_TRUE(has_token(*env, "ZETTA_SEED_PEER=10.0.0.1:7777"));
    EXPECT_TRUE(has_token(
        *env,
        "ZETTA_CLUSTER_TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));

    // Service account → set serviceaccount.
    const std::vector<std::string>* sa = call_with(calls, {"set", "serviceaccount"});
    ASSERT_NE(sa, nullptr);
    EXPECT_TRUE(has_token(*sa, "zetta-burst-sa"));

    // Spot → patch with a nodeSelector.
    const std::vector<std::string>* patch = call_with(calls, {"patch"});
    ASSERT_NE(patch, nullptr);
    EXPECT_NE(flag_value(*patch, "--patch").find("nodeSelector"), std::string::npos);
    EXPECT_NE(flag_value(*patch, "--patch").find("spot"), std::string::npos);
}

TEST(K8sCliControlPlane, BringUpIsBestEffortWhenFewerPodsComeUp) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "get") && has_token(argv, "pods")) {
            result.stdout_text = two_pod_listing();
        }
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    // Ask for three but the listing reports two.
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
}

TEST(K8sCliControlPlane, BringUpReportsScaleFailure) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "scale")) {
            result.exit_code = 1;
            result.stderr_text = "deployments.apps \"zetta-burst\" not found";
        }
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_NE(result.detail.find("not found"), std::string::npos);
}

TEST(K8sCliControlPlane, DestroyDeletesThePod) {
    std::vector<std::string> seen;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        seen = argv;
        CommandResult result;
        if (has_token(argv, "missing")) result.exit_code = 1;  // unknown pod
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.destroy(sample_config(), "zetta-burst-aaa"));
    EXPECT_TRUE(has_token(seen, "delete"));
    EXPECT_TRUE(has_token(seen, "pod"));
    EXPECT_TRUE(has_token(seen, "zetta-burst-aaa"));

    EXPECT_FALSE(control_plane.destroy(sample_config(), "missing"));  // non-zero → false
}

TEST(K8sCliControlPlane, ListParsesPodsAndSkipsMalformed) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        // One valid pod, one missing node_id (rejected at the boundary).
        result.stdout_text = R"({"items":[
          {"node_id":"zetta-burst-aaa","address":"10.2.0.1:7777",
           "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}},
          {"address":"10.2.0.2:7777",
           "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}}
        ]})";
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    const std::vector<ProvisionedNode> nodes = control_plane.list(sample_config());
    ASSERT_EQ(nodes.size(), 1u);  // the malformed entry is skipped
    EXPECT_EQ(nodes.front().node_id, "zetta-burst-aaa");
}

TEST(K8sCliControlPlane, ListReturnsEmptyOnCommandFailure) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        result.exit_code = 1;  // `kubectl` failed
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.list(sample_config()).empty());
}

TEST(K8sCliControlPlane, ContextFlagIsPassedWhenSet) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "get") && has_token(argv, "pods")) {
            result.stdout_text = two_pod_listing();
        }
        return result;
    };

    K8sCliControlPlane control_plane{runner};
    K8sConfig config = sample_config();
    config.context = "prod-cluster";

    control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    // Every kubectl call carries the pinned context.
    const std::vector<std::string>* scale = call_with(calls, {"scale"});
    ASSERT_NE(scale, nullptr);
    EXPECT_TRUE(has_flag(*scale, "--context", "prod-cluster"));
}
