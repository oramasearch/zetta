// CTL-2 V2 enrollment (#1319, #1513): the enrollment CA (random, never derived
// from the join token), enrollment certificates, the replicated
// `node_cert:<id>` record, and every node pinning an enrolled id to its key. In
// the second release (#1513) the enrollment CA is the transport's only trust
// anchor: a token-derived certificate is refused, and a new node enrolls over a
// link where only the member authenticates, checking the member against the
// CA's public key (--cluster-ca). See docs/cluster/security.md.
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
    // The founder already has its own enrollment certificate: it is what its
    // transport presents (#1513).
    EXPECT_FALSE(founder->enrolled_certificate_pem.empty());
    EXPECT_EQ(founder->cluster_ca_public_key, ca->public_key);
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

// ----- the transport (#1513): only enrollment certificates are trusted ----------

namespace {

// A node that holds the enrollment CA, with its own enrollment certificate.
ClusterSecurity founder(const std::string& token, const std::string& id, const EnrollmentCa& ca) {
    ClusterSecurity security = derived(token, id);
    security.enroll = true;
    security.enrollment_ca_seed = ca.seed;
    std::string reason;
    EXPECT_TRUE(zetta::cluster::self_enroll(security, &reason)) << reason;
    return security;
}

// A node that is not enrolled: the token and the CA's public key (--cluster-ca).
ClusterSecurity joiner(const std::string& token, const std::string& id,
                       const std::string& cluster_ca) {
    ClusterSecurity security = derived(token, id);
    security.enroll = true;
    security.cluster_ca_public_key = cluster_ca;
    return security;
}

}  // namespace

TEST(ClusterEnrollment, ASignedRequestOpensOnlyUnderItsKey) {
    const ClusterSecurity node = derived(zetta::cluster::generate_join_token(), "node-a");
    const std::string public_key = zetta::cluster::node_raw_public_key(node.private_key_pem);
    ASSERT_EQ(public_key.size(), 64u);
    EXPECT_EQ(zetta::cluster::raw_public_key_sha256(public_key),
              zetta::cluster::certificate_pem_public_key_sha256(node.certificate_pem))
        << "the request's key hash must be the one its certificate will carry";
    const std::string signature = zetta::cluster::sign_with_node_key(node.private_key_pem, "nonce");
    EXPECT_TRUE(zetta::cluster::verify_with_raw_public_key(public_key, "nonce", signature));
    EXPECT_FALSE(zetta::cluster::verify_with_raw_public_key(public_key, "other", signature));
    const ClusterSecurity other = derived(zetta::cluster::generate_join_token(), "node-b");
    EXPECT_FALSE(zetta::cluster::verify_with_raw_public_key(
        zetta::cluster::node_raw_public_key(other.private_key_pem), "nonce", signature));
}

TEST(ClusterEnrollment, ACertificateIsCheckedAgainstTheCasPublicKey) {
    const auto ca = zetta::cluster::generate_enrollment_ca();
    const auto other = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca && other);
    const ClusterSecurity node = founder(zetta::cluster::generate_join_token(), "node-a", *ca);
    const auto der = zetta::cluster::certificate_pem_to_der(node.enrolled_certificate_pem);
    EXPECT_TRUE(zetta::cluster::certificate_signed_by(der, ca->public_key));
    EXPECT_FALSE(zetta::cluster::certificate_signed_by(der, other->public_key));
    EXPECT_FALSE(zetta::cluster::certificate_signed_by(
        zetta::cluster::certificate_pem_to_der(node.certificate_pem), ca->public_key))
        << "a token-derived certificate passed for an enrollment one";
    EXPECT_EQ(zetta::cluster::enrollment_ca_public_key_of(ca->certificate_pem), ca->public_key);
    EXPECT_TRUE(zetta::cluster::enrollment_ca_public_key_of(node.ca_certificate_pem).empty());
}

TEST(ClusterEnrollment, AProvisionedSeedGivesTheNodeTheCa) {
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    ClusterSecurity node = derived(zetta::cluster::generate_join_token(), "node-a");
    node.cluster_ca_public_key = ca->public_key;
    std::string reason;
    ASSERT_TRUE(zetta::cluster::provision_enrollment_ca(node, ca->seed, &reason)) << reason;
    EXPECT_FALSE(node.enrolled_certificate_pem.empty());
    // Another CA than the one the node was told to trust is refused.
    ClusterSecurity mismatched = derived(zetta::cluster::generate_join_token(), "node-b");
    mismatched.cluster_ca_public_key = zetta::cluster::generate_enrollment_ca()->public_key;
    EXPECT_FALSE(zetta::cluster::provision_enrollment_ca(mismatched, ca->seed, &reason));
    EXPECT_FALSE(zetta::cluster::provision_enrollment_ca(node, "short", &reason));
}

