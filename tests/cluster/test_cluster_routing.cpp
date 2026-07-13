#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

// Routing-table behaviours for #739 (stacked on the per-node-id link dedup, #741):
//   - a node advertises a SET of candidate endpoints, and the set round-trips in the
//     HELLO (multi-endpoint advertise);
//   - the endpoint a peer reached us on is recorded per-observer (the reachable path
//     differs by vantage point);
//   - a node dialing a peer via a NON-advertised address (loopback while the peer
//     advertises its LAN IP) converges to ONE stable connection with NO redial churn
//     (reconnect deduped by node id, not by matching the seed string);
//   - the dialer tries a peer's candidate endpoints in order until one connects;
//   - membership survives a brief peer blip (hysteresis debounces the down edge).

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterPeer;
using zetta::cluster::NodeStatus;
using zetta::cluster::ReplicationManager;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Poll a predicate patiently (seconds, not a fixed tiny count) — multi-node tests
// run real in-process Corosio TCP, so convergence timing is not instantaneous.
template <typename Predicate>
bool wait_for(Predicate predicate, int max_ms = 8000, int step_ms = 25) {
    for (int waited = 0; waited < max_ms; waited += step_ms) {
        if (predicate()) return true;
        sleep_ms(step_ms);
    }
    return predicate();
}

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

// A node advertises its full candidate-endpoint SET (bound address + --advertise
// extras), and the whole set round-trips in the HELLO. The observer records the
// endpoint it actually reached the peer on (the address it dialed), not merely the
// peer's advertised primary — per-observer path selection.
TEST(ClusterRouting, MultiEndpointHelloRoundTrips) {
    const std::string a_id = zetta::util::new_uuid();
    const std::string b_id = zetta::util::new_uuid();

    ClusterConfig a_config;
    a_config.listen_host = "127.0.0.1";
    a_config.listen_port = 43110;
    a_config.port_range = 1;
    // An extra reachable endpoint the operator knows about (e.g. an LB/public
    // address); 192.0.2.0/24 is TEST-NET-1 — a valid host:port that is never local.
    a_config.advertise = {"192.0.2.7:43110"};

    ClusterConfig b_config;
    b_config.listen_host = "127.0.0.1";
    b_config.listen_port = 43111;
    b_config.port_range = 1;
    b_config.seeds = {"127.0.0.1:43110"};

    Cluster a(a_id, a_config);
    Cluster b(b_id, b_config);
    a.start();
    b.start();

    ASSERT_TRUE(wait_for([&] { return b.peer_count() == 1 && a.peer_count() == 1; }));

    // B dialed A on loopback, so B learns A's whole advertised set AND records the
    // loopback as the endpoint it reached A on.
    const ClusterPeer a_seen_by_b = b.peers().front();
    EXPECT_EQ(a_seen_by_b.node_id, a_id);
    EXPECT_EQ(a_seen_by_b.address, "127.0.0.1:43110");  // the path B dialed
    ASSERT_EQ(a_seen_by_b.endpoints.size(), 2u);
    EXPECT_TRUE(contains(a_seen_by_b.endpoints, "127.0.0.1:43110"));
    EXPECT_TRUE(contains(a_seen_by_b.endpoints, "192.0.2.7:43110"));  // the extra

    // The node's own advertised set carries the extra too.
    EXPECT_TRUE(contains(a.advertised_endpoints(), "127.0.0.1:43110"));
    EXPECT_TRUE(contains(a.advertised_endpoints(), "192.0.2.7:43110"));

    // A accepted B's single-endpoint HELLO — a one-element set, backward compatible.
    const ClusterPeer b_seen_by_a = a.peers().front();
    EXPECT_EQ(b_seen_by_a.node_id, b_id);
    ASSERT_EQ(b_seen_by_a.endpoints.size(), 1u);
    EXPECT_EQ(b_seen_by_a.endpoints.front(), "127.0.0.1:43111");

    b.stop();
    a.stop();
}

