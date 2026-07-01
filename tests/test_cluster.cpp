#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster.hpp"
#include "zetta/replication.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::NodeStatus;
using zetta::ReplicationManager;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace

TEST(Cluster, PortRangeLetsNodesShareAHost) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 42100;
    config.port_range = 8;

    Cluster first(zetta::new_uuid(), config);
    first.start();
    Cluster second(zetta::new_uuid(), config);  // same base + range
    second.start();

    // Binding is synchronous in start(), so the second node falls forward.
    EXPECT_EQ(first.listen_address(), "127.0.0.1:42100");
    EXPECT_EQ(second.listen_address(), "127.0.0.1:42101");

    second.stop();
    first.stop();
}

TEST(Cluster, DefaultsToAllInterfacesAndAdvertisesReachableAddress) {
    // A plain node binds the wildcard so it's reachable across machines, but
    // never advertises the bare 0.0.0.0 — that isn't dial-able.
    EXPECT_EQ(ClusterConfig{}.listen_host, "0.0.0.0");

    Cluster node(zetta::new_uuid(), ClusterConfig{});
    node.start();
    const std::vector<std::string> addresses = node.listen_addresses();
    ASSERT_FALSE(addresses.empty());
    for (const std::string& address : addresses) {
        EXPECT_EQ(address.rfind("0.0.0.0:", 0), std::string::npos);
    }
    EXPECT_EQ(node.peer_command(), "zetta --peer " + addresses.front());
    node.stop();
}

TEST(Cluster, SpecificHostAdvertisesExactlyThatAddress) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 42150;
    config.port_range = 4;

    Cluster node(zetta::new_uuid(), config);
    node.start();

    // A non-wildcard bind is reachable exactly where it bound — one address.
    const std::vector<std::string> addresses = node.listen_addresses();
    ASSERT_EQ(addresses.size(), 1u);
    EXPECT_EQ(addresses.front(), "127.0.0.1:42150");
    EXPECT_EQ(node.peer_command(), "zetta --peer 127.0.0.1:42150");

    node.stop();
}

TEST(Cluster, WildcardBindExpandsToReachableAddresses) {
    ClusterConfig config;
    config.listen_host = "0.0.0.0";
    config.listen_port = 42160;
    config.port_range = 4;

    Cluster node(zetta::new_uuid(), config);
    node.start();

    // A wildcard bind isn't itself dial-able, so it expands to the machine's
    // interface IPv4s (falling back to loopback) — each a real host:port.
    const std::vector<std::string> addresses = node.listen_addresses();
    ASSERT_FALSE(addresses.empty());
    for (const std::string& address : addresses) {
        EXPECT_NE(address, "0.0.0.0:42160");
        EXPECT_NE(address.find(":42160"), std::string::npos);
    }
    // peer_command picks the first (non-loopback preferred) reachable address.
    EXPECT_EQ(node.peer_command(), "zetta --peer " + addresses.front());

    node.stop();
}

TEST(Cluster, PeersConnectAsEquals) {
    ClusterConfig first_config;
    first_config.listen_host = "127.0.0.1";
    first_config.listen_port = 42200;
    first_config.port_range = 1;
    Cluster first(zetta::new_uuid(), first_config);
    first.start();
    sleep_ms(100);  // let it bind/listen

    ClusterConfig second_config;
    second_config.listen_host = "127.0.0.1";
    second_config.listen_port = 42201;
    second_config.port_range = 1;
    second_config.seeds = {"127.0.0.1:42200"};
    Cluster second(zetta::new_uuid(), second_config);
    second.start();

    bool connected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (first.peer_count() == 1 && second.peer_count() == 1) {
            connected = true;
            break;
        }
        sleep_ms(20);
    }

    EXPECT_TRUE(connected);
    EXPECT_EQ(first.peer_count(), 1u);
    EXPECT_EQ(second.peer_count(), 1u);
    // Symmetric handshake: each side learned the other's advertised address.
    if (first.peer_count() == 1) {
        EXPECT_EQ(first.peers().front().address, "127.0.0.1:42201");
    }
    if (second.peer_count() == 1) {
        EXPECT_EQ(second.peers().front().address, "127.0.0.1:42200");
    }

    second.stop();
    first.stop();
}

TEST(Cluster, RemovesDeadPeerAndReconnects) {
    // Fast timeouts so the test doesn't wait on the 6s default.
    const std::string id_a = zetta::new_uuid();
    ReplicationManager repl_a{id_a};
    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 42300;
    cfg_a.port_range = 1;
    cfg_a.peer_idle_timeout_seconds = 1;
    cfg_a.reconnect_interval_seconds = 1;
    Cluster a{id_a, cfg_a, &repl_a};
    a.start();
    sleep_ms(100);

    const auto make_peer = [](const std::string& id, ReplicationManager& repl) {
        ClusterConfig cfg;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = 42301;
        cfg.port_range = 1;
        cfg.seeds = {"127.0.0.1:42300"};
        cfg.peer_idle_timeout_seconds = 1;
        cfg.reconnect_interval_seconds = 1;
        return std::make_unique<Cluster>(id, cfg, &repl);
    };

    const std::string id_b1 = zetta::new_uuid();
    ReplicationManager repl_b1{id_b1};
    auto b = make_peer(id_b1, repl_b1);
    b->start();

    const auto wait_for = [](auto predicate) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (predicate()) return true;
            sleep_ms(25);
        }
        return false;
    };

    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b->peer_count() == 1; }));
    EXPECT_EQ(a.peers().front().node_id, id_b1);

    // Peer goes away → the survivor drops it.
    b->stop();
    b.reset();
    EXPECT_TRUE(wait_for([&] { return a.peer_count() == 0; }));

    // A fresh node at the same address (new id) reconnects and replaces it —
    // no ghost entry left behind.
    const std::string id_b2 = zetta::new_uuid();
    ReplicationManager repl_b2{id_b2};
    auto b2 = make_peer(id_b2, repl_b2);
    b2->start();
    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b2->peer_count() == 1; }));
    EXPECT_EQ(a.peers().front().node_id, id_b2);  // the new node, not a ghost

    b2->stop();
    a.stop();
}

