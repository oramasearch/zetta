#include "zetta/cluster/cluster.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy.hpp>

#include "zetta/data/blob_store.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/runtime/net.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/util/uuid.hpp"

#include "../base64.hpp"

// src/core/cluster.cpp is the networking adapter: the one .cpp allowed to use
// the Corosio backend (via zetta::runtime::net). Cluster's public header
// exposes no backend types, so the rest of zetta stays decoupled.
namespace zetta::cluster {

namespace capy = boost::capy;
namespace net = zetta::runtime::net;

// One peer connection: the socket (or its TLS layer), shared between the read pump
// and the per-link write pump. `initiate` records whether we dialed it (true) or
// accepted it (false) — used to pick a single canonical link when both sides
// connect to each other (the min-node-id-initiator wins), so duplicate links reap.
struct GossipLink {
    std::shared_ptr<net::tcp_socket> socket;
    std::unique_ptr<net::openssl_stream> tls;  // engaged when security is on
    net::any_stream stream;                    // the protocol's transport
    bool alive = true;
    bool initiate = false;  // did we dial this link (true) or accept it (false)?
    std::chrono::steady_clock::time_point last_activity{};  // last line received
};

struct Cluster::State {
    // A peer we know, plus the replication status it last reported over gossip.
    // Per-peer routing/reachability lives here (#739): the candidate endpoints the
    // peer advertised, the one we actually reached it on, and whether a live link
    // is held right now (vs. kept purely by membership hysteresis).
    struct PeerRecord {
        std::string node_id;
        // The endpoint we reached this peer on (its chosen/last-good path from our
        // vantage point) — one of `endpoints`.
        std::string address;
        // The peer's full advertised candidate-endpoint set (from its HELLO).
        std::vector<std::string> endpoints;
        // Is a live link to this peer held right now? A record with connected=false
        // is retained only by membership hysteresis until its grace window lapses.
        bool connected = true;
        // When a live link was last held (stamped on connect and on disconnect) —
        // the clock the hysteresis grace window is measured from.
        std::chrono::steady_clock::time_point last_seen{};
        bool has_status = false;  // have we received a STATUS from it yet?
        std::size_t entries = 0;
        std::string version;
        std::chrono::steady_clock::time_point last_sync{};
    };

    // An RPC awaiting its response: the promise the caller blocks on, plus the
    // peer it was sent to (so a peer disconnect can fail it immediately).
    struct PendingRequest {
        std::shared_ptr<std::promise<SignalResponse>> promise;
        std::string node_id;
    };

    net::io_context io_context;
    std::optional<net::tcp_acceptor> acceptor;
    std::thread network_thread;
    std::uint16_t bound_port = 0;
    // Mutual-TLS context (CTL-2, #466), engaged when ClusterConfig::security is
    // enabled: this node's token-derived identity + the cluster CA as the only
    // trust anchor, require_peer both ways, TLS 1.3 minimum. Shared by every
    // link (client and server handshakes use the same context); configured
    // before start() spawns any coroutine, never modified after.
    std::optional<net::tls_context> tls;
    ReplicationManager* replication = nullptr;  // gossip target (optional)
    zetta::data::BlobStore* blobs = nullptr;                 // large-object body transfer (optional)
    std::chrono::steady_clock::duration idle_timeout{};
    std::chrono::steady_clock::duration reconnect_interval{};
    // How long a peer stays in the membership view after its live link drops
    // (hysteresis, #739). Zero drops it immediately.
    std::chrono::steady_clock::duration membership_grace{};
    bool log_events = false;
    mutable std::mutex mutex;
    std::vector<PeerRecord> peers;
    // At most one live connection per peer node_id (guarded by `mutex`). A reconnect
    // race, a dialed-vs-advertised address mismatch (a seed that never matches the
    // peer's advertised address, e.g. 127.0.0.1 or a LB VIP), or both sides dialing
    // each other would otherwise pile up duplicate held links; register_link reaps
    // them to one so connections can't leak.
    std::map<std::string, std::shared_ptr<GossipLink>> active_links;
    // Count of connections actually being SERVED (a link that won dedup and spawned
    // its pumps), incremented past register_link and decremented at teardown. Equals
    // peer_count() in steady state; a higher value would mean links are leaking. An
    // atomic so connection_count() can read it without the peer mutex.
    std::atomic<int> live_connections{0};

    // --- Reconnect routing (dedup redials by node id, #739) -----------------
    //
    // The reconnect loop must NOT redial a seed just because the seed STRING isn't
    // among peers' advertised addresses — a seed that never matches (a 127.0.0.1
    // loopback or a load-balancer VIP vs. the peer's advertised LAN/pod IP) would
    // redial every interval forever, churning connect+handshake (each reaped by the
    // per-node-id registry, so no leak, but wasteful). Instead we remember which
    // node id each seed resolved to and skip a seed whose node already holds a live
    // link. `dialing` suppresses a second concurrent dial of the same seed while one
    // is in flight. Both are touched only by the network-thread coroutines
    // (dial/accept/reconnect), but guarded so a stray reader is still safe.
    mutable std::mutex route_mutex;
    std::map<std::string, std::string> seed_node;  // seed string -> resolved node id
    std::set<std::string> dialing;                 // seeds with a dial in flight

    // --- Signal plane -------------------------------------------------------
    //
    // The application's handler for incoming signals. Set on any thread, read on
    // the network thread, so it's atomic.
    std::atomic<SignalHandler*> signal_handler{nullptr};

    // Per-peer outbound queue of wire lines waiting to be sent. Producers run on
    // the caller's thread (request/send_event/broadcast_event) AND the network
    // thread (queued responses + LORES); the per-link writer drains its peer's
    // queue. Keyed by peer node id and guarded by its own mutex — the only
    // cross-thread hand-off, so we never post coroutines across threads (the
    // backend's frame allocator isn't thread-safe).
    std::mutex outbox_mutex;
    std::map<std::string, std::deque<std::string>> outbox;

    // RPCs awaiting a response, keyed by correlation id.
    std::mutex requests_mutex;
    std::map<std::string, PendingRequest> pending_requests;

    // Queue one wire line for a peer (any thread).
    void queue_outbound(const std::string& node_id, std::string line) {
        const std::lock_guard<std::mutex> lock(outbox_mutex);
        outbox[node_id].push_back(std::move(line));
    }
    // Take (and clear) a peer's queued lines — the writer's drain (network thread).
    std::deque<std::string> take_outbound(const std::string& node_id) {
        const std::lock_guard<std::mutex> lock(outbox_mutex);
        const auto it = outbox.find(node_id);
        if (it == outbox.end()) return {};
        return std::exchange(it->second, std::deque<std::string>{});
    }
    // Drop a disconnected peer's queue so it can't accumulate forever.
    void clear_outbound(const std::string& node_id) {
        const std::lock_guard<std::mutex> lock(outbox_mutex);
        outbox.erase(node_id);
    }

