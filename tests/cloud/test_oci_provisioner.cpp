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
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/oci_cli_control_plane.hpp"
#include "zetta/cloud/oci_provisioner.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"

using zetta::cloud::BurstIdentityManager;
using zetta::governance::CapabilityDomain;
using zetta::governance::CapabilitySet;
using zetta::cloud::CommandResult;
using zetta::cloud::CommandRunner;
using zetta::cloud::Gpu;
using zetta::cloud::GpuKind;
using zetta::Node;
using zetta::cloud::NodeCapability;
using zetta::NodeConfig;
using zetta::cloud::OciCliControlPlane;
using zetta::cloud::OciConfig;
using zetta::cloud::OciControlPlane;
using zetta::cloud::OciProvisioner;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;
using zetta::governance::ResourceKind;
using zetta::governance::Scope;
using zetta::test::TempDir;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

OciConfig sample_config() {
    OciConfig config;
    config.compartment_id = "ocid1.compartment.oc1..aaaa";
    config.availability_domain = "Uocm:PHX-AD-1";
    config.subnet_id = "ocid1.subnet.oc1..bbbb";
    return config;
}

NodeCapability gpu_spec() {
    NodeCapability capability;
    capability.cores = 8;
    capability.memory_bytes = 64 * kGiB;
    capability.gpu = Gpu{GpuKind::Cuda, 312.0, "A100"};
    return capability;
}

// A fake OciControlPlane modelling a burst pool: bring_up scales the pool to the
// requested count (bounded by `capacity`, to exercise best-effort), destroy removes
// one node, list returns what is up.
class FakeControlPlane : public OciControlPlane {
public:
    std::size_t capacity = 1000;          // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;    // what is currently up
    int bring_up_calls = 0;

