// Inter-node transport security (CTL-2, #466): the join token / derived
// cluster CA / per-node identity material, its persistence, and the cluster
// transport actually enforcing it — same-token nodes converge over mTLS,
// wrong-token and plaintext nodes never join. See docs/cluster/security.md.

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>
#include <thread>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/util/uuid.hpp"

#include "../temp_dir.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterSecurity;
using zetta::cluster::ReplicationManager;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Wait (patiently — real TCP + TLS in-process) until the cluster reports
// `expected` peers, or the timeout runs out.
bool wait_for_peers(const Cluster& cluster, std::size_t expected,
                    int timeout_ms = 20000) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (cluster.peer_count() == expected) return true;
        sleep_ms(50);
    }
    return cluster.peer_count() == expected;
}

ClusterSecurity must_derive(const std::string& token,
                            const std::string& node_id) {
    std::string reason;
    auto security =
        zetta::cluster::derive_cluster_security(token, node_id, &reason);
    EXPECT_TRUE(security) << reason;
    return security ? *security : ClusterSecurity{};
}

}  // namespace

TEST(ClusterSecurity, JoinTokenGenerationAndValidation) {
    const std::string token = zetta::cluster::generate_join_token();
    ASSERT_EQ(token.size(), 64u);
    EXPECT_TRUE(zetta::cluster::valid_join_token(token));
    EXPECT_NE(token, zetta::cluster::generate_join_token());  // random

    EXPECT_FALSE(zetta::cluster::valid_join_token(""));
    EXPECT_FALSE(zetta::cluster::valid_join_token("short"));
    EXPECT_FALSE(zetta::cluster::valid_join_token(
        std::string(63, 'a') + "G"));  // uppercase / non-hex
    EXPECT_FALSE(zetta::cluster::valid_join_token(std::string(63, 'a')));
}

TEST(ClusterSecurity, SameTokenDerivesTheSameClusterCa) {
    const std::string token = zetta::cluster::generate_join_token();
    const ClusterSecurity first = must_derive(token, "node-a");
    const ClusterSecurity second = must_derive(token, "node-b");
    ASSERT_TRUE(first.enabled());
    ASSERT_TRUE(second.enabled());

    // The trust anchor is identical on every token holder; the per-node
    // identities are distinct keypairs.
    EXPECT_EQ(first.ca_certificate_pem, second.ca_certificate_pem);
    EXPECT_NE(first.certificate_pem, second.certificate_pem);
    EXPECT_NE(first.private_key_pem, second.private_key_pem);

    // A different token derives a different CA — the join gate.
    const ClusterSecurity other =
        must_derive(zetta::cluster::generate_join_token(), "node-a");
    EXPECT_NE(other.ca_certificate_pem, first.ca_certificate_pem);
}

TEST(ClusterSecurity, DeriveRejectsBadInput) {
    std::string reason;
    EXPECT_FALSE(
        zetta::cluster::derive_cluster_security("not-a-token", "n", &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(zetta::cluster::derive_cluster_security(
        zetta::cluster::generate_join_token(), "", &reason));
}

TEST(ClusterSecurity, LoadOrCreatePersistsTokenAndIdentity) {
    zetta::test::TempDir dir;
    const std::string node_id = zetta::util::new_uuid();

    std::string reason;
    auto first = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason);
    ASSERT_TRUE(first) << reason;
    ASSERT_TRUE(first->enabled());

    // A second start reuses everything: same token, same identity.
    auto second = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason);
    ASSERT_TRUE(second) << reason;
    EXPECT_EQ(second->join_token, first->join_token);
    EXPECT_EQ(second->certificate_pem, first->certificate_pem);
    EXPECT_EQ(second->private_key_pem, first->private_key_pem);

    // An operator-passed token override re-keys the node under the new CA.
    const std::string other_token = zetta::cluster::generate_join_token();
    auto rekeyed = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, other_token, &reason);
    ASSERT_TRUE(rekeyed) << reason;
    EXPECT_EQ(rekeyed->join_token, other_token);
    EXPECT_NE(rekeyed->ca_certificate_pem, first->ca_certificate_pem);
    EXPECT_NE(rekeyed->certificate_pem, first->certificate_pem);

    // …and the override persists: the next plain start stays on the new token.
    auto after = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason);
    ASSERT_TRUE(after) << reason;
    EXPECT_EQ(after->join_token, other_token);
    EXPECT_EQ(after->certificate_pem, rekeyed->certificate_pem);
}

