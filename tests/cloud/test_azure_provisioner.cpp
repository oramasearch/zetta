#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/azure_cli_control_plane.hpp"
#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::AzCliControlPlane;
using zetta::AzureConfig;
using zetta::AzureControlPlane;
using zetta::AzureProvisioner;
using zetta::CommandResult;
using zetta::CommandRunner;
using zetta::Gpu;
using zetta::GpuKind;
using zetta::NodeCapability;
using zetta::ProvisionedNode;
using zetta::ProvisionRequest;
using zetta::ProvisionResult;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

AzureConfig sample_config() {
    AzureConfig config;
    config.subscription_id = "sub-123";
    config.resource_group = "zetta-rg";
    config.location = "eastus";
    return config;
}

NodeCapability gpu_spec() {
    NodeCapability capability;
    capability.cores = 8;
    capability.memory_bytes = 64 * kGiB;
    capability.gpu = Gpu{GpuKind::Cuda, 312.0, "A100"};
    return capability;
}

// A fake AzureControlPlane modelling a burst pool: bring_up scales the pool to the
// requested count (bounded by `capacity`, to exercise best-effort), destroy
// removes one node, list returns what is up.
class FakeControlPlane : public AzureControlPlane {
public:
    std::size_t capacity = 1000;          // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;    // what is currently up
    int bring_up_calls = 0;

