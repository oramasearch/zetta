#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "zetta/cluster.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;

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
