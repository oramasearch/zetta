#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "secret_argv.hpp"
#include "temp_dir.hpp"
#include "zetta/cloud/burst_identity.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/cloud/gcp_cli_control_plane.hpp"
#include "zetta/cloud/gcp_provisioner.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"

using zetta::cloud::BurstIdentity;
using zetta::cloud::BurstIdentityManager;
using zetta::governance::CapabilityDomain;
using zetta::governance::CapabilitySet;
using zetta::cloud::CommandResult;
using zetta::cloud::CommandRunner;
using zetta::cloud::GcpCliControlPlane;
using zetta::cloud::GcpConfig;
using zetta::cloud::GcpControlPlane;
using zetta::cloud::GcpProvisioner;
using zetta::cloud::Gpu;
using zetta::cloud::GpuKind;
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

GcpConfig sample_config() {
    GcpConfig config;
    config.project_id = "example-churn";
    config.zone = "us-central1-a";
    return config;
}

NodeCapability gpu_spec() {
    NodeCapability capability;
    capability.cores = 8;
    capability.memory_bytes = 64 * kGiB;
    capability.gpu = Gpu{GpuKind::Cuda, 312.0, "A100"};
    return capability;
}

// A fake GcpControlPlane modelling a burst pool: bring_up scales the pool to the
// requested count (bounded by `capacity`, to exercise best-effort), destroy removes
// one node, list returns what is up.
class FakeControlPlane : public GcpControlPlane {
public:
    std::size_t capacity = 1000;          // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;    // what is currently up
    int bring_up_calls = 0;

    ProvisionResult bring_up(const GcpConfig& /*config*/,
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

    bool destroy(const GcpConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const GcpConfig& /*config*/) const override {
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

// Find an argv whose value at `name` contains `needle`.
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

// The first recorded call containing all of `tokens` (e.g. {"managed","create"} to
// pick the MIG create out of the several `gcloud compute` calls a bring_up makes).
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

// ---- GcpConfig JSON boundary -----------------------------------------------

TEST(GcpConfig, RoundTripsMinimalConfig) {
    const GcpConfig config = sample_config();
    const auto parsed = GcpConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(GcpConfig, RoundTripsFullConfig) {
    GcpConfig config = sample_config();
    config.pool_name = "churn-burst";
    config.instance_template = "churn-template";
    config.machine_type = "a2-highgpu-1g";
    config.image = "debian-12";
    config.network = "default";
    config.subnetwork = "us-central1-sub";
    config.service_account = "burst@example-churn.iam.gserviceaccount.com";
    config.spot = true;
    config.seed_peer = "10.0.0.1:7777";

    const auto parsed = GcpConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(GcpConfig, DefaultsAreFilledIn) {
    const GcpConfig config = sample_config();
    EXPECT_EQ(config.pool_name, "zetta-burst");              // default MIG name
    EXPECT_EQ(config.instance_template, "zetta-burst-template");
    EXPECT_FALSE(config.spot);
}

TEST(GcpConfig, RejectsMalformedDocuments) {
    EXPECT_FALSE(GcpConfig::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(GcpConfig::from_json("not-an-object").has_value());

    // Missing each required field in turn.
    for (const char* missing : {"project_id", "zone"}) {
        nlohmann::json document = sample_config().to_json();
        document.erase(missing);
        EXPECT_FALSE(GcpConfig::from_json(document).has_value()) << missing;
    }

    // Empty required field.
    nlohmann::json empty_required = sample_config().to_json();
    empty_required["zone"] = "";
    EXPECT_FALSE(GcpConfig::from_json(empty_required).has_value());

    // Wrong-typed fields.
    nlohmann::json wrong_project = sample_config().to_json();
    wrong_project["project_id"] = 42;
    EXPECT_FALSE(GcpConfig::from_json(wrong_project).has_value());

    nlohmann::json wrong_spot = sample_config().to_json();
    wrong_spot["spot"] = "yes";
    EXPECT_FALSE(GcpConfig::from_json(wrong_spot).has_value());
}

// ---- GcpProvisioner lifecycle (against a fake control plane) ----------------

TEST(GcpProvisioner, ProvisionBringsUpNodesAndMintsIdentities) {
    FakeControlPlane control_plane;
    std::vector<ProvisionedNode> minted;
    int revoke_count = 0;

    GcpProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };
    identity.revoke = [&] { ++revoke_count; };

    GcpProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(control_plane.bring_up_calls, 1);
    // Identities minted for exactly the nodes that came up — so they can join.
    EXPECT_EQ(minted, result.nodes);
    EXPECT_EQ(revoke_count, 0);  // nothing torn down yet
}

TEST(GcpProvisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeControlPlane control_plane;
    control_plane.capacity = 2;  // the pool can only bring up two
    std::vector<ProvisionedNode> minted;

    GcpProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };

    GcpProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{5, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(minted.size(), 2u);  // mint whatever actually came up
}

TEST(GcpProvisioner, ZeroCountIsASatisfiedNoOp) {
    FakeControlPlane control_plane;
    int mint_calls = 0;

    GcpProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    GcpProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(control_plane.bring_up_calls, 0);  // control plane untouched
    EXPECT_EQ(mint_calls, 0);                    // identity untouched
}

TEST(GcpProvisioner, NoMintWhenNothingCameUp) {
    FakeControlPlane control_plane;
    control_plane.capacity = 0;  // nothing can come up
    int mint_calls = 0;

    GcpProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    GcpProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(mint_calls, 0);  // nothing to mint for
}

TEST(GcpProvisioner, TeardownRevokesIdentitiesWhenPoolDrains) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    GcpProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) {};
    identity.revoke = [&] { ++revoke_count; };

    GcpProvisioner provisioner{control_plane, sample_config(), identity};
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

TEST(GcpProvisioner, TeardownOfUnknownNodeIsAnIdempotentNoOp) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    GcpProvisioner::IdentityHooks identity;
    identity.revoke = [&] { ++revoke_count; };

    GcpProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{1, gpu_spec()});

    // An unknown id releases nothing and does not fire revoke (the pool is not
    // empty, and no node was actually released).
    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(revoke_count, 0);
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
}

TEST(GcpProvisioner, ProvisionedReflectsLiveCapacity) {
    FakeControlPlane control_plane;
    GcpProvisioner provisioner{control_plane, sample_config()};  // no identity wired
    provisioner.provision(ProvisionRequest{3, gpu_spec()});
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // A Spot preemption removes a node without any teardown call — provisioned()
    // re-reads the control plane, so it shrinks to match.
    control_plane.pool.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
}

TEST(GcpProvisioner, WorksWithNoIdentityHooks) {
    FakeControlPlane control_plane;
    GcpProvisioner provisioner{control_plane, sample_config()};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, gpu_spec()});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(provisioner.teardown("burst-0"));  // drains to one, no revoke hook
    EXPECT_TRUE(provisioner.teardown("burst-1"));  // drains to empty, still fine
}