TEST(ClusterSecurity, SecretFileIsSchemaValidated) {
    zetta::test::TempDir dir;
    const std::string node_id = zetta::util::new_uuid();

    // A malformed secret file fails the load with a reason — it is never
    // silently clobbered by a fresh token.
    std::filesystem::create_directories(dir.path() /
                                        "security");
    std::ofstream(dir.path() / "security" /
                  "cluster-secret.json")
        << R"({"version": 1, "join_token": "NOT-HEX"})";
    std::string reason;
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason));
    EXPECT_NE(reason.find("cluster-secret.json"), std::string::npos);

    // Same for JSON that isn't the expected shape at all.
    std::ofstream(dir.path() / "security" /
                  "cluster-secret.json")
        << R"({"token": "wrong-key"})";
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason));
}

TEST(ClusterSecurity, PeerCommandCarriesTheJoinToken) {
    const std::string token = zetta::cluster::generate_join_token();
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 42400;
    config.security = must_derive(token, "node-a");

    Cluster node("node-a", config);
    node.start();
    EXPECT_EQ(node.peer_command(),
              "zetta --peer 127.0.0.1:42400 --join-token " + token);
    node.stop();
}

TEST(ClusterSecurity, SameTokenNodesConvergeOverMutualTls) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager replication_a(id_a);
    replication_a.set("greeting", {{"text", "hello over mTLS"}});

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 42410;
    config_a.security = must_derive(token, id_a);
    Cluster node_a(id_a, config_a, &replication_a);
    node_a.start();

    ReplicationManager replication_b(id_b);
    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 42411;
    config_b.seeds = {"127.0.0.1:42410"};
    config_b.security = must_derive(token, id_b);
    Cluster node_b(id_b, config_b, &replication_b);
    node_b.start();

    // Membership over TLS…
    ASSERT_TRUE(wait_for_peers(node_a, 1));
    ASSERT_TRUE(wait_for_peers(node_b, 1));

    // …and the gossip actually flows through it (the CRDT document merges).
    bool merged = false;
    for (int waited = 0; waited < 20000 && !merged; waited += 100) {
        merged = replication_b.snapshot().contains("greeting");
        if (!merged) sleep_ms(100);
    }
    ASSERT_TRUE(merged) << "replication did not converge over the TLS link";
    EXPECT_EQ(replication_b.snapshot()["greeting"]["text"], "hello over mTLS");

    node_b.stop();
    node_a.stop();
}

TEST(ClusterSecurity, WrongTokenNodeCannotJoin) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 42420;
    config_a.security = must_derive(token, id_a);
    Cluster node_a(id_a, config_a);
    node_a.start();

    // A dialer holding a DIFFERENT token derives a different CA: both sides
    // reject the other's certificate and the handshake never completes.
    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 42421;
    config_b.seeds = {"127.0.0.1:42420"};
    config_b.security =
        must_derive(zetta::cluster::generate_join_token(), id_b);
    Cluster node_b(id_b, config_b);
    node_b.start();

    // Give it several reconnect cycles' worth of chances, then assert nothing
    // joined in either direction.
    sleep_ms(4000);
    EXPECT_EQ(node_a.peer_count(), 0u);
    EXPECT_EQ(node_b.peer_count(), 0u);

    node_b.stop();
    node_a.stop();
}

TEST(ClusterSecurity, PlaintextNodeCannotJoinTlsCluster) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 42430;
    config_a.security = must_derive(token, id_a);
    Cluster node_a(id_a, config_a);
    node_a.start();

    // No security at all (the --cluster-plaintext / bare-embedding shape):
    // its HELLO bytes are not a TLS ClientHello, so the TLS side drops it.
    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 42431;
    config_b.seeds = {"127.0.0.1:42430"};
    Cluster node_b(zetta::util::new_uuid(), config_b);
    node_b.start();

    sleep_ms(4000);
    EXPECT_EQ(node_a.peer_count(), 0u);
    EXPECT_EQ(node_b.peer_count(), 0u);

    node_b.stop();
    node_a.stop();
}
