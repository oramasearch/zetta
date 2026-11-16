#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <map>

#include "zetta/cluster/frame_faults.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/governance/audit_event.hpp"

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
    // The endpoint THIS node reached the peer on — the reachable path from this
    // vantage point (a peer behind NAT/LB advertises several candidates; the one
    // that connected differs per observer). Chosen from `endpoints` at dial time.
    std::string address;
    // The full candidate-endpoint SET the peer advertised in its HELLO (pod IP,
    // LAN IP, LB/public address, …). One node can be reachable at several; `address`
    // is the one that worked here. Empty for a `dial-out-only` peer, which
    // advertises no dialable address (see below). See #739 (routing table).
    std::vector<std::string> endpoints;
    // Reachability (#803): a `dial-out-only` peer (behind NAT / a private VNet) has
    // NO routable address — it advertises an empty candidate set and is reached
    // ONLY over the inbound link it opened. Peers must never try to dial it (its
    // private/bound IP is unroutable and, worse, collides across clusters sharing a
    // pod CIDR). It is a pure initiator; its one outbound link carries CRDT sync
    // both ways. False = wan-accessible (a routable rendezvous others dial out to).
    bool dial_out_only = false;
    unsigned audit_protocol_version = 0;
};

// --- HELLO bounds (#771) -------------------------------------------------------
//
// A peer's HELLO is untrusted input: its node id and advertised endpoints are
// stored in the peer table, shown in the node list and logs, and dialed by the
// reconnect loop. These bounds keep one connection from holding more than a few
// kilobytes of peer-chosen text, and keep that text to the shapes a node emits.

// The most endpoints a HELLO may advertise. A HELLO listing more is refused, and a
// node never advertises more than this (Cluster::advertised_endpoints trims).
inline constexpr std::size_t kMaxAdvertisedEndpoints = 16;
// The longest node id accepted (node ids are UUIDs, 36 characters).
inline constexpr std::size_t kMaxNodeIdBytes = 128;
// The longest advertised endpoint: a 253-byte DNS name, a colon, a 5-digit port,
// plus room for IPv6 brackets.
inline constexpr std::size_t kMaxEndpointBytes = 261;
// The largest HELLO frame read off a new connection, before the steady-state
// frame limit applies. Comfortably above the longest valid HELLO
// (16 endpoints of 261 bytes, a 128-byte id and the fixed tokens: under 4.5 KiB).
inline constexpr std::size_t kMaxHelloBytes = 16 * 1024;

// True when `node_id` is 1..kMaxNodeIdBytes of [A-Za-z0-9._-]: no spaces, control
// characters or terminal escapes can reach the node list or stderr through it.
bool is_valid_node_id(std::string_view node_id);

// True when `endpoint` is HOST:PORT, at most kMaxEndpointBytes long, with a port
// of 1..65535 (digits only) and a non-empty host of [A-Za-z0-9._-] plus the IPv6
// literal characters `:`, `[`, `]` and `%`: an IPv4 address, a DNS name or an IPv6
// literal. Anything else (spaces, control characters, quotes, escapes) fails.
bool is_valid_peer_endpoint(std::string_view endpoint);

// Parse "ZETTA/1 HELLO <node_id> <ep1[,ep2,...]|-> [wan|dial-out] [audit-version]".
// Returns a peer with an EMPTY node_id when the line is malformed: the wrong
// protocol or verb, an invalid node id, more than kMaxAdvertisedEndpoints
// endpoints, any endpoint failing is_valid_peer_endpoint, or a wan HELLO with no
// endpoint. The caller drops such a connection before recording anything.
ClusterPeer parse_hello(std::string_view line);

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
    unsigned audit_protocol_version = 0;
};

// One row of the FULL cluster membership from the replicated node-list (#800):
// every node the CRDT document knows, whether or not THIS node holds a direct link
// to it. Distinct from ClusterPeer/NodeStatus, which are the DIRECT-peer views — on
// a spanning cluster (many k8s clusters + hosts) no single node is directly linked
// to all N, so `system.nodes` shows only a slice; but the node-list CRDT converges
// everywhere, so membership() lists every member from any node. Built by
// Cluster::membership().
struct ClusterMember {
    std::string node_id;
    std::string address;         // its dialable address; empty for dial-out-only
    bool dial_out_only = false;  // reachability recorded in the node-list entry
    std::string node_type;       // "permanent" | "ephemeral" (from the node-list entry)
    bool is_self = false;
    bool directly_connected = false;  // a live link is held to it right now
};

