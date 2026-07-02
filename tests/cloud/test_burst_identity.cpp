#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/burst_identity.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/util/timestamp.hpp"

using zetta::AuditAction;
using zetta::AzureConfig;
using zetta::AzureControlPlane;
using zetta::AzureProvisioner;
using zetta::BurstIdentity;
using zetta::BurstIdentityManager;
using zetta::CapabilityDomain;
using zetta::CapabilitySet;
using zetta::Gpu;
using zetta::GpuKind;
using zetta::Node;
using zetta::NodeCapability;
using zetta::NodeConfig;
using zetta::PrincipalKind;
using zetta::ProvisionedNode;
using zetta::ProvisionRequest;
using zetta::ProvisionResult;
using zetta::Provisioner;
using zetta::ResourceKind;
using zetta::Scope;
using zetta::test::TempDir;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

// The same capacity-bounded fake the provisioner seam test uses — so the burst
// identity flow is exercised against the real Provisioner abstraction.
class FakeProvisioner : public Provisioner {
public:
    explicit FakeProvisioner(std::size_t capacity) : capacity_(capacity) {}

    ProvisionResult provision(const ProvisionRequest& request) override {
        ProvisionResult result;
        for (std::size_t index = 0; index < request.count; ++index) {
            if (live_.size() >= capacity_) {
                result.satisfied = false;
                result.detail = "capacity exhausted";
                return result;
            }
            ProvisionedNode node;
            node.node_id = "burst-" + std::to_string(next_id_++);
            node.address = "10.0.0." + std::to_string(live_.size() + 1) + ":7777";
            node.capability = request.spec;
            live_.push_back(node);
            result.nodes.push_back(node);
        }
        result.satisfied = result.nodes.size() == request.count;
        result.detail = result.satisfied ? "ok" : "capacity exhausted";
        return result;
    }

    bool teardown(const std::string& node_id) override {
        for (auto it = live_.begin(); it != live_.end(); ++it) {
            if (it->node_id == node_id) {
                live_.erase(it);
                return true;
            }
        }
        return false;
    }

    std::vector<ProvisionedNode> provisioned() const override { return live_; }

private:
    std::size_t capacity_;
    std::size_t next_id_ = 0;
    std::vector<ProvisionedNode> live_;
};

// A fake AzureControlPlane modelling a burst pool — so the burst-identity lifecycle
// runs through the **real** AzureProvisioner (the reference ADR-20 ephemeral-burst
// backend) with no cloud SDK / CLI. bring_up scales the pool to the requested count,
// destroy removes one node, list reports what is up.
class FakeBurstControlPlane : public AzureControlPlane {
public:
    ProvisionResult bring_up(const AzureConfig& /*config*/,
                             const ProvisionRequest& request) override {
        pool_.clear();
        for (std::size_t index = 0; index < request.count; ++index) {
            ProvisionedNode node;
            node.node_id = "burst-" + std::to_string(index);
            node.address = "10.9.0." + std::to_string(index + 1) + ":7777";
            node.capability = request.spec;
            pool_.push_back(node);
        }
        ProvisionResult result;
        result.nodes = pool_;
        result.satisfied = pool_.size() == request.count;
        result.detail = "scaled";
        return result;
    }

    bool destroy(const AzureConfig& /*config*/,
                 const std::string& node_id) override {
        const auto found = std::find_if(
            pool_.begin(), pool_.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool_.end()) return false;
        pool_.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> list(const AzureConfig& /*config*/) const override {
        return pool_;
    }

private:
    std::vector<ProvisionedNode> pool_;
};

AzureConfig burst_config() {
    AzureConfig config;
    config.subscription_id = "sub-test";
    config.resource_group = "zetta-rg";
    config.location = "eastus";
    return config;
}

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

NodeCapability gpu_spec() {
    return NodeCapability{8, 32 * kGiB, Gpu{GpuKind::Cuda, 50.0, ""}};
}

// A short-lived, job-scoped access policy: scope the key to the job's project and
// grant a TTL'd orchestration right (so the identity expires with the burst).
Scope job_scope(const std::string& project) {
    Scope scope;
    scope.grant(ResourceKind::Project, project);
    return scope;
}

CapabilitySet short_lived_caps() {
    CapabilitySet caps;
    caps.grant(CapabilityDomain::Orchestration, "run", "2999-01-01T00:00:00.000Z");
    return caps;
}

std::size_t count_action(const Node& node, AuditAction action) {
    std::size_t n = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == action) ++n;
    }
    return n;
}

}  // namespace

// ---- the acceptance criterion: bulk mint N → bulk revoke → audited -----------

