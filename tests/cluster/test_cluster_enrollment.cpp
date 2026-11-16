// CTL-2 V2 enrollment (#1319): the enrollment CA (random, never derived from the
// join token), enrollment certificates for the key a node presents, the
// replicated `node_cert:<id>` record, and every node pinning an enrolled id to
// its key. The cluster-level proof: a join-token holder that mints a
// token-derived certificate for an enrolled node's id, with its own key, is
// refused. A node without the token never completes the handshake at all
// (ClusterSecurity.WrongTokenNodeCannotJoin). See docs/cluster/security.md.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterSecurity;
using zetta::cluster::EnrollmentCa;
using zetta::cluster::ReplicationManager;
using zetta::cluster::kNodeCertPrefix;

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

ClusterSecurity derived(const std::string& token, const std::string& node_id) {
    std::string reason;
    auto security = zetta::cluster::derive_cluster_security(token, node_id, &reason);
    EXPECT_TRUE(security) << reason;
    return security ? *security : ClusterSecurity{};
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

}  // namespace

TEST(ClusterEnrollment, TheCaComesFromItsSeedNotFromTheToken) {
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    const auto again = zetta::cluster::enrollment_ca_from_seed(ca->seed);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->certificate_pem, ca->certificate_pem);
    EXPECT_EQ(again->public_key, ca->public_key);
    const auto other = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(other);
    EXPECT_NE(other->public_key, ca->public_key);
    EXPECT_FALSE(zetta::cluster::enrollment_ca_from_seed("not-hex"));
}

TEST(ClusterEnrollment, AnEnrollmentCertificateCarriesThePresentedKey) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id = zetta::util::new_uuid();
    const ClusterSecurity node = derived(token, id);
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    const auto der = zetta::cluster::certificate_pem_to_der(node.certificate_pem);
    ASSERT_FALSE(der.empty());
    const std::string certificate = zetta::cluster::sign_enrollment_certificate(*ca, id, der);
    ASSERT_FALSE(certificate.empty());
    const auto verified = zetta::cluster::verify_enrolled_certificate(certificate,
                                                                      ca->certificate_pem);
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->node_id, id);
    EXPECT_EQ(verified->public_key_sha256, zetta::cluster::certificate_public_key_sha256(der));
    EXPECT_EQ(verified->ca_public_key, ca->public_key);
    EXPECT_EQ(verified->serial.size(), 16u);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    EXPECT_GT(verified->not_after, now + 29 * 24 * 3600);
    EXPECT_LT(verified->not_after, now + 31 * 24 * 3600);
    // Another CA did not sign it.
    const auto other = zetta::cluster::generate_enrollment_ca();
    EXPECT_FALSE(zetta::cluster::verify_enrolled_certificate(certificate, other->certificate_pem));
    // The token-derived CA is not an enrollment CA.
    EXPECT_FALSE(zetta::cluster::verify_enrolled_certificate(node.certificate_pem,
                                                             node.ca_certificate_pem));
}

TEST(ClusterEnrollment, TheProofBindsTheTokenTheIdAndTheKey) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string proof = zetta::cluster::enrollment_proof(token, "node-a", "aa");
    EXPECT_EQ(proof.size(), 64u);
    EXPECT_TRUE(zetta::cluster::enrollment_proof_matches(token, "node-a", "aa", proof));
    EXPECT_FALSE(zetta::cluster::enrollment_proof_matches(token, "node-b", "aa", proof));
    EXPECT_FALSE(zetta::cluster::enrollment_proof_matches(token, "node-a", "ab", proof));
    EXPECT_FALSE(zetta::cluster::enrollment_proof_matches(
        zetta::cluster::generate_join_token(), "node-a", "aa", proof));
}

TEST(ClusterEnrollment, TheFounderGeneratesTheCaAndAJoinerDoesNot) {
    zetta::test::TempDir founder_dir;
    zetta::test::TempDir joiner_dir;
    std::string reason;
    const auto founder = zetta::cluster::load_or_create_cluster_security(
        founder_dir.path().string(), zetta::util::new_uuid(), {}, &reason);
    ASSERT_TRUE(founder) << reason;
    EXPECT_TRUE(founder->enroll);
    EXPECT_FALSE(founder->enrollment_ca_seed.empty());
    EXPECT_TRUE(std::filesystem::exists(founder_dir.path() / "security" / "cluster-ca.json"));
    // The CA is not the token's: nothing about it derives from the join token.
    const auto ca = zetta::cluster::enrollment_ca_from_seed(founder->enrollment_ca_seed);
    ASSERT_TRUE(ca);
    EXPECT_NE(ca->certificate_pem, founder->ca_certificate_pem);

    const auto joiner = zetta::cluster::load_or_create_cluster_security(
        joiner_dir.path().string(), zetta::util::new_uuid(), founder->join_token, &reason);
    ASSERT_TRUE(joiner) << reason;
    EXPECT_TRUE(joiner->enrollment_ca_seed.empty()) << "a joiner is handed the CA when it enrolls";
}

