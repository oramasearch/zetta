#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster/cluster.hpp"
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