TEST(BurstIdentity, BulkMintAndRevokeViaTheProvisionerSeam) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    ASSERT_EQ(node.principal_count(), 0u);

    FakeProvisioner provisioner{/*capacity=*/8};
    BurstIdentityManager identities{node};
    const std::string job = "job-churn-1";

    // Provision a burst of 3, then bulk-mint an identity for each provisioned node.
    const ProvisionResult burst = provisioner.provision(ProvisionRequest{3, gpu_spec()});
    ASSERT_EQ(burst.nodes.size(), 3u);

    const std::vector<BurstIdentity> minted =
        identities.mint(job, burst.nodes, job_scope("churn"), short_lived_caps());
    ASSERT_EQ(minted.size(), 3u);

    // One service principal + one job-scoped key per burst node.
    EXPECT_EQ(node.principal_count(), 3u);
    for (std::size_t index = 0; index < minted.size(); ++index) {
        const BurstIdentity& identity = minted[index];
        EXPECT_EQ(identity.node_id, burst.nodes[index].node_id);
        EXPECT_FALSE(identity.principal_id.empty());
        EXPECT_FALSE(identity.key_id.empty());

        auto key = node.find_access_key(identity.key_id);
        ASSERT_TRUE(key);
        EXPECT_TRUE(key->permits(zetta::Resource{ResourceKind::Project, "churn"}));
        EXPECT_FALSE(key->permits(zetta::Resource{ResourceKind::Project, "fraud"}));
    }

    // Each mint recorded a key_issued audit event; nothing revoked yet.
    EXPECT_EQ(count_action(node, AuditAction::KeyIssued), 3u);
    EXPECT_EQ(count_action(node, AuditAction::KeyRevoked), 0u);

    // minted() derives the live set from the principals.
    EXPECT_EQ(identities.minted(job).size(), 3u);
    EXPECT_TRUE(identities.minted("other-job").empty());

    // Tear the burst down: bulk-revoke the identities, then release the nodes.
    const std::size_t revoked = identities.revoke(job);
    EXPECT_EQ(revoked, 3u);
    for (const ProvisionedNode& provisioned : provisioner.provisioned()) {
        provisioner.teardown(provisioned.node_id);
    }

    // Principals + keys are gone; each revoke recorded a key_revoked event.
    EXPECT_EQ(node.principal_count(), 0u);
    EXPECT_TRUE(identities.minted(job).empty());
    EXPECT_EQ(count_action(node, AuditAction::KeyRevoked), 3u);

    // Revoking again is an idempotent no-op.
    EXPECT_EQ(identities.revoke(job), 0u);
    EXPECT_EQ(count_action(node, AuditAction::KeyRevoked), 3u);

    node.stop();
}

// ---- TC-6: the full burst lifecycle through the real BurstProvisioner --------
//
// The acceptance the test_case (#253 / TC-6, ADR-20 / RSK-9) names: bursting N
// nodes mints N **distinct** identities, and teardown revokes them with **no
// orphans**. Unlike the test above (which calls mint/revoke by hand), this wires the
// real cloud-burst provisioner's IdentityHooks to a real BurstIdentityManager — so
// provisioning *triggers* the mint and the draining teardown *triggers* the bulk
// revoke, exactly as the production ephemeral-burst path runs.

TEST(BurstIdentity, BurstProvisionerMintsDistinctIdentitiesAndRevokesOnTeardown) {
    for (const std::size_t fleet : {1u, 3u, 8u}) {
        TempDir tmp;
        Node node{config_for(tmp)};
        node.start();

        FakeBurstControlPlane control_plane;
        BurstIdentityManager identities{node};
        const std::string job = "job-burst-" + std::to_string(fleet);

        // Hooks bound to the real manager: mint on provision, bulk-revoke on drain.
        AzureProvisioner::IdentityHooks hooks;
        hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
            identities.mint(job, nodes, job_scope("churn"), short_lived_caps());
        };
        hooks.revoke = [&] { identities.revoke(job); };

        AzureProvisioner provisioner{control_plane, burst_config(), hooks};

        // Burst the fleet — provision mints one identity per node that came up.
        const ProvisionResult burst =
            provisioner.provision(ProvisionRequest{fleet, gpu_spec()});
        ASSERT_TRUE(burst.satisfied);
        ASSERT_EQ(burst.nodes.size(), fleet);

        // N distinct identities: distinct nodes, principals, and keys.
        const std::vector<BurstIdentity> minted = identities.minted(job);
        ASSERT_EQ(minted.size(), fleet);
        std::set<std::string> node_ids, principals, keys;
        for (const BurstIdentity& identity : minted) {
            node_ids.insert(identity.node_id);
            principals.insert(identity.principal_id);
            keys.insert(identity.key_id);
            EXPECT_FALSE(identity.principal_id.empty());
            EXPECT_FALSE(identity.key_id.empty());
        }
        EXPECT_EQ(node_ids.size(), fleet);     // one per distinct burst node
        EXPECT_EQ(principals.size(), fleet);   // no shared principal
        EXPECT_EQ(keys.size(), fleet);         // no shared key
        EXPECT_EQ(node.principal_count(), fleet);
        EXPECT_EQ(count_action(node, AuditAction::KeyIssued), fleet);
        EXPECT_EQ(count_action(node, AuditAction::KeyRevoked), 0u);

        // Tear the burst down node by node. The bulk revoke fires only when the last
        // teardown drains the pool — until then the identities stay live.
        std::vector<std::string> live_ids;
        for (const ProvisionedNode& live : provisioner.provisioned()) {
            live_ids.push_back(live.node_id);
        }
        ASSERT_EQ(live_ids.size(), fleet);
        for (std::size_t index = 0; index < live_ids.size(); ++index) {
            EXPECT_TRUE(provisioner.teardown(live_ids[index]));
            const bool drained = index + 1 == live_ids.size();
            // No identity is revoked until the pool drains on the final teardown.
            EXPECT_EQ(identities.minted(job).size(), drained ? 0u : fleet);
        }

        // No orphans: every principal + key is gone, and each was audited.
        EXPECT_EQ(node.principal_count(), 0u);
        EXPECT_TRUE(identities.minted(job).empty());
        EXPECT_EQ(count_action(node, AuditAction::KeyRevoked), fleet);

        node.stop();
    }
}