// A joiner with the token and --cluster-ca enrolls over a link where only the
// member authenticates, is recorded as node_cert:<id>, and then links with full
// mutual TLS on its enrollment certificate.
TEST(ClusterEnrollment, AJoinerEnrollsAndIsRecorded) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);

    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46610, founder(token, id_a, *ca)), &replication_a);
    node_a.start();
    EXPECT_NE(node_a.peer_command().find("--cluster-ca " + ca->public_key), std::string::npos);

    const ClusterSecurity security_b = joiner(token, id_b, ca->public_key);
    ReplicationManager replication_b(id_b);
    Cluster node_b(id_b, config_on(46611, security_b, {"127.0.0.1:46610"}), &replication_b);
    node_b.start();

    ASSERT_TRUE(eventually([&] { return has_peer(node_a, id_b) && has_peer(node_b, id_a); }))
        << "the joiner did not link after enrolling";
    const std::string key_b = std::string(kNodeCertPrefix) + id_b;
    ASSERT_TRUE(eventually([&] { return replication_b.snapshot().contains(key_b); }));
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

// #1513's "done when": a node holding only the join token cannot join under any
// id. Its token-derived certificate fails the handshake, whatever id it names.
TEST(ClusterEnrollment, ATokenDerivedCertificateIsRefused) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_c = zetta::util::new_uuid();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);
    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46612, founder(token, id_a, *ca)), &replication_a);
    node_a.start();
    Cluster token_only(id_c, config_on(46613, derived(token, id_c), {"127.0.0.1:46612"}));
    token_only.start();
    sleep_ms(5000);  // several reconnect cycles
    EXPECT_FALSE(has_peer(node_a, id_c)) << "a token-derived certificate joined";
    token_only.stop();
    node_a.stop();
}

// A joiner without the cluster's token is refused at the enrollment step
// (bad-proof), and one told another CA refuses to enroll through the member.
TEST(ClusterEnrollment, EnrollingNeedsTheTokenAndTheRightCa) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const std::string id_c = zetta::util::new_uuid();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    const auto other_ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca && other_ca);
    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46614, founder(token, id_a, *ca)), &replication_a);
    node_a.start();
    Cluster wrong_token(id_b, config_on(46618, joiner(zetta::cluster::generate_join_token(), id_b,
                                                      ca->public_key),
                                        {"127.0.0.1:46614"}));
    wrong_token.start();
    Cluster wrong_ca(id_c, config_on(46619, joiner(token, id_c, other_ca->public_key),
                                     {"127.0.0.1:46614"}));
    wrong_ca.start();
    sleep_ms(5000);
    const auto snapshot = replication_a.snapshot();
    EXPECT_FALSE(snapshot.contains(std::string(kNodeCertPrefix) + id_b))
        << "a joiner without the token enrolled";
    EXPECT_FALSE(snapshot.contains(std::string(kNodeCertPrefix) + id_c))
        << "a joiner enrolled through a member outside its CA";
    EXPECT_FALSE(has_peer(node_a, id_b));
    EXPECT_FALSE(has_peer(node_a, id_c));
    wrong_ca.stop();
    wrong_token.stop();
    node_a.stop();
}

// #1319's "done when", now through the enrollment request: a token holder that
// asks to enroll an enrolled node's id with its own key is refused
// (enrolled-to-another-key), and a token-derived certificate for that id gets no
// link either.
TEST(ClusterEnrollment, ATokenHolderCannotTakeAnEnrolledId) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const auto ca = zetta::cluster::generate_enrollment_ca();
    ASSERT_TRUE(ca);

    ReplicationManager replication_a(id_a);
    Cluster node_a(id_a, config_on(46615, founder(token, id_a, *ca)), &replication_a);
    node_a.start();
    std::string enrolled_key;
    {
        ReplicationManager replication_b(id_b);
        const ClusterSecurity security_b = joiner(token, id_b, ca->public_key);
        enrolled_key = zetta::cluster::certificate_pem_public_key_sha256(security_b.certificate_pem);
        Cluster node_b(id_b, config_on(46616, security_b, {"127.0.0.1:46615"}), &replication_b);
        node_b.start();
        ASSERT_TRUE(eventually([&] { return has_peer(node_a, id_b); })) << "B did not enroll";
        node_b.stop();
    }
    ASSERT_TRUE(eventually([&] { return !has_peer(node_a, id_b); }));

    // The impostors: the same token, B's id, a fresh key.
    const ClusterSecurity asking = joiner(token, id_b, ca->public_key);
    ASSERT_NE(zetta::cluster::certificate_pem_public_key_sha256(asking.certificate_pem),
              enrolled_key);
    Cluster impostor(id_b, config_on(46617, asking, {"127.0.0.1:46615"}));
    impostor.start();
    Cluster token_only(id_b, config_on(46609, derived(token, id_b), {"127.0.0.1:46615"}));
    token_only.start();
    sleep_ms(5000);  // several reconnect cycles
    EXPECT_FALSE(has_peer(node_a, id_b)) << "an impostor took an enrolled node id";
    const auto record = replication_a.snapshot()[std::string(kNodeCertPrefix) + id_b];
    EXPECT_EQ(record["public_key_sha256"], enrolled_key) << "the enrollment record moved";

    token_only.stop();
    impostor.stop();
    node_a.stop();
}
