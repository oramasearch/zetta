#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zetta {

// Elected at runtime. Leader election + failover are planned; until then every
// node stays a Follower and all nodes are treated as equals.
enum class ClusterRole { Follower, Candidate, Leader };

// A peer this node is currently connected to.
struct ClusterPeer {
    std::string node_id;
    std::string address;  // host:port the peer reported it listens on
};

struct ClusterConfig {
    // Bind all interfaces by default (the wildcard 0.0.0.0) so a plain node is
    // reachable from other machines out of the box; listen_addresses() expands
    // it to the host's real LAN IPs. Set a specific host (e.g. "127.0.0.1") to
    // restrict it to loopback. Nodes default to the same port; the range lets
    // several share one host (e.g. one per NUMA node) — each binds the first
    // free port in [listen_port, listen_port + port_range).
    std::string listen_host = "0.0.0.0";
    std::uint16_t listen_port = 7777;
    std::uint16_t port_range = 16;
    // Other nodes (host:port) to dial. All peers are equal — a seed is just
    // somewhere to start; the handshake is symmetric.
    std::vector<std::string> seeds;
};

// Cluster membership over TCP. Every node is an equal peer: it listens for
// connections and dials its configured `seeds`, exchanging a symmetric
// handshake so both sides record each other. Leadership is NOT configured —
// leader election and failover are planned; until then `role()` is Follower.
// Networking runs on a background thread; the public surface exposes no Corosio
// types (the adapter is src/core/cluster.cpp).
class Cluster {
public:
    Cluster(std::string node_id, ClusterConfig config);
    ~Cluster();

    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;

    // Bind a listener (first free port in the range) and dial the seeds.
    void start();
    // Stop the network thread and disconnect. Safe to call when not started.
    void stop();

    ClusterRole role() const { return role_; }
    const std::string& node_id() const { return node_id_; }

    // The host:port this node actually bound (the configured base until start()
    // picks a free port in the range).
    std::string listen_address() const;

    // The address(es) other nodes can reach this one at. When bound to a
    // specific host, that host:port. When bound to a wildcard (0.0.0.0), the
    // machine's actual interface IPv4 addresses (non-loopback first) — the
    // wildcard itself isn't connectable.
    std::vector<std::string> listen_addresses() const;

    // Command to run on another machine to add it to this cluster (uses the
    // best reachable address — a non-loopback one when available).
    std::string peer_command() const;

    // Snapshot of currently-connected peers (thread-safe).
    std::vector<ClusterPeer> peers() const;
    std::size_t peer_count() const;

    // Opaque networking state, defined in cluster.cpp — hides the Corosio
    // backend, the io_context thread, and the peer table. Public so the
    // file-local networking coroutines can name it; still incomplete here.
    struct State;

private:
    std::unique_ptr<State> state_;
    std::string node_id_;
    ClusterConfig config_;
    ClusterRole role_;
};

}  // namespace zetta