    ProvisionResult bring_up(const AzureConfig& /*config*/,
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

    bool destroy(const AzureConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const AzureConfig& /*config*/) const override {
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

// The `az vmss create` call among captured calls (bring_up now also runs an
// `az group create` first — the two share the "create" token, so match on "vmss").
const std::vector<std::string>& vmss_create_call(
    const std::vector<std::vector<std::string>>& calls) {
    static const std::vector<std::string> none;
    for (const std::vector<std::string>& argv : calls) {
        if (has_token(argv, "vmss") && has_token(argv, "create")) return argv;
    }
    return none;
}

}  // namespace

// ---- AzureConfig JSON boundary ---------------------------------------------

TEST(AzureConfig, RoundTripsMinimalConfig) {
    const AzureConfig config = sample_config();
    const auto parsed = AzureConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(AzureConfig, RoundTripsFullConfig) {
    AzureConfig config = sample_config();
    config.pool_name = "churn-burst";
    config.vm_size = "Standard_NC24ads_A100_v4";
    config.image = "Ubuntu2204";
    config.spot = true;
    config.max_price = 1.5;
    config.seed_peer = "10.0.0.1:7777";
    config.managed_identity = "/subscriptions/sub-123/.../burst-mi";
    config.tenant_id = "tenant-abc";

    const auto parsed = AzureConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(AzureConfig, DefaultsAreFilledIn) {
    const AzureConfig config = sample_config();
    EXPECT_EQ(config.pool_name, "zetta-burst");  // default name
    EXPECT_FALSE(config.spot);
    EXPECT_DOUBLE_EQ(config.max_price, -1.0);     // Azure: up to on-demand
}

TEST(AzureConfig, RejectsMalformedDocuments) {
    EXPECT_FALSE(AzureConfig::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(AzureConfig::from_json("not-an-object").has_value());

    // Missing each required field in turn.
    for (const char* missing : {"subscription_id", "resource_group", "location"}) {
        nlohmann::json document = sample_config().to_json();
        document.erase(missing);
        EXPECT_FALSE(AzureConfig::from_json(document).has_value()) << missing;
    }

    // Empty required field.
    nlohmann::json empty_required = sample_config().to_json();
    empty_required["location"] = "";
    EXPECT_FALSE(AzureConfig::from_json(empty_required).has_value());

    // Wrong-typed fields.
    nlohmann::json wrong_spot = sample_config().to_json();
    wrong_spot["spot"] = "yes";
    EXPECT_FALSE(AzureConfig::from_json(wrong_spot).has_value());

    nlohmann::json wrong_price = sample_config().to_json();
    wrong_price["max_price"] = "cheap";
    EXPECT_FALSE(AzureConfig::from_json(wrong_price).has_value());

    // max_price below the Azure floor of -1.
    nlohmann::json low_price = sample_config().to_json();
    low_price["max_price"] = -2.0;
    EXPECT_FALSE(AzureConfig::from_json(low_price).has_value());
}

// ---- AzureProvisioner lifecycle (against a fake control plane) --------------

TEST(AzureProvisioner, ProvisionBringsUpNodesAndMintsIdentities) {
    FakeControlPlane control_plane;
    std::vector<ProvisionedNode> minted;
    int revoke_count = 0;

    AzureProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };
    identity.revoke = [&] { ++revoke_count; };

    AzureProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(control_plane.bring_up_calls, 1);
    // Identities minted for exactly the nodes that came up — so they can join.
    EXPECT_EQ(minted, result.nodes);
    EXPECT_EQ(revoke_count, 0);  // nothing torn down yet
}

TEST(AzureProvisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeControlPlane control_plane;
    control_plane.capacity = 2;  // the pool can only bring up two
    std::vector<ProvisionedNode> minted;

    AzureProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };

    AzureProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{5, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(minted.size(), 2u);  // mint whatever actually came up
}

// set_placement steers the next burst: region → Azure location, instance type →
// vm_size. An empty dimension is left unchanged (the GUI's editable placement).
TEST(AzureProvisioner, SetPlacementUpdatesRegionAndSize) {
    FakeControlPlane control_plane;
    AzureProvisioner provisioner{control_plane, sample_config()};

    provisioner.set_placement("westus2", "Standard_D4s_v5");
    EXPECT_EQ(provisioner.config().location, "westus2");
    EXPECT_EQ(provisioner.config().vm_size, "Standard_D4s_v5");

    // An empty dimension leaves that field unchanged.
    provisioner.set_placement("", "Standard_D8s_v5");
    EXPECT_EQ(provisioner.config().location, "westus2");           // kept
    EXPECT_EQ(provisioner.config().vm_size, "Standard_D8s_v5");    // updated
}

TEST(AzureProvisioner, ZeroCountIsASatisfiedNoOp) {
    FakeControlPlane control_plane;
    int mint_calls = 0;

    AzureProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    AzureProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(control_plane.bring_up_calls, 0);  // control plane untouched
    EXPECT_EQ(mint_calls, 0);                    // identity untouched
}

TEST(AzureProvisioner, NoMintWhenNothingCameUp) {
    FakeControlPlane control_plane;
    control_plane.capacity = 0;  // nothing can come up
    int mint_calls = 0;

    AzureProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    AzureProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(mint_calls, 0);  // nothing to mint for
}

TEST(AzureProvisioner, TeardownRevokesIdentitiesWhenPoolDrains) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    AzureProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) {};
    identity.revoke = [&] { ++revoke_count; };

    AzureProvisioner provisioner{control_plane, sample_config(), identity};
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

TEST(AzureProvisioner, TeardownOfUnknownNodeIsAnIdempotentNoOp) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    AzureProvisioner::IdentityHooks identity;
    identity.revoke = [&] { ++revoke_count; };

    AzureProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{1, gpu_spec()});

    // An unknown id releases nothing and does not fire revoke (the pool is not
    // empty, and no node was actually released).
    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(revoke_count, 0);
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
}

TEST(AzureProvisioner, ProvisionedReflectsLiveCapacity) {
    FakeControlPlane control_plane;
    AzureProvisioner provisioner{control_plane, sample_config()};  // no identity wired
    provisioner.provision(ProvisionRequest{3, gpu_spec()});
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // A Spot eviction removes a node without any teardown call — provisioned()
    // re-reads the control plane, so it shrinks to match.
    control_plane.pool.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
}

TEST(AzureProvisioner, WorksWithNoIdentityHooks) {
    FakeControlPlane control_plane;
    AzureProvisioner provisioner{control_plane, sample_config()};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, gpu_spec()});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(provisioner.teardown("burst-0"));  // drains to one, no revoke hook
    EXPECT_TRUE(provisioner.teardown("burst-1"));  // drains to empty, still fine
}

// ---- AzCliControlPlane reference binding (against a fake command runner) ----