    // Register an RPC and hand back its future.
    std::future<SignalResponse> add_pending(const std::string& id,
                                            const std::string& node_id) {
        auto promise = std::make_shared<std::promise<SignalResponse>>();
        std::future<SignalResponse> future = promise->get_future();
        const std::lock_guard<std::mutex> lock(requests_mutex);
        pending_requests.emplace(
            id, PendingRequest{.promise = std::move(promise), .node_id = node_id});
        return future;
    }
    // Claim an RPC's promise (whoever claims it owns the single set_value), or
    // null if it was already claimed (timed out / failed / unknown).
    std::shared_ptr<std::promise<SignalResponse>> take_pending(
        const std::string& id) {
        const std::lock_guard<std::mutex> lock(requests_mutex);
        const auto it = pending_requests.find(id);
        if (it == pending_requests.end()) return nullptr;
        auto promise = it->second.promise;
        pending_requests.erase(it);
        return promise;
    }
    // Fail every RPC awaiting a peer that just disconnected (no need to wait out
    // each timeout). set_value runs outside the lock.
    void fail_pending_for_peer(const std::string& node_id) {
        std::vector<std::shared_ptr<std::promise<SignalResponse>>> orphaned;
        {
            const std::lock_guard<std::mutex> lock(requests_mutex);
            for (auto it = pending_requests.begin();
                 it != pending_requests.end();) {
                if (it->second.node_id == node_id) {
                    orphaned.push_back(std::move(it->second.promise));
                    it = pending_requests.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& promise : orphaned) {
            promise->set_value(
                SignalResponse{.ok = false, .error = "peer disconnected"});
        }
    }

    // Record a live peer. `address` is the endpoint we reached it on (its chosen
    // path from our vantage); `endpoints` is the full advertised candidate set.
    // One entry per node id AND per reached address: a node that restarts reuses
    // its address with a fresh node id, so a new HELLO from a known address evicts
    // the stale entry (and any same-id entry from a prior connection) — no ghost.
    void add_peer(std::string node_id, std::string address,
                  std::vector<std::string> endpoints) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(peers, [&](const PeerRecord& p) {
            return p.node_id == node_id || p.address == address;
        });
        peers.push_back(PeerRecord{.node_id = std::move(node_id),
                                   .address = std::move(address),
                                   .endpoints = std::move(endpoints),
                                   .connected = true,
                                   .last_seen = std::chrono::steady_clock::now()});
    }
    // A live link to this peer dropped. Membership hysteresis (#739): don't erase
    // the record — flag it not-connected and stamp when, so the membership view
    // (nodes()) keeps it through the grace window while a reconnect is attempted.
    // reap_members() removes it once the grace lapses; a reconnect refreshes it.
    void mark_peer_disconnected(const std::string& node_id) {
        const std::lock_guard<std::mutex> lock(mutex);
        for (PeerRecord& peer : peers) {
            if (peer.node_id != node_id) continue;
            peer.connected = false;
            peer.last_seen = std::chrono::steady_clock::now();
            return;
        }
    }
    // Drop peers whose link has been gone longer than the membership grace window
    // (the down side of the hysteresis). Keeps still-connected and still-in-grace
    // peers. Runs off a timer on the network thread.
    void reap_members() {
        const auto now = std::chrono::steady_clock::now();
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(peers, [&](const PeerRecord& p) {
            return !p.connected && (now - p.last_seen) > membership_grace;
        });
    }
    // Register `link` as THE connection for `node_id`, reaping duplicates down to one.
    // Returns true if this link is the keeper (the caller proceeds to serve it); false
    // if it lost the tiebreak (the caller must close it and stop). The keeper is the
    // link INITIATED BY the smaller node_id, so both ends independently choose the same
    // physical connection when they cross-dial; among same-direction duplicates the
    // first registered stays and later redials lose. A win that displaces a stale link
    // hands it back via `superseded` for the caller to close OUTSIDE the lock.
    bool register_link(const std::string& node_id,
                       const std::shared_ptr<GossipLink>& link,
                       const std::string& my_node_id,
                       std::shared_ptr<GossipLink>& superseded) {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = active_links.find(node_id);
        if (it == active_links.end() || !it->second || !it->second->alive) {
            active_links[node_id] = link;
            return true;
        }
        const bool link_is_canonical = (my_node_id < node_id) == link->initiate;
        const bool existing_is_canonical =
            (my_node_id < node_id) == it->second->initiate;
        if (link_is_canonical && !existing_is_canonical) {
            superseded = it->second;  // caller closes it outside the lock
            superseded->alive = false;
            it->second = link;
            return true;
        }
        return false;  // an equal/canonical link already holds this peer
    }
    // Drop `link` as the connection for `node_id` iff it's still the registered one (a
    // superseded link must not evict its replacement). Returns true only if it was the
    // active link — the signal for the caller to tear down the shared peer state.
    bool unregister_link(const std::string& node_id, const GossipLink* link) {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = active_links.find(node_id);
        if (it != active_links.end() && it->second.get() == link) {
            active_links.erase(it);
            return true;
        }
        return false;
    }
    // Is a live registered link to this node id held right now? The reconnect loop's
    // dedup key: a seed whose resolved node is already linked needs no redial (#739).
    bool has_live_link(const std::string& node_id) const {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = active_links.find(node_id);
        return it != active_links.end() && it->second && it->second->alive;
    }
    // Remember which node id a seed resolved to (learned during its handshake), so a
    // later redial of that seed can be skipped while its node holds a live link.
    void note_seed_node(const std::string& seed, const std::string& node_id) {
        if (seed.empty()) return;
        const std::lock_guard<std::mutex> lock(route_mutex);
        seed_node[seed] = node_id;
    }
    std::string seed_resolved_node(const std::string& seed) const {
        const std::lock_guard<std::mutex> lock(route_mutex);
        const auto it = seed_node.find(seed);
        return it == seed_node.end() ? std::string{} : it->second;
    }
    // Claim a seed for dialing; false if a dial for it is already in flight (so we
    // never run two concurrent dials of the same seed).
    bool begin_dial(const std::string& seed) {
        if (seed.empty()) return true;  // an un-keyed dial is never deduped
        const std::lock_guard<std::mutex> lock(route_mutex);
        return dialing.insert(seed).second;
    }
    void end_dial(const std::string& seed) {
        if (seed.empty()) return;
        const std::lock_guard<std::mutex> lock(route_mutex);
        dialing.erase(seed);
    }
    // Record a peer's reported replication status (entries + version) and stamp
    // the sync time. No-op if the peer has since disconnected.
    void update_peer_status(const std::string& node_id, std::size_t entries,
                            std::string version,
                            std::chrono::steady_clock::time_point when) {
        const std::lock_guard<std::mutex> lock(mutex);
        for (PeerRecord& peer : peers) {
            if (peer.node_id != node_id) continue;
            peer.has_status = true;
            peer.entries = entries;
            peer.version = std::move(version);
            peer.last_sync = when;
            return;
        }
    }
};

namespace {

constexpr std::string_view kProtocol = "ZETTA/1";

// Diagnostic logging (gated by --log-cluster), to stderr.
void log_event(const Cluster::State* self, const std::string& message) {
    if (!self->log_events) return;
    std::fprintf(stderr, "cluster: %s\n", message.c_str());
    std::fflush(stderr);
}

// First UUID segment, for compact logs.
std::string short_id(const std::string& id) { return id.substr(0, 8); }

// Split "host:port" into its host token and validated port. The host may be a
// numeric IPv4 OR a DNS name (validated only for non-emptiness here); the port
// must be 1..65535. Returns false on a missing/out-of-range port or empty host.
// This is the general split; parse_host_port stays the numeric-IPv4 fast path.
bool split_host_port(const std::string& text, std::string& host,
                     std::uint16_t& port) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    host = text.substr(0, colon);
    if (host.empty()) return false;
    const std::string port_text = text.substr(colon + 1);
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), value);
    if (ec != std::errc{} || ptr != port_text.data() + port_text.size() ||
        value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_host_port(const std::string& text, net::ipv4_address& address,
                     std::uint16_t& port) {
    std::string host;
    if (!split_host_port(text, host, port)) return false;
    // Numeric IPv4 only: parse_ipv4_address returns a truthy error on a
    // non-numeric host (a DNS name), which this fast path rejects.
    if (net::parse_ipv4_address(host, address)) return false;
    return true;
}

// Cap on a single line, generous enough for a gossiped (base64) document blob or
// a transferred large-object body, but bounded so a misbehaving peer can't make
// us buffer without limit. base64 inflates ~4/3, so this admits ~48 MiB bodies;
// larger objects would need chunked transfer (a future step).
constexpr std::size_t kMaxLineBytes = 64 * 1024 * 1024;

// Read one '\n'-terminated line (without the newline) from the stream, using a
// PERSISTENT `pending` buffer that carries bytes past the newline to the next
// call. This is essential: one `read_some` routinely returns several lines
// coalesced (Nagle, MTU batching on a real link), and dropping the remainder
// would silently lose every line after the first — exactly the framing bug that
// stalled cross-machine gossip. Empty optional on EOF/error or overflow.
capy::task<std::optional<std::string>> read_line(net::any_stream& stream,
                                                 std::string& pending) {
    for (;;) {
        if (const auto newline = pending.find('\n');
            newline != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);  // keep everything after the '\n'
            co_return line;
        }
        if (pending.size() > kMaxLineBytes) co_return std::nullopt;
        char buffer[4096];
        net::mutable_buffer chunk(buffer, sizeof buffer);
        auto [ec, n] = co_await stream.read_some(std::span(&chunk, 1));
        if (ec || n == 0) co_return std::nullopt;
        pending.append(buffer, n);
    }
}

// Returns whether the whole line was written (false on a closed/broken stream).
capy::task<bool> write_line(net::any_stream& stream, std::string line) {
    line.push_back('\n');
    auto [ec, written] =
        co_await net::write(stream, net::const_buffer(line.data(), line.size()));
    co_return !ec && written == line.size();
}

// Split a comma-separated endpoint list into its parts, trimming surrounding
// spaces and dropping empties. A single `host:port` (no comma) yields one element —
// so an old single-address HELLO or `--peer` entry still parses as a one-element set.
std::vector<std::string> split_endpoints(const std::string& text) {
    std::vector<std::string> parts;
    std::string::size_type start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto end = comma == std::string::npos ? text.size() : comma;
        std::string part = text.substr(start, end - start);
        while (!part.empty() && part.front() == ' ') part.erase(0, 1);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) parts.push_back(std::move(part));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return parts;
}