TEST(ClusterEnrollment, ATokenChangeKeepsTheNodesKey) {
    zetta::test::TempDir dir;
    const std::string id = zetta::util::new_uuid();
    std::string reason;
    const auto first = zetta::cluster::load_or_create_cluster_security(dir.path().string(), id,
                                                                       {}, &reason);
    ASSERT_TRUE(first) << reason;
    const auto rekeyed = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), id, zetta::cluster::generate_join_token(), &reason);
    ASSERT_TRUE(rekeyed) << reason;
    EXPECT_NE(rekeyed->certificate_pem, first->certificate_pem);
    EXPECT_EQ(zetta::cluster::certificate_pem_public_key_sha256(rekeyed->certificate_pem),
              zetta::cluster::certificate_pem_public_key_sha256(first->certificate_pem))
        << "the id would no longer match the key it enrolled";
}

TEST(ClusterEnrollment, PinsArePersistedAndValidated) {
    zetta::test::TempDir dir;
    const std::string directory = dir.path().string();
    std::map<std::string, std::string> pins{{"node-a", std::string(64, 'a')}};
    ASSERT_TRUE(zetta::cluster::store_node_pins(directory, pins));
    std::map<std::string, std::string> loaded;
    ASSERT_TRUE(zetta::cluster::load_node_pins(directory, loaded));
    EXPECT_EQ(loaded, pins);
    std::ofstream(dir.path() / "node-pins.json") << R"({"version": 1, "pins": {"x": "short"}})";
    std::string reason;
    EXPECT_FALSE(zetta::cluster::load_node_pins(directory, loaded, &reason));
    EXPECT_FALSE(reason.empty());
}

// A joiner enrolls through the founder: it is handed the CA, its certificate is
// recorded as node_cert:<id> in both documents, and each pins the other.
TEST(ClusterEnrollment, AJoinerEnrollsAndIsRecorded) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);

    ClusterSecurity security_a = derived(token, id_a);
    security_a.enroll = true;
    security_a.enrollment_ca_seed = ca->seed;
    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46610, security_a), &replication_a);
    node_a.start();

    ClusterSecurity security_b = derived(token, id_b);
    security_b.enroll = true;
    ReplicationManager replication_b(id_b);
    Cluster node_b(id_b, config_on(46611, security_b, {"127.0.0.1:46610"}), &replication_b);
    node_b.start();

    const std::string key_b = std::string(kNodeCertPrefix) + id_b;
    const std::string key_a = std::string(kNodeCertPrefix) + id_a;
    ASSERT_TRUE(eventually([&] {
        const auto snapshot = replication_b.snapshot();
        return snapshot.contains(key_a) && snapshot.contains(key_b);
    })) << "the enrollment records did not reach the joiner";
    const nlohmann::json record = replication_b.snapshot()[key_b];
    const auto verified = zetta::cluster::verify_enrolled_certificate(
        record["certificate"].get<std::string>(), ca->certificate_pem);
    ASSERT_TRUE(verified) << "the joiner's certificate is not from the cluster's CA";
    EXPECT_EQ(verified->node_id, id_b);
    EXPECT_EQ(verified->public_key_sha256,
              zetta::cluster::certificate_pem_public_key_sha256(security_b.certificate_pem));
    EXPECT_EQ(record["issued_by"], id_a);

    node_b.stop();
    node_a.stop();
}

// #1319's "done when": a token holder mints a token-derived certificate for an
// enrolled node's id, with its own key. The token-derived CA still signs it and
// its HELLO matches its certificate, but the id is pinned to the key it
// enrolled, so the member refuses it. Asking to enroll that id to the new key
// would be refused the same way ("enrolled-to-another-key"); the link never
// gets that far.
TEST(ClusterEnrollment, ATokenHolderCannotTakeAnEnrolledId) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);

    ClusterSecurity security_a = derived(token, id_a);
    security_a.enroll = true;
    security_a.enrollment_ca_seed = ca->seed;
    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46615, security_a), &replication_a);
    node_a.start();

    {
        ClusterSecurity security_b = derived(token, id_b);
        security_b.enroll = true;
        ReplicationManager replication_b(id_b);
        Cluster node_b(id_b, config_on(46616, security_b, {"127.0.0.1:46615"}),
                       &replication_b);
        node_b.start();
        ASSERT_TRUE(eventually([&] {
            return replication_a.snapshot().contains(std::string(kNodeCertPrefix) + id_b);
        })) << "B did not enroll";
        node_b.stop();
    }
    ASSERT_TRUE(eventually([&] { return !has_peer(node_a, id_b); }));

    // The impostor: the same token, B's id, a fresh key.
    ClusterSecurity impostor = derived(token, id_b);
    ASSERT_NE(zetta::cluster::certificate_pem_public_key_sha256(impostor.certificate_pem),
              zetta::cluster::certificate_pem_public_key_sha256(
                  replication_a.snapshot()[std::string(kNodeCertPrefix) + id_b]["certificate"]
                      .get<std::string>()));
    Cluster node_c(id_b, config_on(46617, impostor, {"127.0.0.1:46615"}));
    node_c.start();
    sleep_ms(5000);  // several reconnect cycles
    EXPECT_FALSE(has_peer(node_a, id_b)) << "an impostor took an enrolled node id";

    node_c.stop();
    node_a.stop();
}