    ProvisionResult bring_up(const OciConfig& /*config*/,
                             const ProvisionRequest& request) override {
        ++bring_up_calls;
        const std::size_t target = std::min(request.count, capacity);
        pool.clear();
        for (std::size_t index = 0; index < target; ++index) {
            ProvisionedNode node;
            node.node_id = "ocid1.instance.oc1..n" + std::to_string(index);
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

    bool destroy(const OciConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const OciConfig& /*config*/) const override {
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

// The value immediately following `name` in argv (empty if absent) — for
// substring-checking a complex JSON flag value like --instance-details.
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

// Is this argv the pool-resolution call (instance-pool list, not list-instances)?
bool is_resolve(const std::vector<std::string>& argv) {
    return has_token(argv, "instance-pool") && has_token(argv, "list") &&
           !has_token(argv, "list-instances");
}

}  // namespace

// ---- OciConfig JSON boundary -----------------------------------------------

TEST(OciConfig, RoundTripsMinimalConfig) {
    const OciConfig config = sample_config();
    const auto parsed = OciConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(OciConfig, RoundTripsFullConfig) {
    OciConfig config = sample_config();
    config.region = "us-phoenix-1";
    config.pool_name = "churn-burst";
    config.shape = "VM.GPU.A10.1";
    config.ocpus = 30;
    config.memory_in_gbs = 240;
    config.image_id = "ocid1.image.oc1..cccc";
    config.spot = true;
    config.seed_peer = "10.0.0.1:7777";

    const auto parsed = OciConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(OciConfig, DefaultsAreFilledIn) {
    const OciConfig config = sample_config();
    EXPECT_EQ(config.pool_name, "zetta-burst");          // default pool name
    EXPECT_EQ(config.shape, "VM.Standard.E5.Flex");
    EXPECT_EQ(config.ocpus, 1u);
    EXPECT_EQ(config.memory_in_gbs, 16u);
    EXPECT_FALSE(config.spot);
}

TEST(OciConfig, RejectsMalformedDocuments) {
    EXPECT_FALSE(OciConfig::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(OciConfig::from_json("not-an-object").has_value());

    // Missing each required field in turn.
    for (const char* missing : {"compartment_id", "availability_domain", "subnet_id"}) {
        nlohmann::json document = sample_config().to_json();
        document.erase(missing);
        EXPECT_FALSE(OciConfig::from_json(document).has_value()) << missing;
    }

    // Empty required field.
    nlohmann::json empty_required = sample_config().to_json();
    empty_required["subnet_id"] = "";
    EXPECT_FALSE(OciConfig::from_json(empty_required).has_value());

    // Wrong-typed / negative fields.
    nlohmann::json wrong_compartment = sample_config().to_json();
    wrong_compartment["compartment_id"] = 42;
    EXPECT_FALSE(OciConfig::from_json(wrong_compartment).has_value());

    nlohmann::json negative_ocpus = sample_config().to_json();
    negative_ocpus["ocpus"] = -2;
    EXPECT_FALSE(OciConfig::from_json(negative_ocpus).has_value());

    nlohmann::json wrong_memory = sample_config().to_json();
    wrong_memory["memory_in_gbs"] = "lots";
    EXPECT_FALSE(OciConfig::from_json(wrong_memory).has_value());

    nlohmann::json wrong_spot = sample_config().to_json();
    wrong_spot["spot"] = "yes";
    EXPECT_FALSE(OciConfig::from_json(wrong_spot).has_value());
}

// ---- OciProvisioner lifecycle (against a fake control plane) ----------------

TEST(OciProvisioner, ProvisionBringsUpNodesAndMintsIdentities) {
    FakeControlPlane control_plane;
    std::vector<ProvisionedNode> minted;
    int revoke_count = 0;

    OciProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };
    identity.revoke = [&] { ++revoke_count; };

    OciProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(control_plane.bring_up_calls, 1);
    EXPECT_EQ(minted, result.nodes);  // minted for exactly what came up
    EXPECT_EQ(revoke_count, 0);
}

TEST(OciProvisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeControlPlane control_plane;
    control_plane.capacity = 2;
    std::vector<ProvisionedNode> minted;

    OciProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };

    OciProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{5, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(minted.size(), 2u);
}

TEST(OciProvisioner, ZeroCountIsASatisfiedNoOp) {
    FakeControlPlane control_plane;
    int mint_calls = 0;

    OciProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    OciProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(control_plane.bring_up_calls, 0);
    EXPECT_EQ(mint_calls, 0);
}

TEST(OciProvisioner, NoMintWhenNothingCameUp) {
    FakeControlPlane control_plane;
    control_plane.capacity = 0;
    int mint_calls = 0;

    OciProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    OciProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(mint_calls, 0);
}

TEST(OciProvisioner, TeardownRevokesIdentitiesWhenPoolDrains) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    OciProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) {};
    identity.revoke = [&] { ++revoke_count; };

    OciProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{2, gpu_spec()});
    ASSERT_EQ(provisioner.provisioned().size(), 2u);

    EXPECT_TRUE(provisioner.teardown("ocid1.instance.oc1..n0"));
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
    EXPECT_EQ(revoke_count, 0);

    EXPECT_TRUE(provisioner.teardown("ocid1.instance.oc1..n1"));
    EXPECT_TRUE(provisioner.provisioned().empty());
    EXPECT_EQ(revoke_count, 1);
}

TEST(OciProvisioner, TeardownOfUnknownNodeIsAnIdempotentNoOp) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    OciProvisioner::IdentityHooks identity;
    identity.revoke = [&] { ++revoke_count; };

    OciProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{1, gpu_spec()});

    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(revoke_count, 0);
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
}

TEST(OciProvisioner, ProvisionedReflectsLiveCapacity) {
    FakeControlPlane control_plane;
    OciProvisioner provisioner{control_plane, sample_config()};
    provisioner.provision(ProvisionRequest{3, gpu_spec()});
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // A preemptible reclaim removes a node without any teardown call.
    control_plane.pool.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
}

TEST(OciProvisioner, WorksWithNoIdentityHooks) {
    FakeControlPlane control_plane;
    OciProvisioner provisioner{control_plane, sample_config()};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, gpu_spec()});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(provisioner.teardown("ocid1.instance.oc1..n0"));
    EXPECT_TRUE(provisioner.teardown("ocid1.instance.oc1..n1"));
}

// ---- "with bulk identity": compose with a real BurstIdentityManager ---------

TEST(OciProvisioner, BurstWithBulkIdentityLifecycle) {
    TempDir tmp;
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    Node node{cfg};
    node.start();
    ASSERT_EQ(node.principal_count(), 0u);

    FakeControlPlane control_plane;
    BurstIdentityManager identities{node};
    const std::string job = "job-churn-oci";

    OciProvisioner::IdentityHooks hooks;
    hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
        Scope scope;
        scope.grant(ResourceKind::Project, "churn");
        CapabilitySet caps;
        caps.grant(CapabilityDomain::Orchestration, "run", "2999-01-01T00:00:00.000Z");
        identities.mint(job, nodes, scope, caps);
    };
    hooks.revoke = [&] { identities.revoke(job); };

    OciProvisioner provisioner{control_plane, sample_config(), hooks};

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