// ---- "with bulk identity": compose with a real BurstIdentityManager ---------

TEST(GcpProvisioner, BurstWithBulkIdentityLifecycle) {
    TempDir tmp;
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    Node node{cfg};
    node.start();
    ASSERT_EQ(node.principal_count(), 0u);

    FakeControlPlane control_plane;
    BurstIdentityManager identities{node};
    const std::string job = "job-churn-gcp";

    // Production wiring: bind the IdentityHooks to the BurstIdentityManager for the
    // job — mint a short-lived, job-scoped credential per burst node, revoke the
    // whole job when the pool drains.
    GcpProvisioner::IdentityHooks hooks;
    hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
        Scope scope;
        scope.grant(ResourceKind::Project, "churn");
        CapabilitySet caps;
        caps.grant(CapabilityDomain::Orchestration, "run", "2999-01-01T00:00:00.000Z");
        identities.mint(job, nodes, scope, caps);
    };
    hooks.revoke = [&] { identities.revoke(job); };

    GcpProvisioner provisioner{control_plane, sample_config(), hooks};

    // Burst 3 → one service principal + key minted per node so each can join.
    const ProvisionResult burst =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});
    ASSERT_EQ(burst.nodes.size(), 3u);
    EXPECT_EQ(node.principal_count(), 3u);
    EXPECT_EQ(identities.minted(job).size(), 3u);

    // Tear the burst down node by node; the last teardown drains the pool and
    // bulk-revokes the job's identities.
    for (const ProvisionedNode& up : provisioner.provisioned()) {
        EXPECT_TRUE(provisioner.teardown(up.node_id));
    }
    EXPECT_TRUE(provisioner.provisioned().empty());
    EXPECT_EQ(node.principal_count(), 0u);
    EXPECT_TRUE(identities.minted(job).empty());

    node.stop();
}

// ---- GcpCliControlPlane reference binding (against a fake command runner) ----

namespace {

// A canned `gcloud … list-instances --format json` output: two ProvisionedNode-
// shaped objects (a production binding projects MIG instances into this shape).
std::string two_node_listing() {
    return R"([
      {"node_id":"vm0","address":"10.2.0.1:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}},
      {"node_id":"vm1","address":"10.2.0.2:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}}
    ])";
}

}  // namespace

