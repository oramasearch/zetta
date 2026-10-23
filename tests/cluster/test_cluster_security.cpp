// Inter-node transport security (CTL-2, #466): the join token / derived
// cluster CA / per-node identity material, its persistence, and the cluster
// transport actually enforcing it — same-token nodes converge over mTLS,
// wrong-token and plaintext nodes never join. See docs/cluster/security.md.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

TEST(ClusterSecurity, PrivateStorageAlsoUpgradesLegacyFilesAndReplacesOldInodes) {
    zetta::test::TempDir temporary;
    const auto directory = temporary.path() / "security";
    const mode_t previous_umask = ::umask(0);
    const auto first = zetta::cluster::load_or_create_cluster_security(
        temporary.path().string(), "node");
    ::umask(previous_umask);
    ASSERT_TRUE(first);
    struct stat status {};
    ASSERT_EQ(::stat(directory.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);
    // Simulate a directory/file left by the older stream-based writer.
    ASSERT_EQ(::chmod(directory.c_str(), 0777), 0);
    for (const auto& name : {"cluster-secret.json", "node-cert.pem", "node-key.pem"}) {
        const auto path = directory / name;
        ASSERT_EQ(::chmod(path.c_str(), 0666), 0);
    }
    const auto secret_path = directory / "cluster-secret.json";
    const int retained = ::open(secret_path.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(retained, 0);
    struct stat before {};
    const int inspected = ::fstat(retained, &before);
    const auto rotated = zetta::cluster::load_or_create_cluster_security(
        temporary.path().string(), "node", std::string(64, 'a'));
    struct stat after {};
    const int checked = ::fstat(retained, &after);
    ::close(retained);
    ASSERT_EQ(inspected, 0);
    ASSERT_EQ(checked, 0);
    ASSERT_TRUE(rotated);
    // A descriptor opened before rotation still points at the old contents.
    EXPECT_EQ(before.st_size, after.st_size);
    ASSERT_EQ(::stat(secret_path.c_str(), &status), 0);
    EXPECT_NE(before.st_ino, status.st_ino);
    ASSERT_EQ(::stat(directory.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);
    for (const auto& name : {"cluster-secret.json", "node-cert.pem", "node-key.pem"}) {
        ASSERT_EQ(::stat((directory / name).c_str(), &status), 0);
        EXPECT_EQ(status.st_mode & 0777, 0600);
    }
}

TEST(ClusterSecurity, RefusesLinkedDirectoriesAndLinkedOrSpecialFiles) {
    for (const auto& name : {"cluster-secret.json", "node-cert.pem", "node-key.pem"}) {
        for (const bool hard_link : {false, true}) {
            zetta::test::TempDir temporary;
            const auto directory = temporary.path() / "security";
            std::filesystem::create_directory(directory);
            const auto outside = temporary.path() / "outside";
            const std::string contents = std::string_view(name) == "cluster-secret.json"
                ? R"({"version":1,"join_token":")" + std::string(64, 'a') + R"("})"
                : "dummy";
            std::ofstream(outside) << contents;
            if (hard_link) {
                std::filesystem::create_hard_link(outside, directory / name);
            } else {
                std::filesystem::create_symlink(outside, directory / name);
            }
            std::string reason;
            EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
                temporary.path().string(), "node", {}, &reason));
            EXPECT_FALSE(reason.empty());
            EXPECT_EQ(std::filesystem::file_size(outside), contents.size());
        }
    }
    zetta::test::TempDir temporary;
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(outside);
    std::filesystem::create_directory_symlink(outside, temporary.path() / "security");
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        temporary.path().string(), "node"));
    EXPECT_TRUE(std::filesystem::is_empty(outside));
    std::filesystem::remove(temporary.path() / "security");
    std::filesystem::create_directory(temporary.path() / "security");
    ASSERT_EQ(::mkfifo((temporary.path() / "security" / "node-key.pem").c_str(), 0600), 0);
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        temporary.path().string(), "node"));
}

