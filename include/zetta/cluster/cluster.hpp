#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zetta/cluster/security.hpp"
#include "zetta/cluster/signal.hpp"

namespace zetta::data { class BlobStore; }

namespace zetta::cluster {

class ReplicationManager;

// Leadership applies only to exclusive resources (locks, sequences, …), and is
// per-(node, resource): a node holds zero to many roles at once. This single
// value is a placeholder for that future per-resource set — today always
// Follower. See docs/cluster/replication.md → Consistency model.
enum class ClusterRole { Follower, Candidate, Leader };

// Human-readable role name ("follower" / "candidate" / "leader").
std::string to_string(ClusterRole role);

// A peer this node is currently connected to.
struct ClusterPeer {
    std::string node_id;
    std::string address;  // host:port the peer reported it listens on
};

// A node's replication state, as last learned over the gossip exchange. For the
// local node it's read straight from its ReplicationManager; for a peer it's
// what that peer last reported.
struct NodeReplication {
    bool known = false;          // have we learned this node's replication state?
    std::size_t entries = 0;     // top-level keys in its replicated document
    std::string version;         // heads fingerprint (matches ours when in sync)
    bool in_sync = false;        // version equals this node's own version
    double seconds_since_sync = -1.0;  // <0 = not applicable (self) or never
};

// One row of the "all nodes + replication status" view: who the node is, how it
// connects, and where its replicated state stands. Built by Cluster::nodes().
struct NodeStatus {
    std::string node_id;
    std::string address;
    ClusterRole role = ClusterRole::Follower;
    bool is_self = false;
    bool connected = false;  // membership (always true for self)
    NodeReplication replication;
};

struct ClusterConfig {
    // Bind all interfaces by default (the wildcard 0.0.0.0) so a plain node is
    // reachable from other machines out of the box; listen_addresses() expands
    // it to the host's real LAN IPs. Set a specific host (e.g. "127.0.0.1") to
    // restrict it to loopback.
    std::string listen_host = "0.0.0.0";
    std::uint16_t listen_port = 7777;
    // Ports to try from `listen_port`: `start()` binds the first free port in
    // [listen_port, listen_port + port_range). Default 1 = strict — bind
    // *exactly* `listen_port` or nothing (start() leaves is_listening() false;
    // the zetta binary then exits non-zero rather than drift). A node's peer
    // address is load-bearing — siblings dial it — so a fixed --listen must
    // never silently move (#738). Widen the range (`--port-range N`) only to
    // pack several nodes on one host (e.g. one per NUMA node); each then grabs
    // the next free port, and the *actually-bound* address is what's advertised.
    std::uint16_t port_range = 1;
    // Other nodes (host:port) to dial. All peers are equal — a seed is just
    // somewhere to start; the handshake is symmetric.
    std::vector<std::string> seeds;
    // Drop a peer that has sent nothing for this long (gossip flows every ~0.5s,
    // so silence means the link died — clears it so a fresh one can replace it).
    unsigned peer_idle_timeout_seconds = 6;
    // Re-dial seeds that aren't currently connected, this often, so a dropped
    // link re-establishes itself.
    unsigned reconnect_interval_seconds = 3;
    // Log every cluster event (handshake, peer add/remove, STATUS sent/received,
    // body requests/transfers, keepalive, reconnect) to stderr — for debugging
    // gossip/convergence issues. Off by default; enable with `--log-cluster`.
    bool log_events = false;
    // Inter-node transport security (CTL-2, #466): when enabled(), every
    // connection — accepted or dialed — runs mutual TLS 1.3 against the
    // token-derived cluster CA before any protocol line is exchanged, so a
    // node without the join token cannot join. Default (all-empty) is
    // plaintext; the zetta binary always populates this (see main.cpp), so
    // plaintext is reached only by explicit --cluster-plaintext or by
    // embedding a bare Cluster. See docs/cluster/security.md.
    ClusterSecurity security;
};

// Cluster membership over TCP — mutual TLS 1.3 when ClusterConfig::security
// is enabled (CTL-2, #466; see docs/cluster/security.md), plaintext otherwise.
// Every node is an equal peer: it listens for connections and dials its
// configured `seeds`, exchanging a symmetric handshake so both sides record
// each other. Leadership is NOT configured —
// leader election and failover are planned; until then `role()` is Follower.
// Networking runs on a background thread; the public surface exposes no Corosio
// types (the adapter is src/core/cluster.cpp).
class Cluster {
public:
    struct State;
    // `replication`, when given, is gossiped to peers: on each connection the
    // nodes exchange their replicated documents (save/merge) and report their
    // replication status, which surfaces in nodes(). `blobs`, when given, lets
    // peers transfer large-object **bodies** (metadata travels in `replication`;
    // bodies don't) — a node fetches the bodies it's missing and serves the ones
    // it has, verifying each against its hash. Both must outlive the Cluster.
    // Passing nullptr for either disables that part (membership-only at minimum).
    Cluster(std::string node_id, ClusterConfig config,
            ReplicationManager* replication = nullptr,
            zetta::data::BlobStore* blobs = nullptr);
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