// Does the node id a peer states in its HELLO match the certificate it
// presented in the mutual-TLS handshake (#669)? True only when the certificate
// parses, carries exactly one subject common name, and that name equals
// `stated_node_id`. The handshake has already verified the certificate against
// the cluster CA; this binds the protocol identity to it.
bool hello_matches_peer_certificate(std::string_view stated_node_id,
                                    std::span<const unsigned char> peer_certificate_der);

struct ClusterConfig {
    // Zero models a legacy peer in protocol tests. The binary always uses 1.
    unsigned audit_protocol_version = zetta::governance::kAuditProtocolVersion;
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
    // Other nodes to dial. All peers are equal — a seed is just somewhere to
    // start; the handshake is symmetric. Each entry is one peer, given as its
    // candidate endpoint(s): a single `host:port`, or a comma-separated set
    // `host:port,host:port,…` when one peer is reachable at several addresses —
    // the dialer tries them in order and keeps the first that connects (the
    // reachable path from this vantage point). See #739 (routing table).
    std::vector<std::string> seeds;
    // Extra reachable endpoints to advertise beyond the bound listen address(es).
    // A node behind NAT / a load balancer lists its public / LB address here
    // (`--advertise HOST:PORT[,...]`), so peers learn the endpoint they can
    // actually dial rather than only the (unreachable) bound pod IP — the root
    // cause of the membership flap this addresses. Folded into the advertised
    // HELLO candidate set alongside listen_addresses(). See #739.
    std::vector<std::string> advertise;
    // Reachability (#803): a `dial-out-only` node (behind NAT / a private VNet — a
    // home node, an AKS pod on a private IP) has no routable address. It advertises
    // an EMPTY dialable candidate set (its bound/private IP is unroutable and, when
    // two clusters share a pod CIDR, collides — the "7 not 9" membership undercount)
    // and reaches peers only OUTBOUND; that one link carries CRDT sync both ways, so
    // a peer never needs to dial back. Set with `--dial-out-only`; the operator sets
    // it for any slice whose reachability isn't wan-accessible. Default false =
    // wan-accessible (advertise a routable address others dial out to).
    bool dial_out_only = false;
    // Node type (#809): "permanent" (default — reliable uptime, durability-bearing:
    // counts toward the replication factor) or "ephemeral" (burst / autoscaled
    // capacity that may vanish at any moment; holds only uncounted *locality*
    // replicas and never moves the replication factor). Self-declared via
    // `--node-type`, written into this node's `node:<id>` entry, surfaced on
    // system.nodes. Orthogonal to reachability. Any string is stored; the engine
    // treats anything other than "ephemeral" as permanent.
    std::string node_type = "permanent";
    // Drop a peer that has sent nothing for this long (gossip flows every ~0.5s,
    // so silence means the link died — clears it so a fresh one can replace it).
    unsigned peer_idle_timeout_seconds = 6;
    // Re-dial seeds that aren't currently connected, this often, so a dropped
    // link re-establishes itself.
    unsigned reconnect_interval_seconds = 3;
    // Membership hysteresis (#739): once a peer's live link drops, keep the node
    // in the membership view (nodes() / system.nodes / the UI) for this long
    // before dropping it, so a brief blip or reconnect doesn't flap it out. Live
    // links reap immediately regardless — peers()/peer_count() and the signal
    // plane reflect real connectivity with no delay; only the membership view is
    // debounced. 0 disables (drop as soon as the link goes).
    unsigned membership_grace_seconds = 10;
    // Log every cluster event (handshake, peer add/remove, STATUS sent/received,
    // body requests/transfers, keepalive, reconnect) to stderr — for debugging
    // gossip/convergence issues. Off by default; enable with `--log-cluster`.
    bool log_events = false;
    // Inter-node transport security (CTL-2, #466): when enabled(), every
    // connection — accepted or dialed — runs mutual TLS 1.3 against the
    // token-derived cluster CA before any protocol line is exchanged, so a
    // node without the join token cannot join. The zetta binary ALWAYS
    // populates this (#799 removed the plaintext opt-out), so the all-empty
    // "plaintext" default is reached only by embedding a bare Cluster in a
    // test/host. See docs/cluster/security.md.
    ClusterSecurity security;
    // One deadline across TLS and both HELLO directions, accepted or dialed
    // (#1401). Progress does not extend it; nonpositive values clamp to 1 ms.
    std::chrono::milliseconds peer_setup_timeout{5000};
    // Concurrent TLS/HELLO setups on accepted connections. Excess accepts are
    // closed before TLS allocation; dials are not counted; zero clamps to one.
    std::size_t maximum_pending_setups = 64;
    // Chaos experiments only (#842): when set, every inbound frame after the
    // handshake passes through this injector, which may drop, delay, duplicate or
    // reorder it (see frame_faults.hpp). Never set by the zetta binary. Must
    // outlive the Cluster's network thread — the shared_ptr keeps it alive.
    std::shared_ptr<FrameFaultInjector> frame_faults;
    // The public key of the cluster's identity root (#764,
    // docs/governance/identity-seal.md), 64 hex. Not a secret: peer_command()
    // carries it as --identity-root so the next node pins this cluster's root.
    // Empty (a test/host Cluster) leaves it out.
    std::string identity_root;
};

