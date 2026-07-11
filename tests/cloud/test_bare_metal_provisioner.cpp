#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cloud/bare_metal_cli_control_plane.hpp"
#include "zetta/cloud/bare_metal_provisioner.hpp"
#include "zetta/cloud/burst_identity.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"

using zetta::cloud::BareMetalCliControlPlane;
using zetta::cloud::BareMetalConfig;
using zetta::cloud::BareMetalControlPlane;
using zetta::cloud::BareMetalProvisioner;
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
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;
using zetta::governance::ResourceKind;
using zetta::governance::Scope;
using zetta::test::TempDir;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

BareMetalConfig sample_config() {
    BareMetalConfig config;
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

// A fake BareMetalControlPlane modelling a burst pool: bring_up scales the pool to
// the requested count (bounded by `capacity`, to exercise best-effort), destroy
// removes one worker, list returns what is up.
class FakeControlPlane : public BareMetalControlPlane {
public:
    std::size_t capacity = 1000;          // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;    // what is currently up
    int bring_up_calls = 0;

    ProvisionResult bring_up(const BareMetalConfig& /*config*/,
                             const ProvisionRequest& request) override {
        ++bring_up_calls;
        const std::size_t target = std::min(request.count, capacity);
        pool.clear();
        for (std::size_t index = 0; index < target; ++index) {
            ProvisionedNode node;
            node.node_id = "zetta-burst-" + std::to_string(index);
            node.address = "172.17.0." + std::to_string(index + 1) + ":7777";
            node.capability = request.spec;
            pool.push_back(node);
        }
        ProvisionResult result;
        result.nodes = pool;
        result.satisfied = pool.size() >= request.count;
        result.detail = result.satisfied ? "scaled" : "best-effort short";
        return result;
    }

    bool destroy(const BareMetalConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const BareMetalConfig& /*config*/) const override {
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

// Build a newline-delimited-JSON listing of `count` ProvisionedNode-shaped objects,
// as `docker ps --format '{{json .}}'` emits.
std::string ndjson_listing(std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) {
        out += R"({"node_id":"container-)" + std::to_string(index) +
               R"(","address":"172.17.0.)" + std::to_string(index + 1) +
               R"(:7777","capability":{"cores":8,"memory_bytes":68719476736,)" +
               R"("gpu":{"kind":"none"}}})" + "\n";
    }
    return out;
}

// A stateful fake container runtime: `run` brings one worker up (optionally failing
// once the pool reaches `fail_at`), `ps` lists what is up as NDJSON, `rm` removes
// one. Records every argv.
struct FakeRuntime {
    std::vector<std::vector<std::string>> calls;
    std::size_t up = 0;
    int fail_at = -1;  // when >= 0, `run` fails once `up` reaches it

    CommandResult operator()(const std::vector<std::string>& argv) {
        calls.push_back(argv);
        CommandResult result;
        if (has_token(argv, "run")) {
            if (fail_at >= 0 && up >= static_cast<std::size_t>(fail_at)) {
                result.exit_code = 1;
                result.stderr_text = "image pull failed";
                return result;
            }
            result.stdout_text = "container-" + std::to_string(up);  // the new id
            ++up;
        } else if (has_token(argv, "ps")) {
            result.stdout_text = ndjson_listing(up);
        } else if (has_token(argv, "rm")) {
            if (up > 0) --up;
        }
        return result;
    }
};

}  // namespace

// ---- BareMetalConfig JSON boundary -----------------------------------------