    // Whether start() actually bound a listen socket. False means the port(s)
    // in [listen_port, listen_port + port_range) were all in use — with the
    // default strict range that's the exact --listen port. The zetta binary
    // treats a false here as fatal (exits non-zero) rather than run a node that
    // never listens or drifts off its advertised address (#738).
    bool is_listening() const;

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

    // Number of live peer connections held. At most one per peer node id: a value
    // above peer_count() would mean duplicate links are leaking (a reconnect race, a
    // seed whose address never matches the peer's advertised one, or a mutual dial).
    // Kept at parity by the per-node-id link registry — see #739.
    std::size_t connection_count() const;

    // Every node and its replication status, for the "list of all nodes" view:
    // this node first (is_self), then each connected peer with the replication
    // state it last reported. Thread-safe. With no ReplicationManager wired in,
    // the replication fields are left unknown.
    std::vector<NodeStatus> nodes() const;

    // --- Signal plane (RPC / events / tasks) --------------------------------
    //
    // A typed, ephemeral message layer over the same peer connections (NOT
    // routed through the replicated document). See docs/cluster/signal-plane.md.

    // Register the handler invoked for incoming requests / events / tasks. It
    // runs on the cluster's network thread, so it must be cheap and thread-safe
    // (offload heavy work). Pass nullptr to clear. Thread-safe; may be called
    // before start() or at any time after. The handler must outlive the Cluster
    // (or be cleared first).
    void set_signal_handler(SignalHandler* handler);

    // The currently registered signal handler (nullptr when none) — what a
    // component chains itself over when it registers as the new handler with a
    // delegate (the DistributedSorter / DistributedFulltextIndex pattern).
    // Thread-safe.
    SignalHandler* signal_handler() const;

    // Send an RPC request to a peer (by node id) and block the calling thread
    // until the peer's response arrives, the timeout elapses, or the peer is
    // found unreachable. Returns ok=false with a reason on an unknown / dropped
    // peer, a timeout, or a remote handler error. Thread-safe — but must NOT be
    // called from the network thread (the handler thread), which would deadlock.
    SignalResponse request(
        const std::string& node_id, const std::string& method,
        const std::string& payload,
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Fire-and-forget an event to one peer (by node id). Returns false if not
    // currently connected to that peer (best-effort: delivery is not
    // guaranteed). Thread-safe.
    bool send_event(const std::string& node_id, const std::string& method,
                    const std::string& payload);

    // Fire-and-forget an event to every connected peer. Returns the number of
    // peers it was queued to (best-effort). Thread-safe.
    std::size_t broadcast_event(const std::string& method,
                                const std::string& payload);

    // Fire-and-forget a task envelope to a peer (by node id) — like send_event
    // but tagged Task, the substrate for job dispatch. Returns false if not
    // connected. Thread-safe.
    bool send_task(const std::string& node_id, const std::string& method,
                   const std::string& payload);

    // Opaque networking state, defined in cluster.cpp — hides the Corosio
    // backend, the io_context thread, and the peer table. Public so the
    // file-local networking coroutines can name it; still incomplete here.

private:
    std::unique_ptr<State> state_;
    std::string node_id_;
    ClusterConfig config_;
    ClusterRole role_;
};

}  // namespace zetta::cluster