// The flap's root cause: a node dials a peer via an address the peer does NOT
// advertise as its primary (loopback, while the peer binds the wildcard and
// advertises its LAN IP). The old reconnect matched the seed STRING against the
// peer's advertised address, never matched, and redialed every interval. Deduped by
// node id, the dialer converges to exactly ONE served connection and never redials.
TEST(ClusterRouting, LoopbackDialToWildcardPeerConvergesWithoutChurn) {
    ClusterConfig a_config;
    a_config.listen_host = "0.0.0.0";  // wildcard: advertises its LAN IP (not loopback)
    a_config.listen_port = 43120;
    a_config.port_range = 1;

    ClusterConfig b_config;
    b_config.listen_host = "127.0.0.1";
    b_config.listen_port = 43121;
    b_config.port_range = 1;
    b_config.seeds = {"127.0.0.1:43120"};    // a NON-advertised path to A (loopback)
    b_config.reconnect_interval_seconds = 1;  // redial promptly, so churn would show

    Cluster a(zetta::util::new_uuid(), a_config);
    Cluster b(zetta::util::new_uuid(), b_config);
    a.start();
    b.start();

    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));

    // Let several reconnect cycles pass. A redial-churn bug would push
    // connection_count() above one (each mismatched redial reaped but re-made); the
    // node-id dedup pins it at exactly one served connection.
    sleep_ms(3500);
    EXPECT_EQ(b.connection_count(), 1u);
    EXPECT_EQ(a.connection_count(), 1u);
    EXPECT_EQ(b.peer_count(), 1u);
    EXPECT_EQ(a.peer_count(), 1u);
    EXPECT_EQ(b.connection_count(), b.peer_count());
    EXPECT_EQ(a.connection_count(), a.peer_count());

    // B recorded the endpoint it actually reached A on — the loopback it dialed —
    // even though A's advertised primary may be a LAN IP. Per-observer path.
    EXPECT_EQ(b.peers().front().address, "127.0.0.1:43120");

    b.stop();
    a.stop();
}

// A peer may be reachable at several candidate endpoints; a seed can list them
// comma-separated. The dialer tries them in order and keeps the first that connects
// (skipping the unreachable one) — the reachable path from this vantage point.
TEST(ClusterRouting, DialTriesCandidateEndpointsUntilOneConnects) {
    const std::string a_id = zetta::util::new_uuid();

    ClusterConfig a_config;
    a_config.listen_host = "127.0.0.1";
    a_config.listen_port = 43130;
    a_config.port_range = 1;

    ClusterConfig b_config;
    b_config.listen_host = "127.0.0.1";
    b_config.listen_port = 43131;
    b_config.port_range = 1;
    // One peer, two candidate endpoints: the first refuses fast (nothing on
    // 127.0.0.1:1), the second is A. The dialer falls through to the reachable one.
    b_config.seeds = {"127.0.0.1:1,127.0.0.1:43130"};
    b_config.reconnect_interval_seconds = 1;

    Cluster a(a_id, a_config);
    Cluster b(zetta::util::new_uuid(), b_config);
    a.start();
    b.start();

    ASSERT_TRUE(wait_for([&] { return b.peer_count() == 1 && a.peer_count() == 1; }));
    EXPECT_EQ(b.peers().front().node_id, a_id);
    EXPECT_EQ(b.peers().front().address, "127.0.0.1:43130");  // the reachable candidate

    // Still exactly one served connection after a few reconnect cycles (the dead
    // candidate must not accumulate dials).
    sleep_ms(2500);
    EXPECT_EQ(b.connection_count(), 1u);
    EXPECT_EQ(b.peer_count(), 1u);

    b.stop();
    a.stop();
}

// Fix #789: a hub-seeded topology (every node --peer'd at ONE seed, as a k8s
// StatefulSet does) must fill into a full equal-peer mesh — not stay a STAR rooted
// at the seed. Two spokes (B, C) seed only the hub (A). Once they learn each other
// through gossiped membership (`node:<id>` entries merged via A), the reconnect loop
// dials the learned member directly, so every node ends up linked to every other.
TEST(ClusterRouting, HubSeededTopologyFillsIntoFullMesh) {
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const std::string id_c = zetta::util::new_uuid();

    // Each node publishes its own membership entry, exactly as main.cpp does; gossip
    // merges them so every node learns every node's address.
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    ReplicationManager repl_c{id_c};
    repl_a.set("node:" + id_a, nlohmann::json{{"address", "127.0.0.1:43160"}});
    repl_b.set("node:" + id_b, nlohmann::json{{"address", "127.0.0.1:43161"}});
    repl_c.set("node:" + id_c, nlohmann::json{{"address", "127.0.0.1:43162"}});

    ClusterConfig cfg_a;  // the hub — no seeds of its own
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43160;
    cfg_a.port_range = 1;
    cfg_a.reconnect_interval_seconds = 1;

    const auto spoke = [](std::uint16_t port) {
        ClusterConfig cfg;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = port;
        cfg.port_range = 1;
        cfg.seeds = {"127.0.0.1:43160"};  // seed ONLY the hub
        cfg.reconnect_interval_seconds = 1;
        return cfg;
    };

    Cluster a(id_a, cfg_a, &repl_a);
    Cluster b(id_b, spoke(43161), &repl_b);
    Cluster c(id_c, spoke(43162), &repl_c);
    a.start();
    b.start();
    c.start();

    // Every node linked to BOTH others — the mesh, not a star (patient: the mesh
    // forms over gossip propagation + a couple reconnect cycles).
    ASSERT_TRUE(wait_for(
        [&] {
            return a.peer_count() == 2 && b.peer_count() == 2 &&
                   c.peer_count() == 2;
        },
        15000));

    // Exactly one served connection per peer — both spokes dial each other, and the
    // per-node-id registry reaps the duplicate to one (no leak, #741/#789).
    sleep_ms(2500);
    EXPECT_EQ(a.connection_count(), 2u);
    EXPECT_EQ(b.connection_count(), 2u);
    EXPECT_EQ(c.connection_count(), 2u);
    EXPECT_EQ(a.peer_count(), 2u);
    EXPECT_EQ(b.peer_count(), 2u);
    EXPECT_EQ(c.peer_count(), 2u);

    c.stop();
    b.stop();
    a.stop();
}