TEST(BareMetalConfig, RoundTripsMinimalConfig) {
    const BareMetalConfig config = sample_config();
    const auto parsed = BareMetalConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(BareMetalConfig, RoundTripsFullConfig) {
    BareMetalConfig config = sample_config();
    config.runtime = "podman";
    config.pool_label = "churn-burst";
    config.name_prefix = "churn-worker";
    config.network = "host";
    config.gpus = true;
    config.seed_peer = "10.0.0.1:7777";

    const auto parsed = BareMetalConfig::from_json(config.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, config);
}

TEST(BareMetalConfig, DefaultsAreFilledIn) {
    const BareMetalConfig config = sample_config();
    EXPECT_EQ(config.runtime, "docker");
    EXPECT_EQ(config.pool_label, "zetta-burst");
    EXPECT_EQ(config.name_prefix, "zetta-burst");
    EXPECT_FALSE(config.gpus);
}

TEST(BareMetalConfig, RejectsMalformedDocuments) {
    EXPECT_FALSE(BareMetalConfig::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(BareMetalConfig::from_json("not-an-object").has_value());

    // Missing / empty required image.
    EXPECT_FALSE(BareMetalConfig::from_json(nlohmann::json::object()).has_value());
    EXPECT_FALSE(BareMetalConfig::from_json(nlohmann::json{{"image", ""}}).has_value());

    // Wrong-typed fields.
    EXPECT_FALSE(BareMetalConfig::from_json(nlohmann::json{{"image", 42}}).has_value());
    EXPECT_FALSE(BareMetalConfig::from_json(
        nlohmann::json{{"image", "x"}, {"runtime", 7}}).has_value());
    EXPECT_FALSE(BareMetalConfig::from_json(
        nlohmann::json{{"image", "x"}, {"gpus", "yes"}}).has_value());
}

// ---- BareMetalProvisioner lifecycle (against a fake control plane) ----------

TEST(BareMetalProvisioner, ProvisionBringsUpWorkersAndMintsIdentities) {
    FakeControlPlane control_plane;
    std::vector<ProvisionedNode> minted;
    int revoke_count = 0;

    BareMetalProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };
    identity.revoke = [&] { ++revoke_count; };

    BareMetalProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(control_plane.bring_up_calls, 1);
    EXPECT_EQ(minted, result.nodes);
    EXPECT_EQ(revoke_count, 0);
}

TEST(BareMetalProvisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeControlPlane control_plane;
    control_plane.capacity = 2;
    std::vector<ProvisionedNode> minted;

    BareMetalProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>& nodes) { minted = nodes; };

    BareMetalProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{5, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(minted.size(), 2u);
}

TEST(BareMetalProvisioner, ZeroCountIsASatisfiedNoOp) {
    FakeControlPlane control_plane;
    int mint_calls = 0;

    BareMetalProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    BareMetalProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(control_plane.bring_up_calls, 0);
    EXPECT_EQ(mint_calls, 0);
}

TEST(BareMetalProvisioner, NoMintWhenNothingCameUp) {
    FakeControlPlane control_plane;
    control_plane.capacity = 0;
    int mint_calls = 0;

    BareMetalProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) { ++mint_calls; };

    BareMetalProvisioner provisioner{control_plane, sample_config(), identity};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(mint_calls, 0);
}

TEST(BareMetalProvisioner, TeardownRevokesIdentitiesWhenPoolDrains) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    BareMetalProvisioner::IdentityHooks identity;
    identity.mint = [&](const std::vector<ProvisionedNode>&) {};
    identity.revoke = [&] { ++revoke_count; };

    BareMetalProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{2, gpu_spec()});
    ASSERT_EQ(provisioner.provisioned().size(), 2u);

    EXPECT_TRUE(provisioner.teardown("zetta-burst-0"));
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
    EXPECT_EQ(revoke_count, 0);

    EXPECT_TRUE(provisioner.teardown("zetta-burst-1"));
    EXPECT_TRUE(provisioner.provisioned().empty());
    EXPECT_EQ(revoke_count, 1);
}

TEST(BareMetalProvisioner, TeardownOfUnknownNodeIsAnIdempotentNoOp) {
    FakeControlPlane control_plane;
    int revoke_count = 0;

    BareMetalProvisioner::IdentityHooks identity;
    identity.revoke = [&] { ++revoke_count; };

    BareMetalProvisioner provisioner{control_plane, sample_config(), identity};
    provisioner.provision(ProvisionRequest{1, gpu_spec()});

    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(revoke_count, 0);
    EXPECT_EQ(provisioner.provisioned().size(), 1u);
}