TEST(Cluster, GossipsReplicationStatusBetweenNodes) {
    // Each node seeds its own entry; after they connect, gossip should converge
    // their documents and nodes() should report the peer in-sync.
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    repl_a.set(id_a, nlohmann::json{{"address", "127.0.0.1:43010"}});
    repl_b.set(id_b, nlohmann::json{{"address", "127.0.0.1:43011"}});

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 43010;
    config_a.port_range = 1;
    Cluster a{id_a, config_a, &repl_a};
    a.start();
    sleep_ms(100);

    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 43011;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:43010"};
    Cluster b{id_b, config_b, &repl_b};
    b.start();

    // The peer entry is reported in-sync once the gossip exchange has both
    // merged the other's document and exchanged a matching status.
    const auto peer_in_sync = [](const Cluster& cluster) {
        const std::vector<NodeStatus> nodes = cluster.nodes();
        return nodes.size() == 2 && nodes[1].replication.in_sync;
    };
    bool synced = false;
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (peer_in_sync(a) && peer_in_sync(b)) {
            synced = true;
            break;
        }
        sleep_ms(50);
    }
    EXPECT_TRUE(synced);

    // Documents converged: both hold both nodes' entries at the same version.
    EXPECT_EQ(repl_a.entry_count(), 2u);
    EXPECT_EQ(repl_b.entry_count(), 2u);
    EXPECT_EQ(repl_a.version(), repl_b.version());

    // nodes() lists self first, then the connected, in-sync peer.
    const std::vector<NodeStatus> nodes = a.nodes();
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_TRUE(nodes[0].is_self);
    EXPECT_EQ(nodes[0].node_id, id_a);
    EXPECT_EQ(nodes[0].replication.entries, 2u);
    EXPECT_FALSE(nodes[1].is_self);
    EXPECT_EQ(nodes[1].node_id, id_b);
    EXPECT_TRUE(nodes[1].connected);
    EXPECT_TRUE(nodes[1].replication.known);
    EXPECT_TRUE(nodes[1].replication.in_sync);
    EXPECT_EQ(nodes[1].replication.entries, 2u);
    EXPECT_GE(nodes[1].replication.seconds_since_sync, 0.0);

    b.stop();
    a.stop();
}

TEST(Cluster, DialsRosterMemberItWasNeverSeededWith) {
    // Membership is replicated state: every node seeds its own node:<id> entry
    // (id + reachable address) and peers merge it. A node should dial every
    // member in that replicated roster, not just its configured --peer seeds —
    // so a node reaches peers it was never explicitly told about, via the CRDT.
    //
    // Topology: A is a hub. B seeds A. C seeds ONLY A. Once C's document merges
    // A's (which by then carries B's roster entry), C must dial B on its own —
    // even though C was never handed B as a seed.
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();
    const std::string id_c = zetta::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    ReplicationManager repl_c{id_c};
    repl_a.set("node:" + id_a, nlohmann::json{{"address", "127.0.0.1:43100"}});
    repl_b.set("node:" + id_b, nlohmann::json{{"address", "127.0.0.1:43101"}});
    repl_c.set("node:" + id_c, nlohmann::json{{"address", "127.0.0.1:43102"}});

    const auto make = [](const std::string& id, ReplicationManager& repl,
                         std::uint16_t port,
                         std::vector<std::string> seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.reconnect_interval_seconds = 1;  // dial the roster promptly
        config.seeds = std::move(seeds);
        return std::make_unique<Cluster>(id, config, &repl);
    };

    auto a = make(id_a, repl_a, 43100, {});
    a->start();
    sleep_ms(100);
    auto b = make(id_b, repl_b, 43101, {"127.0.0.1:43100"});
    auto c = make(id_c, repl_c, 43102, {"127.0.0.1:43100"});  // knows only A
    b->start();
    c->start();

    const auto wait_for = [](auto predicate) {
        for (int attempt = 0; attempt < 400; ++attempt) {
            if (predicate()) return true;
            sleep_ms(25);
        }
        return false;
    };

    // C connects to B without ever being seeded with B — it learned B's address
    // from the replicated roster (merged via A) and dialed it. Every node ends up
    // connected to the other two.
    ASSERT_TRUE(wait_for([&] {
        return a->peer_count() == 2 && b->peer_count() == 2 &&
               c->peer_count() == 2;
    }));

    // C holds a direct link to B specifically (not just to the hub A).
    bool c_sees_b = false;
    for (const zetta::ClusterPeer& peer : c->peers()) {
        if (peer.node_id == id_b) c_sees_b = true;
    }
    EXPECT_TRUE(c_sees_b);

    // nodes() reports the full replicated roster (all three), each connected.
    const std::vector<NodeStatus> nodes = c->nodes();
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_TRUE(nodes[0].is_self);
    for (const NodeStatus& node : nodes) EXPECT_TRUE(node.connected);

    c->stop();
    b->stop();
    a->stop();
}