// A seed given as a DNS HOSTNAME (not a numeric IPv4) is resolved at dial time and
// the node connects to the resolved address — the Kubernetes StatefulSet per-pod
// DNS case, which never dialed before because the dialer accepted only numeric IPs
// (#785). `localhost` always resolves to loopback, so this is hermetic.
TEST(ClusterRouting, HostnameSeedResolvesAndConnects) {
    const std::string a_id = zetta::util::new_uuid();

    ClusterConfig a_config;
    a_config.listen_host = "127.0.0.1";
    a_config.listen_port = 43150;
    a_config.port_range = 1;

    ClusterConfig b_config;
    b_config.listen_host = "127.0.0.1";
    b_config.listen_port = 43151;
    b_config.port_range = 1;
    b_config.seeds = {"localhost:43150"};     // a DNS name, not 127.0.0.1
    b_config.reconnect_interval_seconds = 1;  // redial promptly

    Cluster a(a_id, a_config);
    Cluster b(zetta::util::new_uuid(), b_config);
    a.start();
    b.start();

    ASSERT_TRUE(wait_for([&] { return b.peer_count() == 1 && a.peer_count() == 1; }));
    EXPECT_EQ(b.peers().front().node_id, a_id);
    // The recorded reached endpoint is the STABLE seed label (the hostname), not
    // the ephemeral resolved IP — so a pod-IP change doesn't rewrite the path.
    EXPECT_EQ(b.peers().front().address, "localhost:43150");

    // Still exactly one served connection after several reconnect cycles: each
    // redial re-resolves (no cached IP), and the node-id dedup keeps it at one.
    sleep_ms(2500);
    EXPECT_EQ(b.connection_count(), 1u);
    EXPECT_EQ(b.peer_count(), 1u);

    b.stop();
    a.stop();
}

// Membership hysteresis (the down edge): when a peer's live link drops, the live
// views (peer_count/peers) reflect it immediately, but the MEMBERSHIP view (nodes())
// keeps the node through the grace window so a brief blip doesn't flap it out of
// system.nodes / the UI. It ages out only after a sustained absence.
TEST(ClusterRouting, MembershipSurvivesPeerBlip) {
    const std::string a_id = zetta::util::new_uuid();
    const std::string b_id = zetta::util::new_uuid();

    ClusterConfig a_config;
    a_config.listen_host = "127.0.0.1";
    a_config.listen_port = 43140;
    a_config.port_range = 1;
    a_config.membership_grace_seconds = 2;  // short, so the test is quick

    ClusterConfig b_config;
    b_config.listen_host = "127.0.0.1";
    b_config.listen_port = 43141;
    b_config.port_range = 1;
    b_config.seeds = {"127.0.0.1:43140"};

    Cluster a(a_id, a_config);
    Cluster b(b_id, b_config);
    a.start();
    b.start();

    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && a.nodes().size() == 2; }));

    // B drops. The live link is gone at once...
    b.stop();
    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 0; }));

    // ...but membership is retained through the grace window: A still lists B in
    // nodes(), flagged not-connected (the anti-flap). This holds well inside the 2s
    // grace since the link dropped only moments ago.
    const std::vector<NodeStatus> during_grace = a.nodes();
    ASSERT_EQ(during_grace.size(), 2u);
    EXPECT_TRUE(during_grace[0].is_self);
    EXPECT_EQ(during_grace[1].node_id, b_id);
    EXPECT_FALSE(during_grace[1].connected);  // a member, but not currently connected

    // After a sustained absence past the grace window, B ages out of membership.
    EXPECT_TRUE(wait_for([&] { return a.nodes().size() == 1; }, 6000));
    EXPECT_EQ(a.nodes().front().node_id, a_id);

    a.stop();
}

