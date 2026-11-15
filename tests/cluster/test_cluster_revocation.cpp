// Per-node revocation (#1320, CTL-2 V2): a revoked node key is refused at the
// handshake on every node, a live link presenting it is closed when the
// revocation arrives, and the revocation itself is a sealed identity record
// (node_revocation:<key hash>) that a member cannot forge. #848 step 6: a node
// removed from the cluster that comes back with its old key and certificate is
// refused by every node. See docs/cluster/security.md ("Revocation").
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "../temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/governance/identity_seal.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterSecurity;
using zetta::cluster::kNodeCertPrefix;
using zetta::cluster::NodeRevocation;
using zetta::cluster::ReplicationManager;
using zetta::governance::IdentityAuthority;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <typename Predicate>
bool eventually(Predicate predicate, int timeout_ms = 20000) {
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (predicate()) return true;
        sleep_ms(100);
    }
    return predicate();
}

ClusterSecurity enrolling(const std::string& token, const std::string& node_id,
                          const std::string& ca_seed = {}) {
    std::string reason;
    auto security = zetta::cluster::derive_cluster_security(token, node_id, &reason);
    EXPECT_TRUE(security) << reason;
    if (!security) return {};
    security->enroll = true;
    security->enrollment_ca_seed = ca_seed;
    return *security;
}

ClusterConfig config_on(std::uint16_t port, ClusterSecurity security,
                        std::vector<std::string> seeds = {}) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.seeds = std::move(seeds);
    config.security = std::move(security);
    return config;
}

bool has_peer(const Cluster& cluster, const std::string& id) {
    const auto peers = cluster.peers();
    return std::any_of(peers.begin(), peers.end(),
                       [&](const auto& peer) { return peer.node_id == id; });
}

std::string key_of(const ClusterSecurity& security) {
    return zetta::cluster::certificate_pem_public_key_sha256(security.certificate_pem);
}

NodeConfig node_config(const zetta::test::TempDir& dir,
                       std::optional<IdentityAuthority> root) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;
    config.identity_authority = std::move(root);
    return config;
}

}  // namespace

// #848 step 6: B enrolled, was removed, and comes back with the same key and
// certificate. Both remaining nodes refuse it at the handshake.
TEST(ClusterRevocation, ARevokedNodeIsRefusedByEveryNode) {
    const std::string token = zetta::cluster::generate_join_token();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const std::string id_c = zetta::util::new_uuid();

    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46620, enrolling(token, id_a, ca->seed)), &replication_a);
    node_a.start();
    ReplicationManager replication_c(id_c);
    Cluster node_c(id_c, config_on(46622, enrolling(token, id_c), {"127.0.0.1:46620"}),
                   &replication_c);
    node_c.start();

    const ClusterSecurity security_b = enrolling(token, id_b);
    {
        ReplicationManager replication_b(id_b);
        Cluster node_b(id_b, config_on(46621, security_b, {"127.0.0.1:46620", "127.0.0.1:46622"}),
                       &replication_b);
        node_b.start();
        ASSERT_TRUE(eventually([&] { return has_peer(node_a, id_b) && has_peer(node_c, id_b); }));
        node_b.stop();
    }
    // The revocation reaches both remaining nodes (the Node hands them the
    // sealed records it trusts; here it is handed directly).
    const std::map<std::string, NodeRevocation> revoked{{key_of(security_b), {id_b, false}}};
    node_a.set_revoked_keys(revoked);
    node_c.set_revoked_keys(revoked);
    ASSERT_TRUE(eventually([&] { return !has_peer(node_a, id_b) && !has_peer(node_c, id_b); }));

    // B returns with its old key and certificate.
    ReplicationManager replication_b(id_b);
    Cluster returning(id_b, config_on(46623, security_b, {"127.0.0.1:46620", "127.0.0.1:46622"}),
                      &replication_b);
    returning.start();
    sleep_ms(5000);  // several reconnect cycles
    EXPECT_FALSE(has_peer(node_a, id_b)) << "a revoked node rejoined";
    EXPECT_FALSE(has_peer(node_c, id_b)) << "a revoked node rejoined";
    EXPECT_TRUE(has_peer(node_a, id_c)) << "the revocation cut a node that was not revoked";

    returning.stop();
    node_c.stop();
    node_a.stop();
}

