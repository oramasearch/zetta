// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster/cluster.hpp"
#include "zetta/util/uuid.hpp"

// Bounds on a peer's HELLO (#771). The node id and advertised endpoints a peer
// sends are stored in the peer table, shown in the node list and logs, and dialed
// by the reconnect loop, so the HELLO is held to:
//   - at most kMaxAdvertisedEndpoints endpoints;
//   - a node id of [A-Za-z0-9._-], at most kMaxNodeIdBytes;
//   - endpoints shaped HOST:PORT with no control characters or spaces;
//   - a frame of at most kMaxHelloBytes before it is parsed.
// A node also never advertises more endpoints than its peers accept.

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterPeer;
using zetta::cluster::is_valid_node_id;
using zetta::cluster::is_valid_peer_endpoint;
using zetta::cluster::kMaxAdvertisedEndpoints;
using zetta::cluster::kMaxEndpointBytes;
using zetta::cluster::kMaxHelloBytes;
using zetta::cluster::kMaxNodeIdBytes;
using zetta::cluster::parse_hello;

namespace {

const std::string kValidId = "0b6f0c8e-6f43-4b8e-9a43-6f3e2d1c0b9a";

std::string hello(const std::string& node_id, const std::string& endpoints,
                  const std::string& reachability = "wan") {
    return "ZETTA/1 HELLO " + node_id + " " + endpoints + " " + reachability + " 1";
}

std::string endpoint_list(std::size_t count) {
    std::string joined;
    for (std::size_t index = 0; index < count; ++index) {
        if (!joined.empty()) joined.push_back(',');
        joined += "10.0.0." + std::to_string(index + 1) + ":7000";
    }
    return joined;
}

template <typename Predicate>
bool wait_for(Predicate predicate, int max_ms = 8000, int step_ms = 25) {
    for (int waited = 0; waited < max_ms; waited += step_ms) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return predicate();
}

// A plain TCP client standing in for a hostile peer on a plaintext cluster.
class RawPeer {
   public:
    explicit RawPeer(std::uint16_t port) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        connected_ = ::connect(socket_, reinterpret_cast<sockaddr*>(&address),
                               sizeof address) == 0;
        timeval timeout{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    }
    ~RawPeer() {
        if (socket_ >= 0) ::close(socket_);
    }
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    bool connected() const { return connected_; }

    bool send_text(const std::string& text) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t count = ::send(socket_, text.data() + sent, text.size() - sent, 0);
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    // True when the node closed the connection (EOF or reset) within the receive
    // timeout; false when it was still holding the connection open.
    bool closed_by_node() {
        std::string discarded(4096, '\0');
        for (;;) {
            const ssize_t count = ::recv(socket_, discarded.data(), discarded.size(), 0);
            if (count == 0) return true;
            if (count < 0) return errno == ECONNRESET || errno == EPIPE;
        }
    }

   private:
    int socket_ = -1;
    bool connected_ = false;
};

ClusterConfig listening_on(std::uint16_t port) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    return config;
}

}  // namespace

TEST(ClusterHelloBounds, NodeIdMustBeAPlainToken) {
    EXPECT_TRUE(is_valid_node_id(kValidId));
    EXPECT_TRUE(is_valid_node_id("node_a.1"));
    EXPECT_TRUE(is_valid_node_id(std::string(kMaxNodeIdBytes, 'a')));

    EXPECT_FALSE(is_valid_node_id(""));
    EXPECT_FALSE(is_valid_node_id(std::string(kMaxNodeIdBytes + 1, 'a')));
    EXPECT_FALSE(is_valid_node_id("abc\x1b[31m"));  // a terminal escape
    EXPECT_FALSE(is_valid_node_id("abc\x01"));
    EXPECT_FALSE(is_valid_node_id("a b"));
    EXPECT_FALSE(is_valid_node_id("a/b"));
}