// Parse "ZETTA/1 HELLO <node_id> <ep1[,ep2,...]>". The endpoints are a single
// whitespace-delimited, comma-separated token — the peer's advertised candidate
// SET (#739). A legacy single-address HELLO parses as a one-element set (backward
// compatible). `address` defaults to the first advertised endpoint; the observer
// overrides it with the endpoint it actually reached the peer on. Returns an empty
// node_id on anything malformed.
ClusterPeer parse_hello(const std::string& line) {
    std::istringstream parser(line);
    std::string protocol, verb, endpoints_token;
    ClusterPeer peer;
    parser >> protocol >> verb >> peer.node_id >> endpoints_token;
    if (protocol != kProtocol || verb != "HELLO") return ClusterPeer{};
    peer.endpoints = split_endpoints(endpoints_token);
    if (!peer.endpoints.empty()) peer.address = peer.endpoints.front();
    return peer;
}

std::string hello_line(const std::string& node_id,
                       const std::vector<std::string>& endpoints) {
    std::string joined;
    for (const std::string& endpoint : endpoints) {
        if (!joined.empty()) joined.push_back(',');
        joined += endpoint;
    }
    return std::string(kProtocol) + " HELLO " + node_id + " " + joined;
}

// --- Replication gossip -----------------------------------------------------
//
// Once peers have shaken hands they exchange replicated state on a timer:
//   ZETTA/1 SYNC <base64 of ReplicationManager::save()>   — merge into ours
//   ZETTA/1 STATUS <entries> <version>                    — its status, for nodes()
// SYNC carries the whole document (the ReplicateToAllNodes policy); the CRDT
// merge is conflict-free, so order and duplication don't matter.

// base64 lives in the shared internal header so the signal plane (signal.cpp)
// uses the same implementation — see src/core/base64.hpp.
using detail::base64_decode;
using detail::base64_decode_to_string;
using detail::base64_encode;
using detail::base64_encode_bytes;

std::string sync_line(const ReplicationManager& replication) {
    return std::string(kProtocol) + " SYNC " +
           base64_encode(replication.save());
}

std::string status_line(const ReplicationManager& replication) {
    return std::string(kProtocol) + " STATUS " +
           std::to_string(replication.entry_count()) + " " +
           replication.version();
}

std::string loreq_line(const std::string& id) {
    return std::string(kProtocol) + " LOREQ " + id;
}
std::string lores_line(const std::string& id, const std::string& body) {
    return std::string(kProtocol) + " LORES " + id + " " +
           base64_encode_bytes(body);
}

// A no-op keepalive. Membership/signal-only links carry no periodic gossip, so a
// PING every cycle keeps the peer's idle-timeout fed (silence == a dead link).
std::string ping_line() { return std::string(kProtocol) + " PING"; }

// One signal frame: `ZETTA/1 SIGNAL <compact-json-envelope>`. The envelope is a
// single line (compact JSON, payload base64'd inside) — see signal.cpp.
std::string signal_line(const Signal& signal) {
    return std::string(kProtocol) + " SIGNAL " + encode_signal(signal);
}