TEST(ClusterRevocation, ALiveLinkIsClosedWhenTheRevocationArrives) {
    const std::string token = zetta::cluster::generate_join_token();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46625, enrolling(token, id_a, ca->seed)), &replication_a);
    node_a.start();
    const ClusterSecurity security_b = enrolling(token, id_b);
    ReplicationManager replication_b(id_b);
    Cluster node_b(id_b, config_on(46626, security_b, {"127.0.0.1:46625"}), &replication_b);
    node_b.start();
    ASSERT_TRUE(eventually([&] { return has_peer(node_a, id_b); }));

    node_a.set_revoked_keys({{key_of(security_b), {id_b, false}}});
    EXPECT_TRUE(eventually([&] { return !has_peer(node_a, id_b); }, 15000))
        << "the live link to a revoked node stayed open";
    sleep_ms(4000);  // B keeps redialing
    EXPECT_FALSE(has_peer(node_a, id_b));

    node_b.stop();
    node_a.stop();
}

// ----- the sealed record ---------------------------------------------------------

TEST(NodeRevocation, ARevocationIsSealedAndRemovesTheNodeFromMembership) {
    zetta::test::TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    const IdentityAuthority root = *IdentityAuthority::from_owner_secret("revocation-owner");
    Node node(node_config(dir, root), &replication);
    ASSERT_TRUE(node.start());
    // The victim's enrollment record, as the cluster writes it.
    const std::string victim = zetta::util::new_uuid();
    const ClusterSecurity victim_security =
        enrolling(zetta::cluster::generate_join_token(), victim);
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    const std::string certificate = zetta::cluster::sign_enrollment_certificate(
        *ca, victim, zetta::cluster::certificate_pem_to_der(victim_security.certificate_pem));
    const auto enrolled = zetta::cluster::verify_enrolled_certificate(certificate,
                                                                      ca->certificate_pem);
    ASSERT_TRUE(enrolled);
    const std::string key = enrolled->public_key_sha256;
    replication.set(std::string(kNodeCertPrefix) + victim,
                    nlohmann::json{{"certificate", certificate},
                                   {"ca_certificate", ca->certificate_pem},
                                   {"public_key_sha256", key}, {"serial", enrolled->serial},
                                   {"issued_by", victim}, {"not_after", enrolled->not_after}});
    replication.set("node:" + victim, nlohmann::json{{"address", "10.0.0.9:7777"}});

    const auto revoked = node.revoke_node(victim, "decommissioned", false, "admin");
    ASSERT_TRUE(revoked.ok) << revoked.error;
    EXPECT_EQ(revoked.revocation.public_key_sha256, key);
    EXPECT_EQ(revoked.revocation.serial, enrolled->serial);
    const std::string record_key = "node_revocation:" + key;
    const nlohmann::json sealed = replication.snapshot()[record_key];
    ASSERT_TRUE(node.identity_authority()->open(record_key, sealed));
    EXPECT_FALSE(replication.snapshot().contains("node:" + victim));
    node.tick();
    ASSERT_EQ(node.node_revocations().size(), 1u);
    EXPECT_EQ(node.node_revocations().front().node_id, victim);
    node.stop();
}

TEST(NodeRevocation, AnUnsealedRevocationIsIgnored) {
    zetta::test::TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(node_config(dir, *IdentityAuthority::from_owner_secret("revocation-owner")),
              &replication);
    ASSERT_TRUE(node.start());
    EXPECT_TRUE(zetta::governance::is_identity_key("node_revocation:" + std::string(64, 'c')));
    // A member writes a revocation (or tries to cut a peer off) without the root.
    replication.set("node_revocation:" + std::string(64, 'c'),
                    nlohmann::json{{"node_id", "someone"}, {"serial", ""},
                                   {"revoked_by", "member"}, {"revoked_at", ""},
                                   {"reason", ""}, {"allow_rekey", false}});
    node.tick();
    EXPECT_TRUE(node.node_revocations().empty());
    node.stop();
}

TEST(NodeRevocation, AVerifyOnlyNodeCannotRevoke) {
    zetta::test::TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    const IdentityAuthority root = *IdentityAuthority::from_owner_secret("revocation-owner");
    Node node(node_config(dir, IdentityAuthority::verify_only(root.public_key_hex())),
              &replication);
    ASSERT_TRUE(node.start());
    const auto revoked = node.revoke_node("someone", "", false, "admin", std::string(64, 'd'));
    EXPECT_FALSE(revoked.ok);
    EXPECT_FALSE(revoked.error.empty());
    node.stop();
}
