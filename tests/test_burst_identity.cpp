#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/burst_identity.hpp"
#include "zetta/capability.hpp"
#include "zetta/node.hpp"
#include "zetta/node_capability.hpp"
#include "zetta/principal.hpp"
#include "zetta/provisioner.hpp"
#include "zetta/scope.hpp"
#include "zetta/timestamp.hpp"

using zetta::AuditAction;
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