// Per-connection state. Both the read pump and the per-link writer run on the
// single io_context thread (cooperative, never truly concurrent). Cross-thread
// outbound goes through Cluster::State::outbox (keyed by peer node id), not this
// struct, so callers on other threads never touch a link directly.
//
// The line protocol runs over `stream` — the raw socket when the cluster is
// plaintext, or the TLS layer when security is on (`tls` then owns the session
// and wraps `socket` in reference mode). `socket` stays directly reachable so
// the keepalive can force-close the connection under either transport; the
// TLS session supports one in-flight read + one in-flight write, which is
// exactly the read-pump + link-writer pattern.
// Dispatch one received signal. `peer_node_id` is the sender. A Request is
// answered by the registered handler and its Response queued back to the sender;
// a Response wakes the waiting RPC; an Event/Task is delivered to the handler.
// Runs on the network thread (the read pump).
void handle_signal(Cluster::State* self, const std::string& envelope,
                   const std::string& peer_node_id) {
    std::string reason;
    std::optional<Signal> signal = decode_signal(envelope, &reason);
    if (!signal) {
        log_event(self, "<- bad SIGNAL from " + short_id(peer_node_id) + ": " +
                            reason);
        return;
    }
    signal->from = peer_node_id;
    SignalHandler* handler = self->signal_handler.load(std::memory_order_acquire);
    switch (signal->kind) {
        case SignalKind::Request: {
            const SignalResponse result =
                handler ? handler->on_request(*signal)
                        : SignalResponse{.ok = false, .error = "no handler"};
            Signal reply;
            reply.kind = SignalKind::Response;
            reply.id = signal->id;
            reply.ok = result.ok;
            reply.payload = result.payload;
            reply.error = result.error;
            self->queue_outbound(peer_node_id, signal_line(reply));
            break;
        }
        case SignalKind::Response: {
            if (auto promise = self->take_pending(signal->id)) {
                promise->set_value(SignalResponse{.ok = signal->ok,
                                                  .payload = signal->payload,
                                                  .error = signal->error});
            }
            break;
        }
        case SignalKind::Event:
            if (handler) handler->on_event(*signal);
            break;
        case SignalKind::Task:
            if (handler) handler->on_task(*signal);
            break;
    }
}

// Apply one received line to our state. `peer_node_id` is who sent it; replies
// (a LORES for a LOREQ, a Response for a Request) are queued on the peer's outbox
// for its writer to send. Runs on the network thread (the read pump).
void apply_gossip(Cluster::State* self, const std::string& line,
                  const std::string& peer_node_id) {
    std::istringstream parser(line);
    std::string protocol, verb;
    parser >> protocol >> verb;
    if (protocol != kProtocol) return;
    if (verb == "SYNC") {
        std::string encoded;
        parser >> encoded;
        if (self->replication && !encoded.empty()) {
            self->replication->merge(base64_decode(encoded));
        }
    } else if (verb == "STATUS") {
        std::size_t entries = 0;
        std::string version;
        parser >> entries >> version;
        log_event(self, "<- STATUS from " + short_id(peer_node_id) +
                            " entries=" + std::to_string(entries) + " ver=" +
                            version);
        self->update_peer_status(peer_node_id, entries, std::move(version),
                                 std::chrono::steady_clock::now());
    } else if (verb == "LOREQ") {
        std::string id;
        parser >> id;
        if (self->blobs && !id.empty()) {
            auto body = self->blobs->read_blob(id);
            log_event(self, "<- LOREQ " + short_id(id) + " from " +
                                short_id(peer_node_id) +
                                (body ? " (serving " + std::to_string(body->size()) +
                                            "B)"
                                      : " (we don't have it)"));
            if (body) self->queue_outbound(peer_node_id, lores_line(id, *body));
        }
    } else if (verb == "LORES") {
        std::string id, encoded;
        parser >> id >> encoded;
        if (self->blobs && !id.empty() && !encoded.empty()) {
            const std::string body = base64_decode_to_string(encoded);
            const bool ok = self->blobs->store_blob(id, body);
            log_event(self, "<- LORES " + short_id(id) + " (" +
                                std::to_string(body.size()) + "B) -> " +
                                (ok ? "stored" : "REJECTED (hash/size)"));
        }
    } else if (verb == "SIGNAL") {
        // The envelope is the remainder of the line: compact JSON, but it may
        // contain spaces inside string values, so take the rest rather than a
        // whitespace-delimited token.
        std::string envelope;
        std::getline(parser, envelope);
        if (!envelope.empty() && envelope.front() == ' ') envelope.erase(0, 1);
        handle_signal(self, envelope, peer_node_id);
    }
    // Any other verb (e.g. PING) is a no-op — last_activity was already
    // refreshed by the read pump, which is all a keepalive needs to do.
}

// How often the writer wakes to flush queued signals (RPC/events/tasks/LORES).
// Short, so a signal leaves within a tick; the heavier gossip/keepalive runs on
// its own ~0.5s cadence below. (A future optimization could replace the poll
// with an explicit cross-thread wake; the backend's frame allocator isn't
// thread-safe, so a poll keeps producers off the network thread for now.)
constexpr std::chrono::milliseconds kOutboxPollInterval{20};
constexpr std::chrono::milliseconds kGossipInterval{500};

// Per-link output pump: flush this peer's queued signals every tick, push our
// replicated document + status (or a PING keepalive) and request missing
// large-object bodies every ~0.5s, and drop the link if the peer goes silent —
// until the connection dies. Runs alongside the read pump (one outstanding
// write, one read — the full-duplex pattern a socket allows). Always spawned
// (the signal plane is always available), even with no replication/blobs.
capy::task<> link_writer(Cluster::State* self, std::shared_ptr<GossipLink> link,
                         std::string peer_node_id) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    net::timer timer(self->io_context);
    std::map<std::string, Clock::time_point> requested;  // body request cooldown
    Clock::time_point last_gossip = Clock::now() - kGossipInterval;  // gossip now
    while (link->alive) {
        // Keepalive: a healthy peer gossips/PINGs us every ~0.5s, so prolonged
        // silence means the link died (e.g. a silent TCP drop). Close it —
        // that unblocks the read pump, which removes the peer.
        if (Clock::now() - link->last_activity > self->idle_timeout) {
            log_event(self, "keepalive timeout — dropping idle peer connection");
            link->socket->close();
            break;
        }
        // Flush queued signals (RPC requests/responses, events, tasks) and any
        // large-object bodies a peer asked us for — promptly, every tick.
        bool flushed = true;
        for (std::string& line : self->take_outbound(peer_node_id)) {
            if (!co_await write_line(link->stream, std::move(line))) {
                flushed = false;
                break;
            }
        }
        if (!flushed) break;
        // Periodic gossip + keepalive, every ~0.5s (not every poll tick).
        if (Clock::now() - last_gossip >= kGossipInterval) {
            last_gossip = Clock::now();
            if (self->replication) {
                if (!co_await write_line(link->stream,
                                         sync_line(*self->replication)) ||
                    !co_await write_line(link->stream,
                                         status_line(*self->replication))) {
                    break;
                }
            } else if (!co_await write_line(link->stream, ping_line())) {
                // No gossip to send — a PING keeps the link alive.
                break;
            }
            // Request bodies we lack, but don't re-ask the same peer too often
            // (a transfer in flight shouldn't trigger duplicate sends).
            if (self->blobs) {
                const Clock::time_point now = Clock::now();
                bool requested_ok = true;
                for (const std::string& id : self->blobs->missing_blobs()) {
                    const auto it = requested.find(id);
                    if (it != requested.end() && now - it->second < 2s) continue;
                    requested[id] = now;
                    log_event(self, "-> LOREQ " + short_id(id));
                    if (!co_await write_line(link->stream, loreq_line(id))) {
                        requested_ok = false;
                        break;
                    }
                }
                if (!requested_ok) break;
            }
        }
        timer.expires_after(kOutboxPollInterval);
        [[maybe_unused]] const auto wait_result = co_await timer.wait();
    }
}