TEST(ClusterHelloBounds, EndpointMustBeHostColonPort) {
    EXPECT_TRUE(is_valid_peer_endpoint("127.0.0.1:7000"));
    EXPECT_TRUE(is_valid_peer_endpoint("zetta-0.zetta.svc.cluster.local:7000"));
    EXPECT_TRUE(is_valid_peer_endpoint("[fe80::1%en0]:7000"));
    EXPECT_TRUE(is_valid_peer_endpoint("::1:7000"));  // an unbracketed IPv6 listen host
    EXPECT_TRUE(is_valid_peer_endpoint("h:1"));
    EXPECT_TRUE(is_valid_peer_endpoint("h:65535"));
    EXPECT_TRUE(is_valid_peer_endpoint(std::string(kMaxEndpointBytes - 6, 'a') + ":65535"));

    EXPECT_FALSE(is_valid_peer_endpoint(""));
    EXPECT_FALSE(is_valid_peer_endpoint("127.0.0.1"));        // no port
    EXPECT_FALSE(is_valid_peer_endpoint(":7000"));            // no host
    EXPECT_FALSE(is_valid_peer_endpoint("host:"));            // empty port
    EXPECT_FALSE(is_valid_peer_endpoint("host:0"));
    EXPECT_FALSE(is_valid_peer_endpoint("host:65536"));
    EXPECT_FALSE(is_valid_peer_endpoint("host:+80"));
    EXPECT_FALSE(is_valid_peer_endpoint("host:80x"));
    EXPECT_FALSE(is_valid_peer_endpoint("host:000080"));      // over five digits
    EXPECT_FALSE(is_valid_peer_endpoint("ho st:80"));
    EXPECT_FALSE(is_valid_peer_endpoint("evil\x1b[2J:80"));   // clears a terminal
    EXPECT_FALSE(is_valid_peer_endpoint("evil\r\nforged log line:80"));
    EXPECT_FALSE(is_valid_peer_endpoint("\"quoted\":80"));
    EXPECT_FALSE(is_valid_peer_endpoint(std::string(kMaxEndpointBytes - 5, 'a') + ":65535"));
}

TEST(ClusterHelloBounds, WellFormedHelloParses) {
    const ClusterPeer peer = parse_hello(hello(kValidId, "10.0.0.1:7000,192.0.2.7:7000"));
    EXPECT_EQ(peer.node_id, kValidId);
    ASSERT_EQ(peer.endpoints.size(), 2u);
    EXPECT_EQ(peer.address, "10.0.0.1:7000");
    EXPECT_FALSE(peer.dial_out_only);
    EXPECT_EQ(peer.audit_protocol_version, 1u);

    // A legacy HELLO: one address, no reachability token.
    const ClusterPeer legacy = parse_hello("ZETTA/1 HELLO " + kValidId + " 10.0.0.1:7000");
    EXPECT_EQ(legacy.node_id, kValidId);
    EXPECT_EQ(legacy.endpoints.size(), 1u);

    // A dial-out-only node advertises the empty set.
    const ClusterPeer dial_out = parse_hello(hello(kValidId, "-", "dial-out"));
    EXPECT_EQ(dial_out.node_id, kValidId);
    EXPECT_TRUE(dial_out.endpoints.empty());
    EXPECT_TRUE(dial_out.dial_out_only);

    // Exactly the cap is accepted.
    EXPECT_EQ(parse_hello(hello(kValidId, endpoint_list(kMaxAdvertisedEndpoints))).endpoints.size(),
              kMaxAdvertisedEndpoints);
}

TEST(ClusterHelloBounds, HelloOverTheEndpointCapIsRefused) {
    EXPECT_TRUE(
        parse_hello(hello(kValidId, endpoint_list(kMaxAdvertisedEndpoints + 1))).node_id.empty());
    EXPECT_TRUE(parse_hello(hello(kValidId, endpoint_list(10000))).node_id.empty());
}

TEST(ClusterHelloBounds, HelloWithUnsafeTextIsRefused) {
    // A control character in the node id or any endpoint refuses the whole HELLO.
    EXPECT_TRUE(parse_hello(hello("evil\x1b[31m", "10.0.0.1:7000")).node_id.empty());
    EXPECT_TRUE(parse_hello(hello(kValidId, "10.0.0.1:7000,evil\x1b[2J:80")).node_id.empty());
    EXPECT_TRUE(parse_hello(hello(kValidId, "not-an-endpoint")).node_id.empty());
    EXPECT_TRUE(parse_hello(hello(kValidId, "10.0.0.1:99999")).node_id.empty());
    // A wan peer must advertise somewhere to reach it.
    EXPECT_TRUE(parse_hello(hello(kValidId, "-", "wan")).node_id.empty());
    // Wrong protocol or verb.
    EXPECT_TRUE(parse_hello("ZETTA/2 HELLO " + kValidId + " 10.0.0.1:7000").node_id.empty());
    EXPECT_TRUE(parse_hello("ZETTA/1 HI " + kValidId + " 10.0.0.1:7000").node_id.empty());
}