// Replication gossip this node has sent since start() (#794), summed over every
// link. `sync_bytes` is the SYNC lines as written to the link (base64 payload plus
// the verb), before any TLS framing. A round is one gossip tick on one link, so a
// node with two peers counts two rounds per tick.
// A revoked node key, as the Cluster enforces it (#1320).
struct NodeRevocation {
    std::string node_id;
    bool allow_rekey = false;  // the id may enroll a new key
};

struct GossipStats {
    std::uint64_t rounds = 0;             // gossip ticks, per link
    std::uint64_t sync_frames = 0;        // SYNC lines sent
    std::uint64_t full_syncs = 0;         // ... carrying the whole document
    std::uint64_t incremental_syncs = 0;  // ... carrying only the changes a peer lacked
    std::uint64_t sync_bytes = 0;         // bytes of those SYNC lines
};

// Bounded cross-thread handoff. The network adapter queues wire messages; the
// Node owner loop reads/writes segment files and validates their event frames.
struct AuditMessage {
    enum class Kind { Status, Request, Response };
    Kind kind = Kind::Status;
    std::string peer;
    std::map<std::string, zetta::governance::AuditWatermark> watermarks;
    zetta::governance::AuditLogBatch batch;
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
    // best reachable address — a non-loopback one when available). Safe to log:
    // with security on, the join token is replaced by kJoinTokenPlaceholder, never
    // printed (#1393). The token is the cluster root secret: it derives the CA key
    // and seals saved connector credentials, so it must not reach stdout, pod logs
    // or a log pipeline.
    std::string peer_command() const;

    // The same command with the clear join token, for the one place an operator
    // explicitly asks for it (the headed app's "Copy command" button). Never log
    // or print this value.
    std::string peer_command_with_join_token() const;

    // Snapshot of peers with a live link right now (thread-safe). A node kept in
    // the membership view purely by hysteresis (its link has dropped but is within
    // the grace window) is NOT listed here — this is the live-connectivity view.
    std::vector<ClusterPeer> peers() const;
    std::size_t peer_count() const;

    // The endpoints this node advertises to peers in its HELLO — its reachable
    // listen address(es) plus any ClusterConfig::advertise extras (deduped). The
    // candidate set a peer stores and dials by. Non-loopback listen addresses come
    // first; front() is what peer_command()/nodes() report as this node's address.
    // EMPTY for a dial-out-only node (#803) — it advertises no dialable address.
    std::vector<std::string> advertised_endpoints() const;

    // Reachability of THIS node (#803): true when it is dial-out-only (advertises
    // no dialable address; a pure initiator). Mirrors ClusterConfig::dial_out_only.
    bool dial_out_only() const;

    // This node's type (#809): "permanent" (default) or "ephemeral". Mirrors
    // ClusterConfig::node_type; written into this node's node:<id> entry.
    std::string node_type() const;