// Run the symmetric handshake over an established connection and stay joined
// until the connection drops. `initiate` controls who speaks first (the dialer
// sends HELLO first; the acceptor replies) — the recorded result is identical
// either way. With security on, the mutual-TLS handshake runs FIRST: no
// protocol byte crosses the wire before both sides have proven a certificate
// chained to the token-derived cluster CA, so an unenrolled dialer (or a
// plaintext one) is dropped here and never reaches HELLO.
// `my_endpoints` is the candidate set we advertise in our HELLO. `dialed_endpoint`
// is the address we connected to when we dialed (empty when we accepted) — the
// endpoint we reached this peer on, recorded as its per-observer path. `seed_key`
// is the original seed string this dial came from (empty for accepts / non-seed
// dials), used to dedup redials by the node id it resolves to (#739).
capy::task<> handshake_and_hold(Cluster::State* self, net::tcp_socket socket,
                                std::string my_node_id,
                                std::vector<std::string> my_endpoints,
                                bool initiate, std::string dialed_endpoint,
                                std::string seed_key) {
    // The connection's transport, built before any line is exchanged: the raw
    // socket, or the TLS layer over it. Lives in the link so the writer pump
    // shares it (and so the keepalive can still close the raw socket).
    auto link = std::make_shared<GossipLink>();
    link->socket = std::make_shared<net::tcp_socket>(std::move(socket));
    link->initiate = initiate;
    if (self->tls) {
        link->tls = std::make_unique<net::openssl_stream>(link->socket.get(),
                                                          *self->tls);
        auto [handshake_error] = co_await link->tls->handshake(
            initiate ? net::tls_stream::client : net::tls_stream::server);
        if (handshake_error) {
            log_event(self, std::string("TLS handshake failed (") +
                                (initiate ? "dialed" : "accepted") +
                                "): " + handshake_error.message());
            link->socket->close();
            co_return;
        }
        link->stream = net::any_stream(link->tls.get());
    } else {
        link->stream = net::any_stream(link->socket.get());
    }

    // One read buffer for the whole connection — the handshake read and the
    // read pump must share it, or bytes the peer sent right after HELLO are lost.
    std::string pending;
    std::optional<std::string> their_hello;
    if (initiate) {
        co_await write_line(link->stream, hello_line(my_node_id, my_endpoints));
        their_hello = co_await read_line(link->stream, pending);
    } else {
        their_hello = co_await read_line(link->stream, pending);
        co_await write_line(link->stream, hello_line(my_node_id, my_endpoints));
    }
    if (!their_hello) {
        link->socket->close();
        co_return;
    }
    const ClusterPeer peer = parse_hello(*their_hello);
    if (peer.node_id.empty() || peer.node_id == my_node_id) {
        link->socket->close();  // malformed, or we dialed ourselves
        co_return;
    }
    // Record which node id this seed resolved to BEFORE dedup, so the reconnect loop
    // can skip the seed while this node is linked — even if this very link then loses
    // the register_link tiebreak (the winning link still holds the node). Ends the
    // redial churn a seed whose string never matches the advertised address caused.
    self->note_seed_node(seed_key, peer.node_id);
    // Keep exactly one connection per peer: a reconnect race, a seed whose address
    // never matches the peer's advertised one (127.0.0.1, a LB VIP), or a mutual dial
    // would otherwise leave duplicate held links (and burn CPU servicing them). Reap
    // to one before we register the peer or spawn its pumps.
    std::shared_ptr<GossipLink> superseded;
    if (!self->register_link(peer.node_id, link, my_node_id, superseded)) {
        link->socket->close();  // a canonical link already holds this peer
        co_return;
    }
    if (superseded) superseded->socket->close();  // we displaced a stale link
    self->live_connections.fetch_add(1, std::memory_order_relaxed);
    // The endpoint we reached this peer on, from THIS vantage point: the address we
    // dialed (the reachable candidate) or, when we accepted, its advertised front.
    const std::string reached =
        !dialed_endpoint.empty() ? dialed_endpoint : peer.address;
    self->add_peer(peer.node_id, reached, peer.endpoints);
    log_event(self, std::string("+ peer ") + short_id(peer.node_id) + " @ " +
                        reached + (initiate ? " (dialed)" : " (accepted)"));

    // Per-connection link shared between this read pump and the per-link write
    // pump (both on the io thread). The writer flushes queued signals + periodic
    // gossip + body requests and enforces the keepalive timeout. Always spawned
    // — the signal plane needs the output pump even with no replication/blobs.
    link->last_activity = std::chrono::steady_clock::now();
    capy::run_async(self->io_context.get_executor())(
        link_writer(self, link, peer.node_id));

    // Read pump: merge gossiped state, record peer status, serve/receive
    // large-object bodies, dispatch signals, until disconnect (or the writer's
    // keepalive closes the socket, which surfaces here as a read error).
    for (;;) {
        std::optional<std::string> line =
            co_await read_line(link->stream, pending);
        if (!line) break;  // peer disconnected / link timed out
        link->last_activity = std::chrono::steady_clock::now();
        apply_gossip(self, *line, peer.node_id);
    }

    link->alive = false;
    // Only the currently-registered link tears down the shared peer state — a link
    // that was already superseded must not evict its replacement's peer record.
    if (self->unregister_link(peer.node_id, link.get())) {
        // Membership hysteresis (#739): flag the peer not-connected but keep it in
        // the membership view through the grace window, so a brief blip / reconnect
        // doesn't flap it out of nodes(). Outbox + pending RPCs go now regardless —
        // nothing can be sent over a dead link.
        self->mark_peer_disconnected(peer.node_id);
        self->clear_outbound(peer.node_id);        // drop any lines queued for it
        self->fail_pending_for_peer(peer.node_id);  // fail RPCs awaiting it
        log_event(self, std::string("- peer ") + short_id(peer.node_id) +
                            " (connection closed)");
    }
    self->live_connections.fetch_sub(1, std::memory_order_relaxed);
    link->socket->close();
}