namespace {

// A canned `az vmss list-instances` output: two ProvisionedNode-shaped objects.
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

TEST(AzCliControlPlane, BringUpBuildsFlexibleCreateCommand) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;  // exit_code 0 by default
    };

    AzCliControlPlane control_plane{runner};
    AzureConfig config = sample_config();
    config.vm_size = "Standard_NC24ads_A100_v4";
    config.image = "Ubuntu2204";

    const ProvisionResult result =
        control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    // A successful placement is satisfied; the VMs join by gossip, not list().
    EXPECT_TRUE(result.satisfied);

    // The create call carries the pool identity, location, count, and SKU/image.
    // The resource group is the first free `<base>-NN` (base = config.resource_group).
    ASSERT_FALSE(calls.empty());
    const std::vector<std::string>& create = vmss_create_call(calls);
    EXPECT_TRUE(has_token(create, "create"));
    EXPECT_TRUE(has_flag(create, "--subscription", "sub-123"));
    EXPECT_TRUE(has_flag(create, "--resource-group", "zetta-rg-01"));
    EXPECT_TRUE(has_flag(create, "--name", "zetta-burst"));
    EXPECT_TRUE(has_flag(create, "--location", "eastus"));
    EXPECT_TRUE(has_flag(create, "--orchestration-mode", "Flexible"));
    EXPECT_TRUE(has_flag(create, "--instance-count", "2"));
    EXPECT_TRUE(has_flag(create, "--vm-sku", "Standard_NC24ads_A100_v4"));
    EXPECT_TRUE(has_flag(create, "--image", "Ubuntu2204"));
    // Flexible orchestration requires these to actually deploy (a missing fault
    // domain count fails create) — see docs/research/azure-cli-compute.md §2.7.
    EXPECT_TRUE(has_flag(create, "--platform-fault-domain-count", "1"));
    EXPECT_TRUE(has_flag(create, "--single-placement-group", "false"));
    EXPECT_TRUE(has_flag(create, "--admin-username", "azureuser"));
    EXPECT_TRUE(has_token(create, "--generate-ssh-keys"));
    EXPECT_FALSE(has_token(create, "--priority"));  // not a spot config
}

// When the config pins no image / SKU, bring_up defaults to the VMSS Flexible
// quickstart's values so a first burst deploys instead of failing on a missing
// image (docs/research/azure-cli-compute.md §2.7).
TEST(AzCliControlPlane, DefaultsImageAndSkuForFlexibleCreate) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    AzCliControlPlane control_plane{runner};
    // sample_config() pins neither image nor vm_size.
    control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    const std::vector<std::string>& create = vmss_create_call(calls);
    EXPECT_TRUE(has_flag(create, "--image", "Ubuntu2204"));
    EXPECT_TRUE(has_flag(create, "--vm-sku", "Standard_B2s"));
    EXPECT_TRUE(has_flag(create, "--platform-fault-domain-count", "1"));
}

TEST(AzCliControlPlane, SpotConfigAddsSpotFlags) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    AzCliControlPlane control_plane{runner};
    AzureConfig config = sample_config();
    config.spot = true;
    config.max_price = 1.25;
    config.seed_peer = "10.0.0.1:7777";

    control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    const std::vector<std::string>& create = vmss_create_call(calls);
    EXPECT_TRUE(has_flag(create, "--priority", "Spot"));
    EXPECT_TRUE(has_token(create, "--eviction-policy"));
    EXPECT_TRUE(has_token(create, "--max-price"));
    EXPECT_TRUE(has_flag(create, "--custom-data", "ZETTA_SEED_PEER=10.0.0.1:7777"));
}

// A successful create is satisfied: the scale set placed the requested VMs, which
// then join the cluster by gossip (they aren't necessarily visible via list-
// instances yet). Reporting keys off placement, not an immediate instance count.
TEST(AzCliControlPlane, BringUpSatisfiedOnSuccessfulPlacement) {
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;  // create/group succeed (exit 0) by default
    };

    AzCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    EXPECT_FALSE(result.detail.empty());
}

// When a SKU has no live capacity (SkuNotAvailable / Capacity Restrictions), the
// create rolls to the next candidate SKU — deleting the failed scale set first so
// the stable pool name is free — and reports the SKU that actually placed. Azure
// has no capacity API, so retry-with-fallback is the only reliable placement path.
TEST(AzCliControlPlane, BringUpFallsBackToNextSkuOnCapacity) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "vmss") && has_token(argv, "create") &&
            has_flag(argv, "--vm-sku", "Standard_B2s")) {
            // The first candidate (default) has no capacity in this region.
            result.exit_code = 1;
            result.stderr_text =
                "ERROR: SkuNotAvailable: ... Capacity Restrictions: Standard_B2s "
                "... not available in location 'eastus'.";
        } else if (has_token(argv, "list-instances")) {
            result.stdout_text = two_node_listing();
        }
        return result;  // the second SKU's create + everything else succeed
    };

    AzCliControlPlane control_plane{runner};
    // sample_config() pins no vm_size, so candidates start with Standard_B2s.
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{1, gpu_spec()});

    EXPECT_TRUE(result.satisfied);

    // It attempted the B2s create, deleted the failed set, then created a fallback.
    bool tried_b2s = false, deleted = false, placed_fallback = false;
    for (const std::vector<std::string>& argv : calls) {
        if (has_token(argv, "vmss") && has_token(argv, "create") &&
            has_flag(argv, "--vm-sku", "Standard_B2s")) {
            tried_b2s = true;
        }
        if (has_token(argv, "vmss") && has_token(argv, "delete")) deleted = true;
        if (has_token(argv, "vmss") && has_token(argv, "create") &&
            !has_flag(argv, "--vm-sku", "Standard_B2s")) {
            placed_fallback = true;
        }
    }
    EXPECT_TRUE(tried_b2s);
    EXPECT_TRUE(deleted);
    EXPECT_TRUE(placed_fallback);
}