TEST(GcpCliControlPlane, BringUpEnsuresTemplateCreatesMigThenLists) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;  // exit_code 0 by default
    };

    GcpCliControlPlane control_plane{runner};
    GcpConfig config = sample_config();
    config.machine_type = "a2-highgpu-1g";
    config.image = "debian-12";
    config.service_account = "burst@example-churn.iam.gserviceaccount.com";

    const ProvisionResult result =
        control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 2u);
    EXPECT_EQ(result.nodes[0].node_id, "vm0");

    // The instance template carries the per-node shape.
    const std::vector<std::string>* tmpl = call_with(calls, {"instance-templates", "create"});
    ASSERT_NE(tmpl, nullptr);
    EXPECT_TRUE(has_token(*tmpl, "zetta-burst-template"));
    EXPECT_TRUE(has_flag(*tmpl, "--project", "example-churn"));
    EXPECT_TRUE(has_flag(*tmpl, "--machine-type", "a2-highgpu-1g"));
    EXPECT_TRUE(has_flag(*tmpl, "--image-family", "debian-12"));
    EXPECT_TRUE(has_flag(*tmpl, "--service-account",
                         "burst@example-churn.iam.gserviceaccount.com"));
    EXPECT_FALSE(has_token(*tmpl, "--provisioning-model"));  // not a spot config

    // The MIG create carries the pool identity, zone, template, and count.
    const std::vector<std::string>* create = call_with(calls, {"managed", "create"});
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(has_token(*create, "zetta-burst"));
    EXPECT_TRUE(has_flag(*create, "--project", "example-churn"));
    EXPECT_TRUE(has_flag(*create, "--zone", "us-central1-a"));
    EXPECT_TRUE(has_flag(*create, "--template", "zetta-burst-template"));
    EXPECT_TRUE(has_flag(*create, "--size", "2"));

    // The MIG is resized to the count (idempotent create-or-resize), then listed.
    const std::vector<std::string>* resize = call_with(calls, {"managed", "resize"});
    ASSERT_NE(resize, nullptr);
    EXPECT_TRUE(has_flag(*resize, "--size", "2"));
    EXPECT_NE(call_with(calls, {"list-instances"}), nullptr);
}

TEST(GcpCliControlPlane, SpotConfigAddsSpotAndSeedFlagsToTheTemplate) {
    std::vector<std::vector<std::string>> calls;
    std::string startup_seen;
    bool startup_private = false;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        // The startup-script arrives through --metadata-from-file; read it while
        // the command runs (the file is removed afterwards).
        for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
            const std::string prefix = "startup-script=";
            if (argv[index] == "--metadata-from-file" &&
                argv[index + 1].starts_with(prefix)) {
                const std::string path = argv[index + 1].substr(prefix.size());
                startup_seen = zetta::test::read_whole_file(path);
                startup_private = zetta::test::owner_only(path);
            }
        }
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    GcpConfig config = sample_config();
    config.spot = true;
    config.seed_peer = "10.0.0.1:7777";
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    const std::vector<std::string>* tmpl = call_with(calls, {"instance-templates", "create"});
    ASSERT_NE(tmpl, nullptr);
    EXPECT_TRUE(has_flag(*tmpl, "--provisioning-model", "SPOT"));
    EXPECT_TRUE(has_token(*tmpl, "--instance-termination-action"));
    // #1316: the startup-script is the node bootstrap script. It names the seed
    // and the Secret Manager secret, never the token; the node's access token
    // needs the cloud-platform scope to read it.
    EXPECT_FALSE(zetta::test::any_argv_contains(calls, config.join_token));
    EXPECT_FALSE(has_token(*tmpl, "--metadata"));
    EXPECT_TRUE(has_flag(*tmpl, "--scopes", "cloud-platform"));
    EXPECT_TRUE(startup_seen.starts_with("#!/bin/sh\n"));
    EXPECT_NE(startup_seen.find("ZETTA_SEED_PEER=10.0.0.1:7777"), std::string::npos);
    EXPECT_NE(startup_seen.find("TOKEN_STORE=gcp-secret-manager"), std::string::npos);
    EXPECT_NE(startup_seen.find("TOKEN_SECRET=zetta-burst-cluster-token"), std::string::npos);
    EXPECT_NE(startup_seen.find("TOKEN_CONTAINER=example-churn"), std::string::npos);
    EXPECT_EQ(startup_seen.find(config.join_token), std::string::npos);
    EXPECT_TRUE(startup_private);
}