capy::task<> accept_loop(Cluster::State* self, std::string my_node_id,
                         std::vector<std::string> my_endpoints) {
    for (;;) {
        net::tcp_socket peer(self->io_context);
        if (auto [ec] = co_await self->acceptor->accept(peer); ec) co_return;
        capy::run_async(self->io_context.get_executor())(handshake_and_hold(
            self, std::move(peer), my_node_id, my_endpoints,
            /*initiate=*/false, /*dialed_endpoint=*/std::string{},
            /*seed_key=*/std::string{}));
    }
}

// Connect to one already-resolved endpoint and, on success, run the handshake and
// hold the link until it drops. `dialed_label` is the candidate string recorded as
// the endpoint we reached the peer on — the STABLE seed/DNS name, not an ephemeral
// resolved IP, so the per-observer path survives a pod IP change. Returns true if
// the connect succeeded (the link was held and has since dropped); false if this
// endpoint wasn't reachable from here (the caller tries the next candidate).
capy::task<bool> connect_and_hold(Cluster::State* self, net::endpoint endpoint,
                                  std::string dialed_label,
                                  std::string my_node_id,
                                  std::vector<std::string> my_endpoints,
                                  std::string seed_key) {
    net::tcp_socket socket(self->io_context);
    socket.open();
    if (auto [ec] = co_await socket.connect(endpoint); ec) co_return false;
    co_await handshake_and_hold(self, std::move(socket), std::move(my_node_id),
                                std::move(my_endpoints), /*initiate=*/true,
                                std::move(dialed_label), std::move(seed_key));
    co_return true;
}

// Dial one seed, trying its candidate endpoints in order until one connects — the
// reachable path from THIS vantage point (a peer behind NAT/LB advertises several
// candidates; only some are reachable here). A candidate whose host is a numeric
// IPv4 connects directly; a candidate whose host is a DNS name (e.g. a k8s
// StatefulSet per-pod address `foo-0.foo-headless.ns.svc.cluster.local:7777`) is
// RESOLVED at dial time and its resolved address(es) tried in order (#785).
// Resolution is never cached — a pod restart gets a fresh IP, so we re-resolve on
// every dial. `seed_key` is the original seed string, used to dedup redials by the
// node id it resolves to. Holds the connection until it drops. The dial-in-flight
// claim is released on every exit path (RAII) so a later reconnect can redial once
// the link is truly gone.
capy::task<> dial_seed(Cluster::State* self, std::vector<std::string> candidates,
                       std::string my_node_id,
                       std::vector<std::string> my_endpoints,
                       std::string seed_key) {
    struct DialGuard {
        Cluster::State* self;
        std::string seed;
        ~DialGuard() { self->end_dial(seed); }
    } guard{self, seed_key};

    for (const std::string& candidate : candidates) {
        std::string host;
        std::uint16_t port = 0;
        if (!split_host_port(candidate, host, port)) continue;  // malformed
        net::ipv4_address numeric;
        if (!net::parse_ipv4_address(host, numeric)) {
            // A numeric IPv4 host — connect directly (the unchanged fast path).
            if (co_await connect_and_hold(self, net::endpoint(numeric, port),
                                          candidate, my_node_id, my_endpoints,
                                          seed_key)) {
                co_return;  // connected (and since dropped); reconnect retries
            }
            continue;  // not reachable from here — try the next candidate
        }
        // A DNS hostname: resolve it at dial time (never cached — pod IPs are
        // ephemeral) and try each resolved address in order (#785).
        net::resolver resolver(self->io_context);
        auto [resolve_error, results] =
            co_await resolver.resolve(host, std::to_string(port));
        if (resolve_error) {
            log_event(self, "resolve " + host + " failed: " +
                                resolve_error.message());
            continue;  // unresolvable from here — try the next candidate
        }
        for (const auto& entry : results) {
            if (co_await connect_and_hold(self, entry.get_endpoint(), candidate,
                                          my_node_id, my_endpoints, seed_key)) {
                co_return;  // connected on a resolved address; path stays the name
            }
        }
    }
}

// Start a dial for one seed, unless a dial for it is already in flight. Splits the
// seed into its candidate endpoints (a comma-separated set names one peer reachable
// at several addresses). Runs on the network thread.
void spawn_dial(Cluster::State* self, const std::string& seed,
                const std::string& my_node_id,
                const std::vector<std::string>& my_endpoints) {
    if (!self->begin_dial(seed)) return;  // a dial for this seed is already in flight
    std::vector<std::string> candidates = split_endpoints(seed);
    if (candidates.empty()) {  // nothing dial-able — release the claim
        self->end_dial(seed);
        return;
    }
    log_event(self, "dial " + seed);
    capy::run_async(self->io_context.get_executor())(
        dial_seed(self, std::move(candidates), my_node_id, my_endpoints, seed));
}

// Periodically re-dial any seed whose node we don't currently hold a live link to,
// so a dropped link re-establishes itself. Dedups by NODE ID, not by matching the
// seed string against advertised addresses: a seed that never matches (a 127.0.0.1
// loopback or a load-balancer VIP vs. the peer's advertised address) used to redial
// every interval; now, once the seed has resolved to a node that's linked, it's
// skipped — ending the connect+handshake churn (#739). Runs for the cluster's life.
capy::task<> reconnect_loop(Cluster::State* self, std::vector<std::string> seeds,
                            std::string my_node_id,
                            std::vector<std::string> my_endpoints) {
    net::timer timer(self->io_context);
    for (;;) {
        timer.expires_after(self->reconnect_interval);
        [[maybe_unused]] const auto wait_result = co_await timer.wait();
        for (const std::string& seed : seeds) {
            const std::string resolved = self->seed_resolved_node(seed);
            if (!resolved.empty() && self->has_live_link(resolved)) {
                continue;  // this seed's node already holds a live link — no redial
            }
            spawn_dial(self, seed, my_node_id, my_endpoints);
        }
    }
}

// Age peers out of the membership view once their link has been gone past the grace
// window (the down side of membership hysteresis, #739). A short cadence is enough —
// nodes() also applies the grace on read, so this is just cleanup. Runs for life.
capy::task<> membership_loop(Cluster::State* self) {
    using namespace std::chrono_literals;
    net::timer timer(self->io_context);
    for (;;) {
        timer.expires_after(1s);
        [[maybe_unused]] const auto wait_result = co_await timer.wait();
        self->reap_members();
    }
}

// True for the IPv4 loopback range (127.0.0.0/8).
bool is_loopback_ipv4(const std::string& address) {
    return address.rfind("127.", 0) == 0;
}