// ---- OciCliControlPlane reference binding (against a fake command runner) ----

namespace {

// A canned `oci ... list-instances` output: the OCI { "data": [ ... ] } envelope
// wrapping two ProvisionedNode-shaped objects (a production binding projects pool
// instances into this shape).
std::string two_node_listing() {
    return R"({"data":[
      {"node_id":"ocid1.instance.oc1..vm0","address":"10.2.0.1:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}},
      {"node_id":"ocid1.instance.oc1..vm1","address":"10.2.0.2:7777",
       "capability":{"cores":8,"memory_bytes":68719476736,
                     "gpu":{"kind":"cuda","throughput_tflops":312.0,"model":"A100"}}}
    ]})";
}

}  // namespace

TEST(OciCliControlPlane, BringUpCreatePathBuildsConfigPoolThenLists) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "";  // no existing pool → create path
        } else if (has_token(argv, "instance-configuration") && has_token(argv, "create")) {
            result.stdout_text = "ocid1.instanceconfiguration.oc1..cfg\n";
        } else if (has_token(argv, "instance-pool") && has_token(argv, "create")) {
            result.stdout_text = "ocid1.instancepool.oc1..pool\n";
        } else if (has_token(argv, "list-instances")) {
            result.stdout_text = two_node_listing();
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    OciConfig config = sample_config();
    config.image_id = "ocid1.image.oc1..img";

    const ProvisionResult result =
        control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 2u);
    EXPECT_EQ(result.nodes[0].node_id, "ocid1.instance.oc1..vm0");

    // The instance configuration carries the per-node launch shape as JSON.
    const std::vector<std::string>* cfg = call_with(calls, {"instance-configuration", "create"});
    ASSERT_NE(cfg, nullptr);
    const std::string details = flag_value(*cfg, "--instance-details");
    EXPECT_NE(details.find("VM.Standard.E5.Flex"), std::string::npos);
    EXPECT_NE(details.find("ocid1.image.oc1..img"), std::string::npos);
    EXPECT_NE(details.find("Uocm:PHX-AD-1"), std::string::npos);

    // The pool create references the config OCID and the requested size.
    const std::vector<std::string>* create = call_with(calls, {"instance-pool", "create"});
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(has_flag(*create, "--instance-configuration-id", "ocid1.instanceconfiguration.oc1..cfg"));
    EXPECT_TRUE(has_flag(*create, "--size", "2"));
    EXPECT_TRUE(has_flag(*create, "--display-name", "zetta-burst"));
    EXPECT_NE(call_with(calls, {"list-instances"}), nullptr);
}

TEST(OciCliControlPlane, BringUpResizePathUpdatesAnExistingPool) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "ocid1.instancepool.oc1..existing\n";  // pool exists → resize
        } else if (has_token(argv, "list-instances")) {
            result.stdout_text = two_node_listing();
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);

    // It resized rather than created — an update call, no instance-pool create.
    const std::vector<std::string>* update = call_with(calls, {"instance-pool", "update"});
    ASSERT_NE(update, nullptr);
    EXPECT_TRUE(has_flag(*update, "--instance-pool-id", "ocid1.instancepool.oc1..existing"));
    EXPECT_TRUE(has_flag(*update, "--size", "2"));
    EXPECT_EQ(call_with(calls, {"instance-pool", "create"}), nullptr);
}

TEST(OciCliControlPlane, SpotConfigAddsPreemptibleAndSeedToInstanceDetails) {
    std::vector<std::vector<std::string>> calls;
    std::string details;
    bool details_private = false;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        // The details carry the token, so they arrive as `file://<path>`; read the
        // file while the command runs.
        for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
            const std::string prefix = "file://";
            if (argv[index] == "--instance-details" && argv[index + 1].starts_with(prefix)) {
                const std::string path = argv[index + 1].substr(prefix.size());
                details = zetta::test::read_whole_file(path);
                details_private = zetta::test::owner_only(path);
            }
        }
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "";
        } else if (has_token(argv, "instance-pool") && has_token(argv, "create")) {
            result.stdout_text = "ocid1.instancepool.oc1..pool\n";
        } else if (has_token(argv, "list-instances")) {
            result.stdout_text = two_node_listing();
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    OciConfig config = sample_config();
    config.spot = true;
    config.seed_peer = "10.0.0.1:7777";
    // On a secure cluster the join token rides alongside the seed peer (#670),
    // both in the one cloud-init user_data value.
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    const std::vector<std::string>* cfg = call_with(calls, {"instance-configuration", "create"});
    ASSERT_NE(cfg, nullptr);
    // #770: the token is never an argv value.
    EXPECT_FALSE(zetta::test::any_argv_contains(calls, config.join_token));
    EXPECT_TRUE(details_private);
    EXPECT_NE(details.find("preemptibleInstanceConfig"), std::string::npos);
    EXPECT_NE(details.find("ZETTA_SEED_PEER=10.0.0.1:7777"), std::string::npos);
    EXPECT_NE(details.find("ZETTA_CLUSTER_TOKEN=0123456789abcdef0123456789abcdef"
                           "0123456789abcdef0123456789abcdef"),
              std::string::npos);
}