// A dial-out-only node (behind NAT / a private VNet, #803) advertises NO dialable
// address: it dials wan peers OUTBOUND and they must never dial it back — its
// private/bound IP is unroutable and collides across clusters sharing a pod CIDR
// (the "7 not 9" membership undercount). Its one inbound link carries CRDT sync both
// ways. Here B is dial-out-only and seeds A (wan): they converge, A records B as
// dial-out-only with an EMPTY candidate set (so A never dials it), and B advertises
// nothing dialable.
TEST(ClusterRouting, DialOutOnlyNodeAdvertisesNoDialableAddress) {
    const std::string a_id = zetta::util::new_uuid();
    const std::string b_id = zetta::util::new_uuid();

    ClusterConfig a_config;
    a_config.listen_host = "127.0.0.1";
    a_config.listen_port = 43140;
    a_config.port_range = 1;

    ClusterConfig b_config;
    b_config.listen_host = "127.0.0.1";
    b_config.listen_port = 43141;
    b_config.port_range = 1;
    b_config.seeds = {"127.0.0.1:43140"};
    b_config.dial_out_only = true;  // no routable address; a pure initiator

    Cluster a(a_id, a_config);
    Cluster b(b_id, b_config);
    a.start();
    b.start();

    ASSERT_TRUE(
        wait_for([&] { return b.peer_count() == 1 && a.peer_count() == 1; }));

    // B advertises nothing dialable; A is a routable wan node.
    EXPECT_TRUE(b.dial_out_only());
    EXPECT_TRUE(b.advertised_endpoints().empty());
    EXPECT_TRUE(b.primary_advertised_address().empty());
    EXPECT_FALSE(a.dial_out_only());
    EXPECT_FALSE(a.advertised_endpoints().empty());
    EXPECT_FALSE(a.primary_advertised_address().empty());

    // A records B as dial-out-only with an EMPTY candidate set + empty reached
    // address — nothing for the dialer to ever target.
    const ClusterPeer b_seen_by_a = a.peers().front();
    EXPECT_EQ(b_seen_by_a.node_id, b_id);
    EXPECT_TRUE(b_seen_by_a.dial_out_only);
    EXPECT_TRUE(b_seen_by_a.endpoints.empty());
    EXPECT_TRUE(b_seen_by_a.address.empty());

    // B sees A as a normal wan peer (candidate set present, not dial-out).
    const ClusterPeer a_seen_by_b = b.peers().front();
    EXPECT_EQ(a_seen_by_b.node_id, a_id);
    EXPECT_FALSE(a_seen_by_b.dial_out_only);
    EXPECT_FALSE(a_seen_by_b.endpoints.empty());

    a.stop();
    b.stop();
}

// system.cluster_nodes' source (#800): Cluster::membership() lists the FULL
// membership from the replicated node-list — every node:<id> the CRDT knows,
// including nodes THIS one has no direct link to — annotated with reachability and
// whether a live link is held. A wan node's entry carries a dialable address; a
// dial-out-only node's carries none.
TEST(ClusterRouting, MembershipListsFullNodeListWithReachability) {
    const std::string self_id = zetta::util::new_uuid();
    const std::string wan_id = zetta::util::new_uuid();
    const std::string dialout_id = zetta::util::new_uuid();

    ReplicationManager replication{self_id};
    // Seed the node-list exactly as a running node does (main.cpp): a dialable
    // address + reachability for a wan node, empty + dial-out-only for the other.
    replication.set("node:" + self_id,
                    nlohmann::json{{"address", "10.0.0.1:7777"},
                                   {"reachability", "wan-accessible"}});
    replication.set("node:" + wan_id,
                    nlohmann::json{{"address", "203.0.113.5:7777"},
                                   {"reachability", "wan-accessible"}});
    replication.set("node:" + dialout_id,
                    nlohmann::json{{"address", ""},
                                   {"reachability", "dial-out-only"}});

    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 43150;
    Cluster cluster(self_id, config, &replication);
    // No start()/live links needed — membership() reads the CRDT snapshot + the
    // (empty) peer table, so only self is "directly connected" here.

    std::vector<zetta::cluster::ClusterMember> members = cluster.membership();
    ASSERT_EQ(members.size(), 3u);

    const auto find = [&](const std::string& id) -> const
        zetta::cluster::ClusterMember* {
        for (const auto& member : members) {
            if (member.node_id == id) return &member;
        }
        return nullptr;
    };

    const auto* self = find(self_id);
    ASSERT_NE(self, nullptr);
    EXPECT_TRUE(self->is_self);
    EXPECT_TRUE(self->directly_connected);  // self is always "reachable"
    EXPECT_FALSE(self->dial_out_only);

    const auto* wan = find(wan_id);
    ASSERT_NE(wan, nullptr);
    EXPECT_FALSE(wan->is_self);
    EXPECT_FALSE(wan->directly_connected);  // no live link stood up in this test
    EXPECT_FALSE(wan->dial_out_only);
    EXPECT_EQ(wan->address, "203.0.113.5:7777");

    const auto* dialout = find(dialout_id);
    ASSERT_NE(dialout, nullptr);
    EXPECT_TRUE(dialout->dial_out_only);
    EXPECT_TRUE(dialout->address.empty());
}
