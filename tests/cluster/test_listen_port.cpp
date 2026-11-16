// SPDX-License-Identifier: MIT

// Regression tests for #738: a fixed --listen port must be honored exactly —
// bound as requested (even immediately after a restart, while the previous
// process's sockets linger in TIME_WAIT) or not bound at all, never silently
// drifted to the next port. See src/core/cluster/cluster.cpp (SO_REUSEADDR +
// strict single-port bind) and docs/cluster/cluster.md.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "zetta/cluster/cluster.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

ClusterConfig loopback_config(std::uint16_t port) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;  // strict: exactly this port, never drift
    return config;
}

}  // namespace

// The default is strict single-port binding — auto-bump is opt-in via a range.
TEST(ClusterListenPort, StrictSingleBindIsTheDefault) {
    EXPECT_EQ(ClusterConfig{}.port_range, 1u);
}

// A node stopped and immediately restarted on the SAME fixed port rebinds that
// exact port — it does not drift to the next one (#738). SO_REUSEADDR lets the
// fresh bind succeed even if the OS is still holding the address.
TEST(ClusterListenPort, RebindsExactPortAfterRestart) {
    const std::uint16_t port = 42500;
    const std::string address = "127.0.0.1:42500";

    auto first = std::make_unique<Cluster>(zetta::util::new_uuid(),
                                           loopback_config(port));
    first->start();
    ASSERT_TRUE(first->is_listening());
    EXPECT_EQ(first->listen_address(), address);

    // Stop and drop it, then rebind straight away on the same port.
    first->stop();
    first.reset();

    Cluster second(zetta::util::new_uuid(), loopback_config(port));
    second.start();
    ASSERT_TRUE(second.is_listening());
    EXPECT_EQ(second.listen_address(), address);  // exact, not 42501

    second.stop();
}

// The faithful repro: a node with a live peer connection is killed and a fresh
// node is restarted on the same port. The dead process's accepted socket
// lingers in TIME_WAIT on that port; without SO_REUSEADDR the rebind is refused
// and the node used to drift. With the fix it rebinds the exact port.
TEST(ClusterListenPort, RebindsExactPortAfterRestartWithLiveConnection) {
    const std::uint16_t server_port = 42510;
    const std::string server_address = "127.0.0.1:42510";

    auto server = std::make_unique<Cluster>(zetta::util::new_uuid(),
                                             loopback_config(server_port));
    server->start();
    ASSERT_TRUE(server->is_listening());
    ASSERT_EQ(server->listen_address(), server_address);

    // A peer dials in, so there is a live established connection whose socket
    // will linger in TIME_WAIT on server_port once the server dies.
    ClusterConfig peer_config = loopback_config(42511);
    peer_config.seeds = {server_address};
    auto peer = std::make_unique<Cluster>(zetta::util::new_uuid(), peer_config);
    peer->start();

    bool connected = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (server->peer_count() == 1 && peer->peer_count() == 1) {
            connected = true;
            break;
        }
        sleep_ms(20);
    }
    ASSERT_TRUE(connected);

    // Kill both sides, leaving server_port in TIME_WAIT, then immediately
    // restart a fresh node on the same port.
    server->stop();
    server.reset();
    peer->stop();
    peer.reset();

    Cluster restarted(zetta::util::new_uuid(), loopback_config(server_port));
    restarted.start();
    ASSERT_TRUE(restarted.is_listening());
    EXPECT_EQ(restarted.listen_address(), server_address);  // exact, not 42511

    restarted.stop();
}

// Strict binding fails loudly instead of drifting: a second node on the same
// port while the first still actively holds it does not bind (SO_REUSEADDR does
// not permit two live listeners on one port) — main() turns this into a
// non-zero exit so a supervisor restarts cleanly, rather than silently moving
// to the next port and falling off the mesh (#738).
TEST(ClusterListenPort, StrictBindRefusesToDriftWhenPortBusy) {
    const std::uint16_t port = 42520;

    Cluster first(zetta::util::new_uuid(), loopback_config(port));
    first.start();
    ASSERT_TRUE(first.is_listening());
    EXPECT_EQ(first.listen_address(), "127.0.0.1:42520");

    Cluster second(zetta::util::new_uuid(), loopback_config(port));
    second.start();
    EXPECT_FALSE(second.is_listening());  // did not drift to 42521

    second.stop();
    first.stop();
}

// The escape hatch is intact: an explicit --port-range still packs several
// nodes onto one host, each grabbing the next free port and advertising the
// address it actually bound.
TEST(ClusterListenPort, ExplicitPortRangeStillPacksNodes) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 42530;
    config.port_range = 4;  // opt into a range

    Cluster first(zetta::util::new_uuid(), config);
    first.start();
    Cluster second(zetta::util::new_uuid(), config);
    second.start();

    ASSERT_TRUE(first.is_listening());
    ASSERT_TRUE(second.is_listening());
    EXPECT_EQ(first.listen_address(), "127.0.0.1:42530");
    EXPECT_EQ(second.listen_address(), "127.0.0.1:42531");  // stepped forward

    second.stop();
    first.stop();
}