TEST(OciCliControlPlane, BringUpIsBestEffortWhenFewerInstancesComeUp) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "ocid1.instancepool.oc1..existing\n";
        } else if (has_token(argv, "list-instances")) {
            result.stdout_text = two_node_listing();
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    // Ask for three but the listing reports two.
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
}

TEST(OciCliControlPlane, BringUpReportsCreateFailure) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "";  // create path
        } else if (has_token(argv, "instance-pool") && has_token(argv, "create")) {
            result.exit_code = 1;
            result.stderr_text = "Out of host capacity";
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_NE(result.detail.find("Out of host capacity"), std::string::npos);
}

TEST(OciCliControlPlane, DestroyDetachesAndTerminatesTheInstance) {
    std::vector<std::string> detach_seen;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "ocid1.instancepool.oc1..pool\n";
        } else if (has_token(argv, "instance-pool-instance") && has_token(argv, "detach")) {
            detach_seen = argv;
            if (has_flag(argv, "--instance-id", "missing")) result.exit_code = 2;
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.destroy(sample_config(), "ocid1.instance.oc1..vm0"));
    EXPECT_TRUE(has_token(detach_seen, "detach"));
    EXPECT_TRUE(has_flag(detach_seen, "--instance-id", "ocid1.instance.oc1..vm0"));
    EXPECT_TRUE(has_flag(detach_seen, "--instance-pool-id", "ocid1.instancepool.oc1..pool"));
    EXPECT_TRUE(has_flag(detach_seen, "--is-auto-terminate", "true"));
    EXPECT_TRUE(has_flag(detach_seen, "--is-decrement-size", "true"));

    EXPECT_FALSE(control_plane.destroy(sample_config(), "missing"));  // non-zero → false
}

TEST(OciCliControlPlane, DestroyIsANoOpWhenNoPoolExists) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) result.stdout_text = "";  // no pool
        return result;
    };

    OciCliControlPlane control_plane{runner};
    EXPECT_FALSE(control_plane.destroy(sample_config(), "ocid1.instance.oc1..vm0"));
}

TEST(OciCliControlPlane, ListParsesInstancesAndSkipsMalformed) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "ocid1.instancepool.oc1..pool\n";
        } else if (has_token(argv, "list-instances")) {
            // One valid node, one missing node_id (rejected at the boundary).
            result.stdout_text = R"({"data":[
              {"node_id":"ocid1.instance.oc1..vm0","address":"10.2.0.1:7777",
               "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}},
              {"address":"10.2.0.2:7777",
               "capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}}
            ]})";
        }
        return result;
    };

    OciCliControlPlane control_plane{runner};
    const std::vector<ProvisionedNode> nodes = control_plane.list(sample_config());
    ASSERT_EQ(nodes.size(), 1u);  // the malformed entry is skipped
    EXPECT_EQ(nodes.front().node_id, "ocid1.instance.oc1..vm0");
}

TEST(OciCliControlPlane, ListReturnsEmptyWhenNoPoolOrCommandFails) {
    // No pool resolved → empty.
    CommandRunner no_pool = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) result.stdout_text = "";
        return result;
    };
    EXPECT_TRUE(OciCliControlPlane{no_pool}.list(sample_config()).empty());

    // Pool resolved but list-instances fails → empty.
    CommandRunner list_fails = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (is_resolve(argv)) {
            result.stdout_text = "ocid1.instancepool.oc1..pool\n";
        } else if (has_token(argv, "list-instances")) {
            result.exit_code = 1;  // `oci` failed
        }
        return result;
    };
    EXPECT_TRUE(OciCliControlPlane{list_fails}.list(sample_config()).empty());
}