// The machine's IPv4 addresses on up, running interfaces — what a peer on
// another machine could actually dial. Non-loopback addresses come first so a
// real LAN address is preferred over 127.0.0.1.
std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> addresses;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || interfaces == nullptr) return addresses;
    for (const ifaddrs* it = interfaces; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0 ||
            (it->ifa_flags & IFF_RUNNING) == 0) {
            continue;
        }
        const auto* in = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof text) == nullptr) {
            continue;
        }
        std::string address(text);
        if (std::find(addresses.begin(), addresses.end(), address) ==
            addresses.end()) {
            addresses.push_back(std::move(address));
        }
    }
    freeifaddrs(interfaces);
    std::stable_partition(addresses.begin(), addresses.end(),
                          [](const std::string& a) {
                              return !is_loopback_ipv4(a);
                          });
    return addresses;
}

}  // namespace

std::string to_string(ClusterRole role) {
    switch (role) {
        case ClusterRole::Follower:
            return "follower";
        case ClusterRole::Candidate:
            return "candidate";
        case ClusterRole::Leader:
            return "leader";
    }
    return "follower";
}

Cluster::Cluster(std::string node_id, ClusterConfig config,
                 ReplicationManager* replication, zetta::data::BlobStore* blobs)
    : state_(std::make_unique<State>()),
      node_id_(std::move(node_id)),
      config_(std::move(config)),
      role_(ClusterRole::Follower) {
    state_->replication = replication;
    state_->blobs = blobs;
    state_->idle_timeout =
        std::chrono::seconds(config_.peer_idle_timeout_seconds);
    state_->reconnect_interval =
        std::chrono::seconds(config_.reconnect_interval_seconds);
    state_->membership_grace =
        std::chrono::seconds(config_.membership_grace_seconds);
    state_->log_events = config_.log_events;
}

Cluster::~Cluster() { stop(); }

void Cluster::start() {
    net::ipv4_address host;
    if (net::parse_ipv4_address(config_.listen_host, host)) return;  // bad host

    // Mutual TLS (CTL-2, #466): one context for every link, dialed or
    // accepted — this node's identity + the cluster CA as the ONLY trust
    // anchor, the peer's certificate required and verified both ways, TLS 1.3
    // minimum. No hostname is set, so verification is chain-only (cluster
    // membership is proven by the CA, not by an address). Fail CLOSED: if any
    // credential doesn't load, the node runs no networking at all rather than
    // falling back to plaintext.
    if (config_.security.enabled()) {
        net::tls_context tls;
        const bool credentials_ok =
            !tls.use_certificate(config_.security.certificate_pem,
                                 net::tls_file_format::pem) &&
            !tls.use_private_key(config_.security.private_key_pem,
                                 net::tls_file_format::pem) &&
            !tls.add_certificate_authority(
                config_.security.ca_certificate_pem) &&
            !tls.set_verify_mode(net::tls_verify_mode::require_peer) &&
            !tls.set_min_protocol_version(net::tls_version::tls_1_3);
        if (!credentials_ok) {
            std::fprintf(stderr,
                         "zetta: cluster security credentials failed to load; "
                         "refusing to start networking\n");
            return;
        }
        state_->tls = std::move(tls);
    }

    // Bind the first free port in the range, synchronously, so listen_address()
    // is accurate before the network thread starts. Use the error-code bind/
    // listen (not the throwing ctor) — "port in use" is expected while probing.
    state_->acceptor.emplace(state_->io_context);
    state_->acceptor->open();  // tcp::v4()
    // SO_REUSEADDR before bind (#738): let a fresh process rebind the exact
    // --listen port while the previous process's accepted sockets still linger
    // in TIME_WAIT, instead of the OS refusing the bind. Set between open() and
    // bind(). set_option throws on failure; keep start() non-throwing (the
    // seam's boundary discipline) — a failure here only forfeits the fast
    // rebind, so log and carry on rather than abort startup.
    try {
        state_->acceptor->set_option(net::reuse_address(true));
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "zetta: could not set SO_REUSEADDR on cluster listener: "
                     "%s\n",
                     e.what());
    }
    for (std::uint16_t offset = 0; offset < config_.port_range; ++offset) {
        const std::uint16_t port = config_.listen_port + offset;
        if (!state_->acceptor->bind(net::endpoint(host, port)) &&
            !state_->acceptor->listen()) {
            state_->bound_port = port;
            break;
        }
    }
    if (state_->bound_port == 0) state_->acceptor.reset();  // nothing bound

    // Advertise the SET of *reachable* endpoints in the handshake — never the bare
    // wildcard (0.0.0.0), which a peer can't dial. This is the reachable listen
    // address(es) plus any --advertise extras (a NAT/LB node's public address), so
    // a peer learns every path it might dial, not just the bound pod IP (#739).
    const std::vector<std::string> my_endpoints = advertised_endpoints();
    if (state_->bound_port != 0) {
        capy::run_async(state_->io_context.get_executor())(
            accept_loop(state_.get(), node_id_, my_endpoints));
    }
    for (const std::string& seed : config_.seeds) {
        spawn_dial(state_.get(), seed, node_id_, my_endpoints);
    }
    // Keep re-dialing seeds that drop, so the cluster self-heals (deduped by node id).
    if (!config_.seeds.empty()) {
        capy::run_async(state_->io_context.get_executor())(
            reconnect_loop(state_.get(), config_.seeds, node_id_, my_endpoints));
    }
    // Age peers out of membership only after the hysteresis grace window (#739).
    capy::run_async(state_->io_context.get_executor())(
        membership_loop(state_.get()));

    state_->network_thread = std::thread([this] { state_->io_context.run(); });
}

void Cluster::stop() {
    if (state_ && state_->network_thread.joinable()) {
        state_->io_context.stop();
        state_->network_thread.join();
    }
}

std::string Cluster::listen_address() const {
    const std::uint16_t port =
        state_->bound_port != 0 ? state_->bound_port : config_.listen_port;
    return config_.listen_host + ":" + std::to_string(port);
}

bool Cluster::is_listening() const { return state_->bound_port != 0; }

std::vector<std::string> Cluster::listen_addresses() const {
    const std::uint16_t port =
        state_->bound_port != 0 ? state_->bound_port : config_.listen_port;
    const std::string suffix = ":" + std::to_string(port);

    // A wildcard bind ("0.0.0.0" / "::") accepts on every interface, but isn't
    // itself a connectable address — expand it to the machine's real IPv4s so
    // we hand peers something they can dial. Otherwise the configured host is
    // exactly what we bound.
    const bool wildcard =
        config_.listen_host == "0.0.0.0" || config_.listen_host == "::";
    if (!wildcard) return {config_.listen_host + suffix};

    std::vector<std::string> addresses;
    for (const std::string& ip : local_ipv4_addresses()) {
        addresses.push_back(ip + suffix);
    }
    if (addresses.empty()) addresses.push_back("127.0.0.1" + suffix);
    return addresses;
}