// A node never advertises more endpoints than its peers accept, and keeps every
// --advertise extra when it trims.
TEST(ClusterHelloBounds, AdvertisedSetIsCappedAndKeepsExtras) {
    ClusterConfig config = listening_on(43771);
    for (std::size_t index = 0; index < kMaxAdvertisedEndpoints + 4; ++index) {
        config.advertise.push_back("192.0.2." + std::to_string(index + 1) + ":43771");
    }
    // Over the cap: the listen address gives way first, then the last extras.
    Cluster crowded(zetta::util::new_uuid(), config);
    EXPECT_EQ(crowded.advertised_endpoints().size(), kMaxAdvertisedEndpoints);

    ClusterConfig few = listening_on(43771);
    few.advertise = {"192.0.2.7:43771", "lb.example.com:443"};
    Cluster node(zetta::util::new_uuid(), few);
    const std::vector<std::string> endpoints = node.advertised_endpoints();
    ASSERT_EQ(endpoints.size(), 3u);
    EXPECT_EQ(endpoints.front(), "127.0.0.1:43771");
    EXPECT_EQ(endpoints.back(), "lb.example.com:443");
}

// Over the wire: a HELLO carrying an escape sequence, or one over the endpoint cap,
// is dropped before anything is recorded; a well-formed HELLO on the same node is
// recorded (so the refusals are not an artefact of the raw client).
TEST(ClusterHelloBounds, LiveNodeRecordsOnlyWellFormedHellos) {
    Cluster node(zetta::util::new_uuid(), listening_on(43772));
    node.start();
    ASSERT_TRUE(wait_for([&] { return node.is_listening(); }));

    {
        RawPeer hostile(43772);
        ASSERT_TRUE(hostile.connected());
        ASSERT_TRUE(hostile.send_text(
            hello(zetta::util::new_uuid(), "10.0.0.1:7000,evil\x1b[2J:80") + "\n"));
        // While the hostile connection is still open, nothing is recorded (an
        // accepted HELLO shows up within milliseconds, as the control below shows).
        EXPECT_FALSE(wait_for([&] { return node.peer_count() > 0; }, 500));
        EXPECT_TRUE(hostile.closed_by_node());
    }
    {
        RawPeer hostile(43772);
        ASSERT_TRUE(hostile.connected());
        ASSERT_TRUE(hostile.send_text(
            hello(zetta::util::new_uuid(), endpoint_list(kMaxAdvertisedEndpoints + 1)) + "\n"));
        // While the hostile connection is still open, nothing is recorded (an
        // accepted HELLO shows up within milliseconds, as the control below shows).
        EXPECT_FALSE(wait_for([&] { return node.peer_count() > 0; }, 500));
        EXPECT_TRUE(hostile.closed_by_node());
    }
    EXPECT_EQ(node.peer_count(), 0u);

    const std::string honest_id = zetta::util::new_uuid();
    RawPeer honest(43772);
    ASSERT_TRUE(honest.connected());
    ASSERT_TRUE(honest.send_text(hello(honest_id, "127.0.0.1:1") + "\n"));
    ASSERT_TRUE(wait_for([&] { return node.peer_count() == 1; }));
    EXPECT_EQ(node.peers().front().node_id, honest_id);

    node.stop();
}

// A peer cannot make a node buffer megabytes before its HELLO is parsed: a frame
// over kMaxHelloBytes with no newline is cut off. Before #771 the node kept reading
// up to the 64 MiB steady-state limit and held the connection open.
TEST(ClusterHelloBounds, OversizedHelloFrameIsCutOff) {
    Cluster node(zetta::util::new_uuid(), listening_on(43773));
    node.start();
    ASSERT_TRUE(wait_for([&] { return node.is_listening(); }));

    RawPeer hostile(43773);
    ASSERT_TRUE(hostile.connected());
    // Ignore a send failure: the node may reset the connection mid-write.
    hostile.send_text("ZETTA/1 HELLO " + std::string(kMaxHelloBytes * 4, 'a'));
    EXPECT_TRUE(hostile.closed_by_node());
    EXPECT_EQ(node.peer_count(), 0u);

    node.stop();
}