// A re-keyed node keeps the tokens it ran under before (newest first, bounded),
// so a secret derived from the old token — the connector-credential sealing
// key (#1230) — can still open what was sealed under it and re-seal it.
TEST(ClusterSecurity, ReKeyingRetiresTheEarlierToken) {
    zetta::test::TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::string reason;
    auto first = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason);
    ASSERT_TRUE(first) << reason;
    EXPECT_TRUE(first->retired_join_tokens.empty());

    // The same token again retires nothing.
    auto same = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, first->join_token, &reason);
    ASSERT_TRUE(same) << reason;
    EXPECT_TRUE(same->retired_join_tokens.empty());

    std::vector<std::string> tokens{first->join_token};
    for (int round = 0; round < 6; ++round) {
        tokens.push_back(zetta::cluster::generate_join_token());
        auto rekeyed = zetta::cluster::load_or_create_cluster_security(
            dir.path().string(), node_id, tokens.back(), &reason);
        ASSERT_TRUE(rekeyed) << reason;
    }
    auto after = zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason);
    ASSERT_TRUE(after) << reason;
    EXPECT_EQ(after->join_token, tokens.back());
    ASSERT_EQ(after->retired_join_tokens.size(),
              zetta::cluster::kMaxRetiredJoinTokens);
    // Newest first: the token before the current one leads.
    EXPECT_EQ(after->retired_join_tokens.front(), tokens[tokens.size() - 2]);
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

// The peer command is printed on every headless start, so it lands in pod logs
// and log pipelines (#1393): it names where the join token goes and never
// carries the token itself. Only the explicit accessor, behind the headed app's
// Copy button, returns the token.
TEST(ClusterSecurity, PeerCommandNeverCarriesTheJoinToken) {
    const std::string token = zetta::cluster::generate_join_token();
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 42400;
    config.security = must_derive(token, "node-a");

    Cluster node("node-a", config);
    node.start();
    EXPECT_EQ(node.peer_command(),
              "zetta --peer 127.0.0.1:42400 --join-token " +
                  std::string(zetta::cluster::kJoinTokenPlaceholder));
    EXPECT_EQ(node.peer_command().find(token), std::string::npos);
    EXPECT_EQ(node.peer_command_with_join_token(),
              "zetta --peer 127.0.0.1:42400 --join-token " + token);
    node.stop();
}

// A secret file that fails validation is reported with a reason that names the
// file and the field, never the value it holds (#1393): the reason is printed
// to stderr on a refused start.
TEST(ClusterSecurity, ASecretFileRefusalNeverEchoesTheToken) {
    zetta::test::TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::filesystem::create_directories(dir.path() / "security");

    // Upper-case hex: the right length, the wrong alphabet.
    const std::string bad_token(64, 'A');
    const std::string good_token = zetta::cluster::generate_join_token();
    std::ofstream(dir.path() / "security" / "cluster-secret.json")
        << R"({"version": 1, "join_token": ")" + bad_token + R"("})";
    std::string reason;
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(reason.find(bad_token), std::string::npos) << reason;

    // A well-formed current token beside a malformed retired one.
    std::ofstream(dir.path() / "security" / "cluster-secret.json")
        << R"({"version": 1, "join_token": ")" + good_token +
               R"(", "retired_join_tokens": [")" + bad_token + R"("]})";
    reason.clear();
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, {}, &reason));
    EXPECT_EQ(reason.find(bad_token), std::string::npos) << reason;
    EXPECT_EQ(reason.find(good_token), std::string::npos) << reason;

    // A malformed --join-token override is refused without repeating it.
    reason.clear();
    EXPECT_FALSE(zetta::cluster::load_or_create_cluster_security(
        dir.path().string(), node_id, bad_token, &reason));
    EXPECT_EQ(reason.find(bad_token), std::string::npos) << reason;
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