// #1316: on a secure cluster the token goes to Secret Manager before the
// template, through a private --data-file, never argv.
TEST(GcpCliControlPlane, StoresTheJoinTokenInSecretManagerFirst) {
    std::vector<std::vector<std::string>> calls;
    std::string data_seen;
    bool data_private = false;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
            if (argv[index] == "--data-file") {
                data_seen = zetta::test::read_whole_file(argv[index + 1]);
                data_private = zetta::test::owner_only(argv[index + 1]);
            }
        }
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    GcpConfig config = sample_config();
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    EXPECT_TRUE(control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()}).satisfied);

    ASSERT_FALSE(calls.empty());
    const std::vector<std::string>& create = calls.front();
    EXPECT_TRUE(has_token(create, "secrets"));
    EXPECT_TRUE(has_token(create, "create"));
    EXPECT_TRUE(has_token(create, "zetta-burst-cluster-token"));
    EXPECT_TRUE(has_flag(create, "--project", "example-churn"));
    EXPECT_TRUE(has_flag(create, "--replication-policy", "automatic"));
    EXPECT_EQ(data_seen, config.join_token);
    EXPECT_TRUE(data_private);
    EXPECT_EQ(call_with(calls, {"versions", "add"}), nullptr);  // created, not re-added
    EXPECT_FALSE(zetta::test::any_argv_contains(calls, config.join_token));
}

// A re-burst: the secret exists, so the token is added as its latest version.
TEST(GcpCliControlPlane, AReBurstAddsASecretVersion) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "secrets") && has_token(argv, "create")) {
            result.exit_code = 1;
            result.stderr_text = "ALREADY_EXISTS";
        }
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    GcpConfig config = sample_config();
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    EXPECT_TRUE(control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()}).satisfied);

    const std::vector<std::string>* add = call_with(calls, {"versions", "add"});
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE(has_token(*add, "zetta-burst-cluster-token"));
    EXPECT_TRUE(has_flag(*add, "--project", "example-churn"));
    EXPECT_TRUE(has_token(*add, "--data-file"));
}

// Fail closed: when Secret Manager refuses the token, no template or MIG is
// touched and the token never falls back to metadata.
TEST(GcpCliControlPlane, FailsClosedWhenSecretManagerRefusesTheToken) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "secrets")) {
            result.exit_code = 1;
            result.stderr_text = "PERMISSION_DENIED: secretmanager.versions.add";
        }
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    GcpConfig config = sample_config();
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const ProvisionResult result = control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_NE(result.detail.find("PERMISSION_DENIED"), std::string::npos);
    EXPECT_EQ(call_with(calls, {"instance-templates", "create"}), nullptr);
    EXPECT_EQ(call_with(calls, {"managed", "create"}), nullptr);
}

TEST(GcpCliControlPlane, BringUpIsBestEffortWhenFewerInstancesComeUp) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    // Ask for three but the listing only reports two.
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
}

TEST(GcpCliControlPlane, BringUpReportsCreateAndResizeFailure) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        // Both the MIG create and the resize fail → the pool can't reach N.
        if (has_token(argv, "create") && has_token(argv, "managed")) {
            result.exit_code = 1;
            result.stderr_text = "quota exceeded";
        }
        if (has_token(argv, "resize")) result.exit_code = 1;
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_NE(result.detail.find("quota exceeded"), std::string::npos);
}

TEST(GcpCliControlPlane, BringUpSucceedsWhenResizeScalesAnExistingMig) {
    // A re-burst: the MIG already exists so `create` fails, but `resize` scales it —
    // the pool is still ready (idempotent create-or-resize).
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "create") && has_token(argv, "managed")) {
            result.exit_code = 1;  // already exists
            result.stderr_text = "already exists";
        }
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;  // resize succeeds (exit 0)
    };

    GcpCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
}

TEST(GcpCliControlPlane, DestroyBuildsDeleteInstancesAndReportsOutcome) {
    std::vector<std::string> seen;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        seen = argv;
        CommandResult result;
        // Fail the delete for one specific name to exercise the idempotent false.
        if (has_flag(argv, "--instances", "missing")) result.exit_code = 2;
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.destroy(sample_config(), "vm0"));
    EXPECT_TRUE(has_token(seen, "delete-instances"));
    EXPECT_TRUE(has_flag(seen, "--instances", "vm0"));
    EXPECT_TRUE(has_token(seen, "--quiet"));

    EXPECT_FALSE(control_plane.destroy(sample_config(), "missing"));  // non-zero → false
}

TEST(GcpCliControlPlane, ListParsesInstancesAndSkipsMalformed) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        // One valid node, one missing node_id (rejected at the boundary).
        result.stdout_text = R"([
          {"node_id":"vm0","address":"10.2.0.1:7777",
           "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}},
          {"address":"10.2.0.2:7777",
           "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}}
        ])";
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    const std::vector<ProvisionedNode> nodes = control_plane.list(sample_config());
    ASSERT_EQ(nodes.size(), 1u);  // the malformed entry is skipped
    EXPECT_EQ(nodes.front().node_id, "vm0");
}

TEST(GcpCliControlPlane, ListReturnsEmptyOnCommandFailure) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        result.exit_code = 1;  // `gcloud` failed
        return result;
    };

    GcpCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.list(sample_config()).empty());
}