std::string Cluster::peer_command() const {
    // Prefer the first reachable address — local_ipv4_addresses() already puts
    // non-loopback (LAN) addresses ahead of 127.0.0.1. With security on, the
    // join token rides along: it is exactly what the next node needs to derive
    // the cluster CA and mint an identity this cluster will trust.
    const std::vector<std::string> addresses = listen_addresses();
    std::string command = "zetta --peer " + addresses.front();
    if (config_.security.enabled()) {
        command += " --join-token " + config_.security.join_token;
    }
    return command;
}

std::vector<std::string> Cluster::advertised_endpoints() const {
    // The reachable listen address(es) first (non-loopback ahead of loopback), then
    // any explicit --advertise extras a peer might dial (a NAT/LB public address),
    // deduped. This is the candidate set carried in the HELLO (#739).
    std::vector<std::string> endpoints = listen_addresses();
    for (const std::string& extra : config_.advertise) {
        if (std::find(endpoints.begin(), endpoints.end(), extra) ==
            endpoints.end()) {
            endpoints.push_back(extra);
        }
    }
    return endpoints;
}

std::vector<ClusterPeer> Cluster::peers() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<ClusterPeer> result;
    result.reserve(state_->peers.size());
    for (const State::PeerRecord& peer : state_->peers) {
        if (!peer.connected) continue;  // live-link view; hysteresis-only excluded
        result.push_back(ClusterPeer{.node_id = peer.node_id,
                                     .address = peer.address,
                                     .endpoints = peer.endpoints});
    }
    return result;
}

std::size_t Cluster::peer_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return static_cast<std::size_t>(
        std::count_if(state_->peers.begin(), state_->peers.end(),
                      [](const State::PeerRecord& p) { return p.connected; }));
}

std::size_t Cluster::connection_count() const {
    const int count = state_->live_connections.load(std::memory_order_relaxed);
    return count > 0 ? static_cast<std::size_t>(count) : 0;
}

std::vector<NodeStatus> Cluster::nodes() const {
    // Read our own replication status first (its own lock), so we can flag each
    // peer in_sync when its reported version matches ours.
    const bool have_replication = state_->replication != nullptr;
    std::string my_version;
    std::size_t my_entries = 0;
    if (have_replication) {
        my_version = state_->replication->version();
        my_entries = state_->replication->entry_count();
    }
    const std::string my_address = listen_addresses().front();
    const auto now = std::chrono::steady_clock::now();

    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<NodeStatus> result;
    result.reserve(state_->peers.size() + 1);

    NodeStatus self;
    self.node_id = node_id_;
    self.address = my_address;
    self.role = role_;
    self.is_self = true;
    self.connected = true;
    self.replication.known = have_replication;
    self.replication.entries = my_entries;
    self.replication.version = my_version;
    self.replication.in_sync = true;            // trivially in sync with itself
    self.replication.seconds_since_sync = -1.0;  // not applicable
    result.push_back(std::move(self));

    for (const State::PeerRecord& peer : state_->peers) {
        // Membership hysteresis (#739): a peer whose link just dropped stays listed
        // through the grace window (connected=false) so a brief blip doesn't flap it
        // out; it's omitted only once the grace lapses (reap_members() also erases it).
        if (!peer.connected &&
            (now - peer.last_seen) > state_->membership_grace) {
            continue;
        }
        NodeStatus node;
        node.node_id = peer.node_id;
        node.address = peer.address;
        node.role = ClusterRole::Follower;
        node.is_self = false;
        node.connected = peer.connected;
        node.replication.known = peer.has_status;
        node.replication.entries = peer.entries;
        node.replication.version = peer.version;
        node.replication.in_sync =
            peer.has_status && have_replication && peer.version == my_version;
        node.replication.seconds_since_sync =
            peer.has_status
                ? std::chrono::duration<double>(now - peer.last_sync).count()
                : -1.0;
        result.push_back(std::move(node));
    }
    return result;
}

// --- Signal plane -----------------------------------------------------------

namespace {

// Node ids of peers with a live link (for addressing + counts). A peer kept only by
// membership hysteresis (its link dropped) is excluded — the signal plane can't reach
// it, so it must not be addressed until a live link is re-established.
std::vector<std::string> connected_node_ids(const Cluster::State& state) {
    const std::lock_guard<std::mutex> lock(state.mutex);
    std::vector<std::string> ids;
    ids.reserve(state.peers.size());
    for (const Cluster::State::PeerRecord& peer : state.peers) {
        if (peer.connected) ids.push_back(peer.node_id);
    }
    return ids;
}

bool is_connected(const Cluster::State& state, const std::string& node_id) {
    const std::lock_guard<std::mutex> lock(state.mutex);
    for (const Cluster::State::PeerRecord& peer : state.peers) {
        if (peer.node_id == node_id && peer.connected) return true;
    }
    return false;
}

}  // namespace

void Cluster::set_signal_handler(SignalHandler* handler) {
    state_->signal_handler.store(handler, std::memory_order_release);
}

SignalHandler* Cluster::signal_handler() const {
    return state_->signal_handler.load(std::memory_order_acquire);
}

SignalResponse Cluster::request(const std::string& node_id,
                                const std::string& method,
                                const std::string& payload,
                                std::chrono::milliseconds timeout) {
    if (!is_connected(*state_, node_id)) {
        return SignalResponse{.ok = false, .error = "peer not connected"};
    }

    Signal request;
    request.kind = SignalKind::Request;
    request.id = zetta::util::new_uuid();  // correlates the response back to this call
    request.method = method;
    request.payload = payload;

    // Register the waiter BEFORE queuing, so a fast response is never missed.
    std::future<SignalResponse> future =
        state_->add_pending(request.id, node_id);
    state_->queue_outbound(node_id, signal_line(request));

    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    }
    // Timed out. Claim the slot so a late response is dropped; if claiming fails
    // the response landed in the race window, so return it.
    if (state_->take_pending(request.id)) {
        return SignalResponse{.ok = false, .error = "request timed out"};
    }
    return future.get();
}

bool Cluster::send_event(const std::string& node_id, const std::string& method,
                         const std::string& payload) {
    if (!is_connected(*state_, node_id)) return false;
    Signal event;
    event.kind = SignalKind::Event;
    event.method = method;
    event.payload = payload;
    state_->queue_outbound(node_id, signal_line(event));
    return true;
}

std::size_t Cluster::broadcast_event(const std::string& method,
                                     const std::string& payload) {
    Signal event;
    event.kind = SignalKind::Event;
    event.method = method;
    event.payload = payload;
    const std::string line = signal_line(event);
    const std::vector<std::string> ids = connected_node_ids(*state_);
    for (const std::string& id : ids) state_->queue_outbound(id, line);
    return ids.size();
}

bool Cluster::send_task(const std::string& node_id, const std::string& method,
                        const std::string& payload) {
    if (!is_connected(*state_, node_id)) return false;
    Signal task;
    task.kind = SignalKind::Task;
    task.method = method;
    task.payload = payload;
    state_->queue_outbound(node_id, signal_line(task));
    return true;
}

}  // namespace zetta::cluster