// ---- the minted principal is a short-lived service actor ---------------------

TEST(BurstIdentity, MintsServicePrincipalsScopedToTheJob) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    BurstIdentityManager identities{node};
    ProvisionedNode burst_node;
    burst_node.node_id = "node-a";
    burst_node.capability = gpu_spec();

    const auto minted =
        identities.mint("job-x", {burst_node}, job_scope("churn"), short_lived_caps());
    ASSERT_EQ(minted.size(), 1u);

    auto principal = node.find_principal("burst/job-x/node-a");
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->kind(), PrincipalKind::Service);
    EXPECT_EQ(principal->id(), minted.front().principal_id);
    ASSERT_EQ(principal->key_count(), 1u);

    // The key is TTL'd: live now, but a key with an elapsed cutoff would be denied.
    auto key = principal->keys().front();
    EXPECT_TRUE(key->permits(zetta::Operation{CapabilityDomain::Orchestration, "run"},
                             zetta::now_iso8601()));
    EXPECT_FALSE(key->permits(
        zetta::Operation{CapabilityDomain::Orchestration, "run"},
        "3000-01-01T00:00:00.000Z"));  // past the TTL → denied

    node.stop();
}

// ---- idempotent mint ---------------------------------------------------------

TEST(BurstIdentity, MintIsIdempotentForTheSameJobAndNode) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    BurstIdentityManager identities{node};
    ProvisionedNode burst_node;
    burst_node.node_id = "node-a";
    burst_node.capability = gpu_spec();

    const auto first =
        identities.mint("job-x", {burst_node}, job_scope("churn"), short_lived_caps());
    const auto second =
        identities.mint("job-x", {burst_node}, job_scope("churn"), short_lived_caps());

    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first.front(), second.front());        // same principal + key
    EXPECT_EQ(node.principal_count(), 1u);           // no duplicate principal
    EXPECT_EQ(count_action(node, AuditAction::KeyIssued), 1u);  // audited once

    node.stop();
}

// ---- identities survive a restart (derived from replicated state) ------------

TEST(BurstIdentity, MintedIdentitiesSurviveRestart) {
    TempDir tmp;
    const std::string job = "job-x";
    {
        Node node{config_for(tmp)};
        node.start();
        BurstIdentityManager identities{node};
        ProvisionedNode burst_node;
        burst_node.node_id = "node-a";
        burst_node.capability = gpu_spec();
        identities.mint(job, {burst_node}, job_scope("churn"), short_lived_caps());
        node.stop();
    }

    // A fresh node rebuilds the principal tree from disk; the burst identity (and
    // its revoke set) is derived, not separately persisted.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    BurstIdentityManager identities{reloaded};
    const auto minted = identities.minted(job);
    ASSERT_EQ(minted.size(), 1u);
    EXPECT_EQ(minted.front().node_id, "node-a");
    EXPECT_FALSE(minted.front().key_id.empty());

    EXPECT_EQ(identities.revoke(job), 1u);
    EXPECT_EQ(reloaded.principal_count(), 0u);
    reloaded.stop();
}

// ---- the new audit actions round-trip + pass schema validation ---------------

TEST(BurstIdentity, KeyIssuedAndRevokedActionsRoundTripAndValidate) {
    EXPECT_EQ(zetta::to_string(AuditAction::KeyIssued), "key_issued");
    EXPECT_EQ(zetta::to_string(AuditAction::KeyRevoked), "key_revoked");
    EXPECT_EQ(zetta::audit_action_from("key_issued"), AuditAction::KeyIssued);
    EXPECT_EQ(zetta::audit_action_from("key_revoked"), AuditAction::KeyRevoked);

    zetta::AuditEventValue value;
    value.action = AuditAction::KeyIssued;
    value.target = "key-123";
    value.time = "2026-06-25T14:00:00.000Z";
    std::string error;
    EXPECT_TRUE(zetta::validate_audit_event_value(zetta::to_json(value), &error))
        << error;
}