TEST(AzCliControlPlane, BringUpReportsCreateFailure) {
    // Group create succeeds; the vmss create fails (quota) — the reported failure.
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "vmss") && has_token(argv, "create")) {
            result.exit_code = 1;
            result.stderr_text = "quota exceeded";
        }
        return result;
    };

    AzCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_NE(result.detail.find("quota exceeded"), std::string::npos);
}

// bring_up creates the resource group (idempotent) BEFORE the vmss create, so a
// first burst into a subscription with no pre-provisioned group just works instead
// of failing with ResourceGroupNotFound. The group name is a fresh `<base>-NN`
// picked to avoid a taken / deprovisioning name (checked via `az group exists`).
TEST(AzCliControlPlane, BringUpEnsuresResourceGroupFirst) {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "list-instances")) result.stdout_text = two_node_listing();
        return result;
    };

    AzCliControlPlane control_plane{runner};
    control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    // A `az group create` for the picked group name precedes any vmss create.
    ASSERT_FALSE(calls.empty());
    std::optional<std::vector<std::string>> group_create;
    std::size_t group_create_index = 0, vmss_create_index = calls.size();
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (has_token(calls[i], "group") && has_token(calls[i], "create")) {
            group_create = calls[i];
            group_create_index = i;
        } else if (has_token(calls[i], "vmss") && has_token(calls[i], "create") &&
                   vmss_create_index == calls.size()) {
            vmss_create_index = i;
        }
    }
    ASSERT_TRUE(group_create.has_value());
    EXPECT_LT(group_create_index, vmss_create_index);  // group before vmss
    EXPECT_TRUE(has_flag(*group_create, "--name", "zetta-rg-01"));
    EXPECT_TRUE(has_flag(*group_create, "--location", "eastus"));
    EXPECT_TRUE(has_flag(*group_create, "--subscription", "sub-123"));

    // A group-create failure aborts before the vmss create (no pool spawned).
    CommandRunner failing = [&](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (has_token(argv, "group") && has_token(argv, "create")) {
            result.exit_code = 1;
            result.stderr_text = "AuthorizationFailed";
        }
        return result;
    };
    AzCliControlPlane cp2{failing};
    const ProvisionResult aborted =
        cp2.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});
    EXPECT_FALSE(aborted.satisfied);
    EXPECT_TRUE(aborted.nodes.empty());
    EXPECT_NE(aborted.detail.find("AuthorizationFailed"), std::string::npos);
}

TEST(AzCliControlPlane, DestroyBuildsDeleteInstancesAndReportsOutcome) {
    std::vector<std::string> seen;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        seen = argv;
        CommandResult result;
        // Fail the delete for one specific id to exercise the idempotent false.
        if (has_flag(argv, "--instance-ids", "missing")) result.exit_code = 2;
        return result;
    };

    AzCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.destroy(sample_config(), "vm0"));
    EXPECT_TRUE(has_token(seen, "delete-instances"));
    EXPECT_TRUE(has_flag(seen, "--instance-ids", "vm0"));

    EXPECT_FALSE(control_plane.destroy(sample_config(), "missing"));  // non-zero → false
}

TEST(AzCliControlPlane, ListParsesInstancesAndSkipsMalformed) {
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

    AzCliControlPlane control_plane{runner};
    const std::vector<ProvisionedNode> nodes = control_plane.list(sample_config());
    ASSERT_EQ(nodes.size(), 1u);  // the malformed entry is skipped
    EXPECT_EQ(nodes.front().node_id, "vm0");
}

TEST(AzCliControlPlane, ListReturnsEmptyOnCommandFailure) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        result.exit_code = 1;  // `az` failed
        return result;
    };

    AzCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.list(sample_config()).empty());
}