TEST(BareMetalProvisioner, ProvisionedReflectsLiveState) {
    FakeControlPlane control_plane;
    BareMetalProvisioner provisioner{control_plane, sample_config()};
    provisioner.provision(ProvisionRequest{3, gpu_spec()});
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // A worker exits without any teardown call — list re-reads, so it shrinks.
    control_plane.pool.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
}

TEST(BareMetalProvisioner, WorksWithNoIdentityHooks) {
    FakeControlPlane control_plane;
    BareMetalProvisioner provisioner{control_plane, sample_config()};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, gpu_spec()});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(provisioner.teardown("zetta-burst-0"));
    EXPECT_TRUE(provisioner.teardown("zetta-burst-1"));
}

// ---- "with bulk identity": compose with a real BurstIdentityManager ---------

TEST(BareMetalProvisioner, BurstWithBulkIdentityLifecycle) {
    TempDir tmp;
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    Node node{cfg};
    node.start();
    ASSERT_EQ(node.principal_count(), 0u);

    FakeControlPlane control_plane;
    BurstIdentityManager identities{node};
    const std::string job = "job-churn-baremetal";

    BareMetalProvisioner::IdentityHooks hooks;
    hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
        Scope scope;
        scope.grant(ResourceKind::Project, "churn");
        CapabilitySet caps;
        caps.grant(CapabilityDomain::Orchestration, "run", "2999-01-01T00:00:00.000Z");
        identities.mint(job, nodes, scope, caps);
    };
    hooks.revoke = [&] { identities.revoke(job); };

    BareMetalProvisioner provisioner{control_plane, sample_config(), hooks};

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

// ---- BareMetalCliControlPlane reference binding (against a fake runtime) -----

TEST(BareMetalCliControlPlane, BringUpLaunchesTheShortfallThenLists) {
    FakeRuntime fake;
    CommandRunner runner = [&fake](const std::vector<std::string>& argv) {
        return fake(argv);
    };

    BareMetalCliControlPlane control_plane{runner};
    BareMetalConfig config = sample_config();
    config.seed_peer = "10.0.0.1:7777";
    // On a secure cluster the join token rides alongside the seed peer (#670) as a
    // second container env var.
    config.join_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    const ProvisionResult result =
        control_plane.bring_up(config, ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 2u);

    // Two `run -d` launches (the pool started empty), each carrying the label, a
    // unique name, the image, and the seed + token env.
    int run_calls = 0;
    for (const std::vector<std::string>& argv : fake.calls) {
        if (has_token(argv, "run")) {
            ++run_calls;
            EXPECT_TRUE(has_token(argv, "-d"));
            EXPECT_TRUE(has_flag(argv, "--label", "zetta-burst"));
            EXPECT_TRUE(has_token(argv, "--name"));
            EXPECT_TRUE(has_flag(argv, "--env", "ZETTA_SEED_PEER=10.0.0.1:7777"));
            EXPECT_TRUE(has_flag(
                argv, "--env",
                "ZETTA_CLUSTER_TOKEN=0123456789abcdef0123456789abcdef"
                "0123456789abcdef0123456789abcdef"));
            EXPECT_TRUE(has_token(argv, "ghcr.io/oramasearch/zetta:latest"));
        }
    }
    EXPECT_EQ(run_calls, 2);

    // The list call filters by the pool label as JSON.
    const std::vector<std::string>* ps = call_with(fake.calls, {"ps"});
    ASSERT_NE(ps, nullptr);
    EXPECT_TRUE(has_flag(*ps, "--filter", "label=zetta-burst"));
}

TEST(BareMetalCliControlPlane, ReBurstToSameCountLaunchesNothing) {
    FakeRuntime fake;
    fake.up = 2;  // two workers already up under the label
    CommandRunner runner = [&fake](const std::vector<std::string>& argv) {
        return fake(argv);
    };

    BareMetalCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{2, gpu_spec()});

    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    // Idempotent on the label: no new `run` was issued.
    EXPECT_EQ(call_with(fake.calls, {"run"}), nullptr);
}