    // The single most-dialable address to record in the replicated node-list
    // (`node:<id>`) for transitive learners (#803/#800): empty when dial-out-only;
    // else the --advertise front (a routable LB/public address) when set, else the
    // bound listen address. Pair with dial_out_only() for the entry's reachability.
    std::string primary_advertised_address() const;

    // Number of live peer connections held. At most one per peer node id: a value
    // above peer_count() would mean duplicate links are leaking (a reconnect race, a
    // seed whose address never matches the peer's advertised one, or a mutual dial).
    // Kept at parity by the per-node-id link registry — see #739.
    std::size_t connection_count() const;

    // Replication gossip sent so far (#794) — the bytes a round costs, and how
    // many rounds shipped the whole document versus only a peer's missing changes.
    GossipStats gossip_stats() const;

    void publish_audit_watermarks(
        const std::map<std::string, zetta::governance::AuditWatermark>& watermarks);
    std::vector<AuditMessage> take_audit_messages();
    bool send_audit(const AuditMessage& message);

    // Every node and its replication status, for the "list of all nodes" view:
    // this node first (is_self), then each member with the replication state it
    // last reported. This is the membership view — it applies hysteresis (#739): a
    // peer whose link just dropped stays listed (with connected=false) through the
    // grace window, so a brief blip doesn't flap it out; it drops only after a
    // sustained absence. Thread-safe. With no ReplicationManager wired in, the
    // replication fields are left unknown.
    std::vector<NodeStatus> nodes() const;

    // The FULL cluster membership from the replicated node-list (#800): every
    // node:<id> the CRDT document knows — including nodes THIS node has no direct
    // link to — annotated with each node's reachability and whether we hold a live
    // link to it right now. On a spanning cluster nodes() shows only a slice, but
    // the node-list CRDT converges everywhere, so this lists all N from any node.
    // Thread-safe; empty with no ReplicationManager wired in.
    std::vector<ClusterMember> membership() const;

    // --- Signal plane (RPC / events / tasks) --------------------------------
    //
    // A typed, ephemeral message layer over the same peer connections (NOT
    // routed through the replicated document). See docs/cluster/signal-plane.md.

    // Register the handler invoked for incoming requests / events / tasks. It
    // runs on the cluster's network thread, so it must be cheap and thread-safe
    // (offload heavy work). Pass nullptr to clear. Thread-safe; may be called
    // before start() or at any time after. The handler must outlive the Cluster
    // (or be cleared first). Called off the network thread, it returns only after
    // any dispatch already inside the previous handler has finished, so the
    // previous handler may be destroyed once this returns (#1131).
    void set_signal_handler(SignalHandler* handler);

    // The currently registered signal handler (nullptr when none) — what a
    // component chains itself over when it registers as the new handler with a
    // delegate (the DistributedSorter / DistributedFulltextIndex pattern).
    // Thread-safe.
    SignalHandler* signal_handler() const;

    // --- Per-node revocation (#1320, docs/cluster/security.md) ---------------
    //
    // The keys revoked by the sealed node_revocation records the Node trusts,
    // by key hash (the SHA-256 of the certificate's public key). Replaces the
    // set. Every later handshake presenting one is refused, ENROLL refuses it,
    // a live link presenting one is closed, and a revocation that allows
    // re-keying releases the id's pin. Any thread.
    void set_revoked_keys(std::map<std::string, NodeRevocation> revoked);

    // Send an RPC request to a peer (by node id) and block the calling thread
    // until the peer's response arrives, the timeout elapses, or the peer is
    // found unreachable. Returns ok=false with a reason on an unknown / dropped
    // peer, a timeout, or a remote handler error. Thread-safe — but must NOT be
    // called from the network thread (the handler thread), which would deadlock.
    SignalResponse request(
        const std::string& node_id, const std::string& method,
        const std::string& payload,
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Send the same RPC request to several peers at once and block until every
    // one has answered, failed or the shared `timeout` has elapsed. Responses are
    // in `node_ids` order. One round trip instead of one per peer: the requests
    // leave together on each link's next outbox flush (#1231, the record store's
    // synchronous replication). Same threading rule as request().
    std::vector<SignalResponse> request_each(
        const std::vector<std::string>& node_ids, const std::string& method,
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