// The TLS-transport stress gate (#786). Interleaving SSL_read and SSL_write across
// coroutine await points on ONE SSL object corrupts the OpenSSL state machine, so a
// mutual-TLS link drops under sustained bidirectional gossip while a plaintext link
// (raw full-duplex socket) syncs fine — the isolation of the fault. This drives a
// 3-node, fully-meshed mTLS cluster under continuous, sizable, concurrent mutations
// on every node, then asserts (a) no link ever dropped (connection_count stayed at
// the full-mesh count — a TLS corruption would tear a link down and reconnect) and
// (b) the documents converge. It is the key validation gate for the per-link TLS
// I/O serialization; on the buggy transport it flaps/fails to converge.
TEST(ClusterSecurity, MeshConvergesOverMutualTlsUnderSustainedLoad) {
    const std::string token = zetta::cluster::generate_join_token();
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const std::string id_c = zetta::util::new_uuid();

    ReplicationManager repl_a(id_a);
    ReplicationManager repl_b(id_b);
    ReplicationManager repl_c(id_c);
    // Each node publishes its membership entry so the hub-seeded star meshes (#789).
    repl_a.set("node:" + id_a, {{"address", "127.0.0.1:42440"}});
    repl_b.set("node:" + id_b, {{"address", "127.0.0.1:42441"}});
    repl_c.set("node:" + id_c, {{"address", "127.0.0.1:42442"}});

    const auto make = [&](const std::string& id, std::uint16_t port,
                          bool seed_hub) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.reconnect_interval_seconds = 1;
        if (seed_hub) config.seeds = {"127.0.0.1:42440"};
        config.security = must_derive(token, id);
        return config;
    };

    Cluster a(id_a, make(id_a, 42440, false), &repl_a);
    Cluster b(id_b, make(id_b, 42441, true), &repl_b);
    Cluster c(id_c, make(id_c, 42442, true), &repl_c);
    a.start();
    b.start();
    c.start();

    // Full mesh over TLS: every node linked to both others.
    const auto meshed = [&] {
        return a.peer_count() == 2 && b.peer_count() == 2 && c.peer_count() == 2;
    };
    ASSERT_TRUE([&] {
        for (int waited = 0; waited < 20000; waited += 50) {
            if (meshed()) return true;
            sleep_ms(50);
        }
        return meshed();
    }()) << "mesh did not form over mTLS";

    // Hammer every node's document concurrently with sizable values so each SYNC
    // line spans multiple TLS records — exercising the read-pump / write-pump
    // interleaving on each TLS stream. Overwrite a bounded key set at a moderate
    // rate: enough sustained bidirectional traffic to trip the #786 corruption,
    // without pathological Automerge history growth that would just make merge slow.
    std::atomic<bool> stop{false};
    std::atomic<int> min_connections{100};
    const auto hammer = [&](ReplicationManager& repl, const std::string& tag) {
        const std::string blob(512, 'x');
        for (std::uint64_t i = 0; !stop.load(); ++i) {
            for (int key = 0; key < 4; ++key) {
                repl.set(tag + ":" + std::to_string(key),
                         {{"n", i}, {"blob", blob}});
            }
            sleep_ms(50);
        }
    };
    std::thread ta(hammer, std::ref(repl_a), "a");
    std::thread tb(hammer, std::ref(repl_b), "b");
    std::thread tc(hammer, std::ref(repl_c), "c");

    // Watch the links for ~6s: a TLS corruption drops a link (connection_count
    // dips below the full-mesh 2) before the reconnect loop heals it.
    for (int waited = 0; waited < 5000; waited += 50) {
        const int lowest = std::min({static_cast<int>(a.connection_count()),
                                     static_cast<int>(b.connection_count()),
                                     static_cast<int>(c.connection_count())});
        int prior = min_connections.load();
        while (lowest < prior && !min_connections.compare_exchange_weak(prior, lowest)) {
        }
        sleep_ms(50);
    }

    stop.store(true);
    ta.join();
    tb.join();
    tc.join();

    // The documents converge once mutations stop and a final gossip round lands.
    const auto converged = [&] {
        return repl_a.version() == repl_b.version() &&
               repl_b.version() == repl_c.version() &&
               repl_a.entry_count() == repl_b.entry_count() &&
               repl_b.entry_count() == repl_c.entry_count();
    };
    const bool did_converge = [&] {
        for (int waited = 0; waited < 45000; waited += 100) {
            if (converged()) return true;
            sleep_ms(100);
        }
        return converged();
    }();
    std::fprintf(stderr,
                 "[diag] min_conn=%d  entries a=%zu b=%zu c=%zu  "
                 "ver a=%s b=%s c=%s\n",
                 min_connections.load(), repl_a.entry_count(),
                 repl_b.entry_count(), repl_c.entry_count(),
                 repl_a.version().c_str(), repl_b.version().c_str(),
                 repl_c.version().c_str());
    EXPECT_TRUE(did_converge) << "documents did not converge over mTLS under load";

    // No TLS link ever dropped under the sustained bidirectional load.
    EXPECT_EQ(min_connections.load(), 2)
        << "a mutual-TLS link dropped under load (SSL state corruption, #786)";

    c.stop();
    b.stop();
    a.stop();
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

    // No security at all (a bare-embedded Cluster with all-empty security — the
    // only way to reach plaintext now that #799 removed the CLI opt-out): its
    // HELLO bytes are not a TLS ClientHello, so the TLS side drops it.
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