TEST(BareMetalCliControlPlane, GpusFlagIsPassedWhenSet) {
    FakeRuntime fake;
    CommandRunner runner = [&fake](const std::vector<std::string>& argv) {
        return fake(argv);
    };

    BareMetalCliControlPlane control_plane{runner};
    BareMetalConfig config = sample_config();
    config.gpus = true;
    config.network = "host";

    control_plane.bring_up(config, ProvisionRequest{1, gpu_spec()});

    const std::vector<std::string>* run = call_with(fake.calls, {"run"});
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(has_flag(*run, "--gpus", "all"));
    EXPECT_TRUE(has_flag(*run, "--network", "host"));
}

TEST(BareMetalCliControlPlane, BringUpIsBestEffortWhenARunFails) {
    FakeRuntime fake;
    fake.fail_at = 1;  // the second `run` fails (only one comes up)
    CommandRunner runner = [&fake](const std::vector<std::string>& argv) {
        return fake(argv);
    };

    BareMetalCliControlPlane control_plane{runner};
    const ProvisionResult result =
        control_plane.bring_up(sample_config(), ProvisionRequest{3, gpu_spec()});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 1u);
    EXPECT_NE(result.detail.find("image pull failed"), std::string::npos);
}

TEST(BareMetalCliControlPlane, UsesTheConfiguredRuntime) {
    FakeRuntime fake;
    CommandRunner runner = [&fake](const std::vector<std::string>& argv) {
        return fake(argv);
    };

    BareMetalCliControlPlane control_plane{runner};
    BareMetalConfig config = sample_config();
    config.runtime = "podman";

    control_plane.bring_up(config, ProvisionRequest{1, gpu_spec()});

    // Every call is a `podman` call, not `docker`.
    ASSERT_FALSE(fake.calls.empty());
    for (const std::vector<std::string>& argv : fake.calls) {
        ASSERT_FALSE(argv.empty());
        EXPECT_EQ(argv.front(), "podman");
    }
}

TEST(BareMetalCliControlPlane, DestroyForceRemovesTheWorker) {
    std::vector<std::string> seen;
    CommandRunner runner = [&](const std::vector<std::string>& argv) -> CommandResult {
        seen = argv;
        CommandResult result;
        if (has_token(argv, "missing")) result.exit_code = 1;  // unknown id
        return result;
    };

    BareMetalCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.destroy(sample_config(), "container-0"));
    EXPECT_TRUE(has_token(seen, "rm"));
    EXPECT_TRUE(has_token(seen, "-f"));
    EXPECT_TRUE(has_token(seen, "container-0"));

    EXPECT_FALSE(control_plane.destroy(sample_config(), "missing"));  // non-zero → false
}

TEST(BareMetalCliControlPlane, ListParsesNdjsonAndSkipsMalformed) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        // NDJSON: one valid worker, one missing node_id (rejected at the boundary).
        result.stdout_text =
            R"({"node_id":"container-0","address":"172.17.0.1:7777","capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}})"
            "\n"
            R"({"address":"172.17.0.2:7777","capability":{"cores":8,"memory_bytes":68719476736,"gpu":{"kind":"none"}}})"
            "\n";
        return result;
    };

    BareMetalCliControlPlane control_plane{runner};
    const std::vector<ProvisionedNode> nodes = control_plane.list(sample_config());
    ASSERT_EQ(nodes.size(), 1u);  // the malformed line is skipped
    EXPECT_EQ(nodes.front().node_id, "container-0");
}

TEST(BareMetalCliControlPlane, ListReturnsEmptyOnCommandFailure) {
    CommandRunner runner = [&](const std::vector<std::string>&) -> CommandResult {
        CommandResult result;
        result.exit_code = 1;  // the runtime failed
        return result;
    };

    BareMetalCliControlPlane control_plane{runner};
    EXPECT_TRUE(control_plane.list(sample_config()).empty());
}
