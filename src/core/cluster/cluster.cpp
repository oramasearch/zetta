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
#include "zetta/transport/framed_stream.hpp"
#include "zetta/transport/security.hpp"
#include "zetta/util/uuid.hpp"

#include "../base64.hpp"
#include "../catalog.hpp"

// src/core/cluster.cpp is the networking adapter: the one .cpp allowed to use
// the Corosio backend (via zetta::runtime::net). Cluster's public header
// exposes no backend types, so the rest of zetta stays decoupled.
namespace zetta::cluster {

namespace capy = boost::capy;
namespace net = zetta::runtime::net;

// One peer connection, shared between the read pump and per-link write pump.
// `initiate` records whether we dialed it (true) or accepted it (false) — used
// to pick a single canonical link when both sides connect to each other (the
// min-node-id-initiator wins), so duplicate links reap.
struct GossipLink {
    std::shared_ptr<zetta::transport::FramedStream> stream;
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
        // Reachability from its HELLO (#803): a dial-out-only peer advertises no
        // dialable address — it is reached ONLY over the inbound link it opened, and
        // must never be dialed (known_member_endpoints skips it).
        bool dial_out_only = false;
        // Is a live link to this peer held right now? A record with connected=false
        // is retained only by membership hysteresis until its grace window lapses.
        bool connected = true;
        // When a live link was last held (stamped on connect and on disconnect) —
        // the clock the hysteresis grace window is measured from.
        std::chrono::steady_clock::time_point last_seen{};
        bool has_status = false;  // have we received a STATUS from it yet?
        std::size_t entries = 0;
        std::string version;
        // The replication clock it last reported (#794): what its document holds,
        // so our gossip ships it only the changes it lacks. Empty when the peer
        // advertises none (an older node) — it then gets the whole document.
        std::string clock;
        std::chrono::steady_clock::time_point last_sync{};
        unsigned audit_protocol_version = 0;
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
    unsigned audit_protocol_version = zetta::governance::kAuditProtocolVersion;
    // These locks guard only the bounded owner/network message handoff.
    std::mutex audit_mutex;
    std::vector<AuditMessage> audit_messages;
    std::size_t audit_message_bytes = 0;
    std::string audit_status;
    std::chrono::steady_clock::duration idle_timeout{};
    std::chrono::milliseconds setup_timeout{5000};
    std::size_t maximum_pending_setups = 64;
    std::size_t pending_setups = 0;  // network thread only, accepted links

    std::chrono::steady_clock::duration reconnect_interval{};
    // How long a peer stays in the membership view after its live link drops
    // (hysteresis, #739). Zero drops it immediately.
    std::chrono::steady_clock::duration membership_grace{};
    bool log_events = false;
    // Reachability of THIS node (#803): when true we advertise no dialable address
    // in our HELLO (an empty candidate set + the dial-out marker) — we are a pure
    // initiator. Set from ClusterConfig::dial_out_only in start().
    bool dial_out_only = false;
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
    // Replication gossip sent (#794), summed over links; read by gossip_stats().
    std::atomic<std::uint64_t> gossip_rounds{0};
    std::atomic<std::uint64_t> gossip_sync_frames{0};
    std::atomic<std::uint64_t> gossip_full_syncs{0};
    std::atomic<std::uint64_t> gossip_incremental_syncs{0};
    std::atomic<std::uint64_t> gossip_sync_bytes{0};

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
    // Signal dispatches in flight on the network thread (#1131). A dispatch
    // counts itself BEFORE it loads the handler, so after set_signal_handler
    // swaps the pointer and waits for this to drain, no call can still be
    // running in the previous handler: a component may restore its predecessor
    // and then be destroyed safely.
    std::atomic<int> dispatching{0};
    std::atomic<std::thread::id> network_thread_id{};

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
                  std::vector<std::string> endpoints, bool dial_out_only) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(peers, [&](const PeerRecord& p) {
            // Dedup by node id, or by the reached address — but NEVER match on an
            // empty address, which every dial-out-only peer shares (#803): matching
            // it would evict an unrelated dial-out peer.
            return p.node_id == node_id ||
                   (!address.empty() && p.address == address);
        });
        peers.push_back(PeerRecord{.node_id = std::move(node_id),
                                   .address = std::move(address),
                                   .endpoints = std::move(endpoints),
                                   .dial_out_only = dial_out_only,
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
    // Record a peer's reported replication status (entries + version + clock)
    // and stamp the sync time. No-op if the peer has since disconnected.
    void update_peer_status(const std::string& node_id, std::size_t entries,
                            std::string version, std::string clock,
                            std::chrono::steady_clock::time_point when) {
        const std::lock_guard<std::mutex> lock(mutex);
        for (PeerRecord& peer : peers) {
            if (peer.node_id != node_id) continue;
            peer.has_status = true;
            peer.entries = entries;
            peer.version = std::move(version);
            peer.clock = std::move(clock);
            peer.last_sync = when;
            return;
        }
    }

    // The clock a peer reported on the current link (#794): one stamped at or
    // after `since`, the moment that link's writer started. A clock from an
    // earlier link is never trusted — the peer may have restarted from an older
    // document since — so a new link opens with the whole document. nullopt
    // until the peer has sent a STATUS on this link; empty when its STATUS
    // carries no clock (a node from before #794).
    std::optional<std::string> peer_clock_since(
        const std::string& node_id, std::chrono::steady_clock::time_point since) const {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const PeerRecord& peer : peers) {
            if (peer.node_id != node_id) continue;
            if (!peer.has_status || peer.last_sync < since) return std::nullopt;
            return peer.clock;
        }
        return std::nullopt;
    }

    // Every OTHER node we know about, keyed by node id, mapped to the endpoint(s)
    // we might dial it on (self excluded). Two sources (#789):
    //   - a DIRECT peer's advertised HELLO candidate set (plus the path we reached
    //     it on) — including a peer held only by membership hysteresis, so a dropped
    //     link is redialed;
    //   - a node known only TRANSITIVELY from the replicated membership document
    //     (`node:<id>` -> {address}, gossiped to us through a common peer).
    // The reconnect loop dials these so a hub-seeded STAR (every node --peer'd at one
    // seed, as k8s does) fills into a full equal-peer mesh — learned members were
    // recorded but never dialed before. Called on the network thread; locks `mutex`
    // for the peer table and reads the mutex-guarded ReplicationManager separately.
    std::map<std::string, std::vector<std::string>> known_member_endpoints(
        const std::string& my_node_id) const {
        std::map<std::string, std::vector<std::string>> members;
        const auto add_endpoint = [&](const std::string& node_id,
                                      const std::string& endpoint) {
            if (node_id.empty() || node_id == my_node_id || endpoint.empty()) {
                return;
            }
            // The replicated membership document is peer-written too: dial only a
            // well-formed node id and HOST:PORT (#771). HELLO-derived entries
            // were already checked by parse_hello.
            if (!is_valid_node_id(node_id) || !is_valid_peer_endpoint(endpoint)) {
                return;
            }
            std::vector<std::string>& list = members[node_id];
            if (std::find(list.begin(), list.end(), endpoint) == list.end()) {
                list.push_back(endpoint);
            }
        };
        {
            const std::lock_guard<std::mutex> lock(mutex);
            for (const PeerRecord& peer : peers) {
                // Never dial a dial-out-only peer (#803): it has no routable
                // address; we reach it only over the inbound link it opened.
                if (peer.dial_out_only) continue;
                for (const std::string& endpoint : peer.endpoints) {
                    add_endpoint(peer.node_id, endpoint);
                }
                add_endpoint(peer.node_id, peer.address);
            }
        }
        if (replication) {
            const nlohmann::json document = replication->snapshot();
            if (document.is_object()) {
                for (const auto& [key, value] : document.items()) {
                    if (!key.starts_with(zetta::catalog::kNodePrefix)) continue;
                    if (!value.is_object()) continue;
                    // A dial-out-only member (transitively learned via the CRDT
                    // node-list) advertises no dialable address — never dial it
                    // (#803); it syncs over its own outbound link.
                    const auto reachability = value.find("reachability");
                    if (reachability != value.end() && reachability->is_string() &&
                        reachability->get<std::string>() == "dial-out-only") {
                        continue;
                    }
                    const auto address = value.find("address");
                    if (address == value.end() || !address->is_string()) continue;
                    add_endpoint(key.substr(zetta::catalog::kNodePrefix.size()),
                                 address->get<std::string>());
                }
            }
        }
        return members;
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

}  // namespace

bool is_valid_node_id(std::string_view node_id) {
    if (node_id.empty() || node_id.size() > kMaxNodeIdBytes) return false;
    return std::ranges::all_of(node_id, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.';
    });
}

bool is_valid_peer_endpoint(std::string_view endpoint) {
    if (endpoint.size() > kMaxEndpointBytes) return false;
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::string_view host = endpoint.substr(0, colon);
    const std::string_view port_text = endpoint.substr(colon + 1);
    if (port_text.empty() || port_text.size() > 5 ||
        !std::ranges::all_of(port_text, [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return false;
    }
    unsigned port = 0;
    std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (port == 0 || port > 65535) return false;
    return std::ranges::all_of(host, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.' || character == ':' ||
               character == '[' || character == ']' || character == '%';
    });
}

// Parse "ZETTA/1 HELLO <node_id> <ep1[,ep2,...]|-> [wan|dial-out]". The endpoints
// are a single whitespace-delimited, comma-separated token — the peer's advertised
// candidate SET (#739) — or "-" for the empty set of a dial-out-only node (#803).
// The trailing reachability token is optional: absent (a legacy HELLO) or "wan"
// means wan-accessible; "dial-out" marks a node that advertises no dialable
// address and must never be dialed. A legacy single-address HELLO parses as a
// one-element wan set (backward compatible). `address` defaults to the first
// advertised endpoint; the observer overrides it with the endpoint it actually
// reached the peer on. Returns an empty node_id on anything malformed, including
// a HELLO that breaks the #771 bounds (see cluster.hpp): the node id and every
// endpoint are stored, shown in the node list and logs, and dialed, so a peer
// cannot plant control characters there or hold an unbounded endpoint list.
ClusterPeer parse_hello(std::string_view line) {
    std::istringstream parser{std::string(line)};
    std::string protocol, verb, endpoints_token, reachability_token;
    ClusterPeer peer;
    parser >> protocol >> verb >> peer.node_id >> endpoints_token >>
        reachability_token;
    if (protocol != kProtocol || verb != "HELLO") return ClusterPeer{};
    if (!is_valid_node_id(peer.node_id)) return ClusterPeer{};
    if (endpoints_token != "-") peer.endpoints = split_endpoints(endpoints_token);
    if (peer.endpoints.size() > kMaxAdvertisedEndpoints) return ClusterPeer{};
    if (!std::ranges::all_of(peer.endpoints, [](const std::string& endpoint) {
            return is_valid_peer_endpoint(endpoint);
        })) {
        return ClusterPeer{};
    }
    peer.dial_out_only = (reachability_token == "dial-out");
    // A wan peer must name somewhere it can be reached; only a dial-out-only peer
    // advertises the empty set.
    if (!peer.dial_out_only && peer.endpoints.empty()) return ClusterPeer{};
    parser >> peer.audit_protocol_version;
    if (!peer.endpoints.empty()) peer.address = peer.endpoints.front();
    return peer;
}

namespace {

std::string hello_line(const std::string& node_id,
                       const std::vector<std::string>& endpoints,
                       bool dial_out_only, unsigned audit_protocol_version) {
    std::string joined;
    for (const std::string& endpoint : endpoints) {
        if (!joined.empty()) joined.push_back(',');
        joined += endpoint;
    }
    // A dial-out-only node advertises an empty candidate set: send "-" for the
    // endpoints and the "dial-out" reachability marker so peers never dial it
    // (#803). A wan node sends its endpoints + "wan".
    if (joined.empty()) joined = "-";
    return std::string(kProtocol) + " HELLO " + node_id + " " + joined +
           (dial_out_only ? " dial-out " : " wan ") +
           std::to_string(audit_protocol_version);
}

// --- Replication gossip -----------------------------------------------------
//
// Once peers have shaken hands they exchange replicated state on a timer:
//   ZETTA/1 SYNC <base64 document blob>          — merge into ours
//   ZETTA/1 STATUS <entries> <version> <clock>   — its status, for nodes(), and
//                                                  its base64 replication clock
// SYNC carries only the changes the peer lacks, judged by the clock the peer
// last reported on this link (ReplicationManager::save_since, #794), and is
// skipped when it lacks none. It carries the whole document on a new link,
// to a peer that reports no clock (an older node), or to one too far behind.
// Either way it is a blob merge() accepts, so the receive path is unchanged;
// the CRDT merge is conflict-free, so order and duplication don't matter.

// base64 lives in the shared internal header so the signal plane (signal.cpp)
// uses the same implementation — see src/core/base64.hpp.
using detail::base64_decode;
using detail::base64_decode_to_string;
using detail::base64_encode;
using detail::base64_encode_bytes;

std::string sync_line(const std::vector<std::byte>& blob) {
    return std::string(kProtocol) + " SYNC " + base64_encode(blob);
}

// The clock rides as a trailing token, which a node that predates #794 never
// reads (it parses entries and version and stops), so mixed versions interoperate.
std::string status_line(const ReplicationManager& replication) {
    return std::string(kProtocol) + " STATUS " +
           std::to_string(replication.entry_count()) + " " +
           replication.version() + " " +
           base64_encode_bytes(replication.clock());
}

// A clock is about 20 bytes per node; anything past this is not a clock.
constexpr std::size_t kMaxClockToken = 64 * 1024;

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
    struct Dispatching {
        std::atomic<int>& count;
        explicit Dispatching(std::atomic<int>& c) : count(c) { count.fetch_add(1, std::memory_order_seq_cst); }
        ~Dispatching() { count.fetch_sub(1, std::memory_order_seq_cst); }
    } dispatching{self->dispatching};
    SignalHandler* handler = self->signal_handler.load(std::memory_order_seq_cst);
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
bool read_audit_sequence(std::istringstream& parser, std::uint64_t& sequence) {
    std::string token;
    if (!(parser >> token)) {
        return false;
    }
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), sequence);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

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
        std::string clock_token;
        parser >> entries >> version >> clock_token;
        log_event(self, "<- STATUS from " + short_id(peer_node_id) +
                            " entries=" + std::to_string(entries) + " ver=" +
                            version);
        // An absent or oversized clock leaves it empty: that peer is sent the
        // whole document, as before #794. ReplicationManager::save_since
        // validates the decoded bytes and falls back the same way.
        std::string clock = clock_token.size() <= kMaxClockToken
                                ? base64_decode_to_string(clock_token)
                                : std::string{};
        self->update_peer_status(peer_node_id, entries, std::move(version),
                                 std::move(clock), std::chrono::steady_clock::now());
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
    } else if (verb == "AUSTATUS" || verb == "AUREQ" || verb == "AURES") {
        bool compatible = false;
        {
            const std::lock_guard lock(self->mutex);
            for (const auto& peer : self->peers) {
                if (peer.node_id == peer_node_id) {
                    compatible = peer.audit_protocol_version == self->audit_protocol_version &&
                        self->audit_protocol_version == zetta::governance::kAuditProtocolVersion;
                }
            }
        }
        if (!compatible || line.size() > 24 * 1024 * 1024) {
            return;
        }
        AuditMessage message;
        message.peer = peer_node_id;
        if (verb == "AUSTATUS") {
            message.kind = AuditMessage::Kind::Status;
            std::string origin;
            zetta::governance::AuditWatermark watermark;
            while (parser >> origin) {
                if (!read_audit_sequence(parser, watermark.high) ||
                    !read_audit_sequence(parser, watermark.low) ||
                    !read_audit_sequence(parser, watermark.summary_low) ||
                    !zetta::governance::valid_audit_origin(origin) ||
                    origin == "legacy" || watermark.low > watermark.high ||
                    watermark.summary_low > watermark.high ||
                    message.watermarks.size() >= 4096) {
                    return;
                }
                if (!message.watermarks.emplace(origin, watermark).second) {
                    return;
                }
            }
        } else {
            message.kind = verb == "AUREQ" ? AuditMessage::Kind::Request : AuditMessage::Kind::Response;
            auto& batch = message.batch;
            if (!(parser >> batch.origin) || !read_audit_sequence(parser, batch.after) ||
                batch.origin == "legacy" ||
                !zetta::governance::valid_audit_origin(batch.origin)) {
                return;
            }
            if (verb == "AURES") {
                std::string encoded;
                if (!read_audit_sequence(parser, batch.next) ||
                    !read_audit_sequence(parser, batch.watermark.high) ||
                    !read_audit_sequence(parser, batch.watermark.low) ||
                    !read_audit_sequence(parser, batch.watermark.summary_low) ||
                    !(parser >> encoded)) {
                    return;
                }
                if (encoded != "-") {
                    batch.frames = base64_decode_to_string(encoded);
                }
            }
            std::string trailing;
            if (parser >> trailing) {
                return;
            }
        }
        const std::lock_guard lock(self->audit_mutex);
        if (self->audit_messages.size() >= 64 ||
            self->audit_message_bytes + message.batch.frames.size() > 20 * 1024 * 1024) {
            return;  // bounded backpressure; the receiver retries its cursor
        }
        self->audit_message_bytes += message.batch.frames.size();
        self->audit_messages.push_back(std::move(message));
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
// A link that has sent a SYNC waits for the peer's STATUS clock to show it took it
// before sending again (#794). A SYNC cut against a clock that predates the last
// one mostly re-ships what is still in flight, and a peer ingesting a large catch-up
// keeps reporting its old clock for seconds — re-sending would ship the whole
// document again. Frames on a link arrive in order or the link drops (a new link
// starts over with the whole document), so the wait is only a backstop: after
// kSyncResendAfter without the peer showing our SYNC, we resend from the clock it
// reports.
constexpr std::chrono::milliseconds kSyncResendAfter{10000};

// Per-link output pump: flush this peer's queued signals every tick, push the
// changes this peer lacks + our status (or a PING keepalive) and request missing
// large-object bodies every ~0.5s, and drop the link if the peer goes silent —
// until the connection dies. FramedStream preserves full-duplex plaintext and
// serializes access to the TLS engine (#786). Always spawned (the signal plane
// is always available), even with no replication/blobs.
capy::task<> link_writer(Cluster::State* self, std::shared_ptr<GossipLink> link,
                         std::string peer_node_id) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    net::timer timer(self->io_context);
    std::map<std::string, Clock::time_point> requested;  // body request cooldown
    Clock::time_point last_gossip = Clock::now() - kGossipInterval;  // gossip now
    // Only a clock the peer reports on THIS link sizes our SYNC (#794); our own
    // clock as of the last SYNC tells when the peer has taken it.
    const Clock::time_point link_started = Clock::now();
    bool sent_any = false;
    std::string sent_clock;
    Clock::time_point last_sync_sent{};
    while (link->alive) {
        // Keepalive: a healthy peer gossips/PINGs us every ~0.5s, so prolonged
        // silence means the link died (e.g. a silent TCP drop). Close it —
        // that unblocks the read pump, which removes the peer.
        if (Clock::now() - link->last_activity > self->idle_timeout) {
            log_event(self, "keepalive timeout — dropping idle peer connection");
            link->stream->close();
            break;
        }
        // Flush queued signals (RPC requests/responses, events, tasks) and any
        // large-object bodies a peer asked us for — promptly, every tick.
        bool flushed = true;
        for (std::string& line : self->take_outbound(peer_node_id)) {
            if (!co_await link->stream->write_frame(std::move(line))) {
                flushed = false;
                break;
            }
        }
        if (!flushed) break;
        // Periodic gossip + keepalive, every ~0.5s (not every poll tick).
        if (Clock::now() - last_gossip >= kGossipInterval) {
            last_gossip = Clock::now();
            self->gossip_rounds.fetch_add(1, std::memory_order_relaxed);
            if (self->replication) {
                // STATUS first: it is what the peer sizes its own SYNC by, so it
                // must not queue behind a large SYNC of ours (#794).
                if (!co_await link->stream->write_frame(
                        status_line(*self->replication))) {
                    break;
                }
                const std::optional<std::string> reported =
                    self->peer_clock_since(peer_node_id, link_started);
                const std::string peer_clock = reported.value_or(std::string{});
                // A peer that reports no clock (a node from before #794) can never
                // show it took a SYNC: it gets the whole document every round, as
                // before.
                const bool clockless_peer = reported && reported->empty();
                const bool awaiting_ack =
                    sent_any && !clockless_peer &&
                    !ReplicationManager::clock_covers(peer_clock, sent_clock) &&
                    Clock::now() - last_sync_sent < kSyncResendAfter;
                // Our clock is read BEFORE the delta, so it never names a change
                // the delta lacks: the peer's clock covering it means it took ours.
                std::string clock_now =
                    awaiting_ack ? std::string{} : self->replication->clock();
                const ReplicationDelta delta =
                    awaiting_ack ? ReplicationDelta{}
                                 : self->replication->save_since(peer_clock);
                if (!delta.blob.empty()) {
                    sent_any = true;
                    sent_clock = std::move(clock_now);
                    last_sync_sent = Clock::now();
                    std::string sync = sync_line(delta.blob);
                    self->gossip_sync_frames.fetch_add(1, std::memory_order_relaxed);
                    (delta.full ? self->gossip_full_syncs : self->gossip_incremental_syncs)
                        .fetch_add(1, std::memory_order_relaxed);
                    self->gossip_sync_bytes.fetch_add(sync.size(),
                                                      std::memory_order_relaxed);
                    if (!co_await link->stream->write_frame(std::move(sync))) break;
                }
            } else if (!co_await link->stream->write_frame(ping_line())) {
                // No gossip to send — a PING keeps the link alive.
                break;
            }
            std::string audit_status;
            {
                const std::lock_guard lock(self->audit_mutex);
                audit_status = self->audit_status;
            }
            bool audit_compatible = false;
            {
                const std::lock_guard lock(self->mutex);
                for (const auto& peer : self->peers) {
                    if (peer.node_id == peer_node_id) {
                        audit_compatible = peer.audit_protocol_version == self->audit_protocol_version &&
                            self->audit_protocol_version == zetta::governance::kAuditProtocolVersion;
                    }
                }
            }
            if (audit_compatible && !audit_status.empty() &&
                !co_await link->stream->write_frame(std::move(audit_status))) {
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
                    if (!co_await link->stream->write_frame(loreq_line(id))) {
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

// A slot is reserved before spawning an accepted setup and released on every
// failure or immediately after setup succeeds. Dials are bounded by our own
// seeds and peers, so they take none: an inbound flood cannot block reconnects.
// Only the network thread uses it.
struct PendingSetup {
    explicit PendingSetup(Cluster::State& cluster_state) : state(cluster_state) {
        ++state.pending_setups;
    }
    ~PendingSetup() { --state.pending_setups; }
    Cluster::State& state;
};

std::unique_ptr<PendingSetup> reserve_setup(Cluster::State& state) {
    if (state.pending_setups >= state.maximum_pending_setups) return {};
    return std::make_unique<PendingSetup>(state);
}

// One cancellable operation covers TLS plus both HELLO directions, including
// writes. Progress never restarts the setup clock (#1401).
capy::io_task<std::optional<std::string>> exchange_hello(
    Cluster::State* self, std::shared_ptr<GossipLink> link,
    std::string my_node_id, std::vector<std::string> my_endpoints) {
    if (self->tls) {
        const std::error_code handshake_error = co_await link->stream->handshake(
            link->initiate ? zetta::transport::HandshakeRole::Client
                           : zetta::transport::HandshakeRole::Server);
        if (handshake_error) co_return {handshake_error, std::nullopt};
    }
    const std::string hello = hello_line(my_node_id, my_endpoints,
                                        self->dial_out_only, self->audit_protocol_version);
    if (link->initiate && !co_await link->stream->write_frame(hello)) {
        co_return {{}, std::nullopt};
    }
    auto their_hello = co_await link->stream->read_frame();
    if (their_hello && !link->initiate && !co_await link->stream->write_frame(hello)) {
        co_return {{}, std::nullopt};
    }
    co_return {{}, std::move(their_hello)};
}

// Accepted and dialed links share the same bounded TLS/HELLO setup. An accepted
// link's slot survives every suspension (dials pass none), then steady-state
// links use the idle writer.
// dialed_endpoint records the reached candidate; seed_key preserves the original
// seed for reconnect deduplication even when its DNS address changes.
capy::task<> handshake_and_hold(Cluster::State* self, net::tcp_socket socket,
                                std::string my_node_id,
                                std::vector<std::string> my_endpoints,
                                bool initiate, std::string dialed_endpoint,
                                std::string seed_key,
                                std::unique_ptr<PendingSetup> pending_setup) {
    auto link = std::make_shared<GossipLink>();
    link->initiate = initiate;
    if (self->tls) {
        link->stream = std::make_shared<zetta::transport::FramedStream>(
            std::move(socket), *self->tls);
    } else {
        link->stream =
            std::make_shared<zetta::transport::FramedStream>(std::move(socket));
    }
    link->stream->set_maximum_frame_bytes(kMaxHelloBytes);
    auto [setup_error, their_hello] = co_await capy::timeout(
        exchange_hello(self, link, my_node_id, my_endpoints), self->setup_timeout);
    if (setup_error) {
        log_event(self, "peer setup failed: " + setup_error.message());
        link->stream->close();
        co_return;
    }
    if (!their_hello) {
        link->stream->close();
        co_return;
    }
    const ClusterPeer peer = parse_hello(*their_hello);
    if (peer.node_id.empty()) {
        // Malformed or outside the #771 bounds. The peer's text is not echoed:
        // it is exactly what the bounds keep out of the logs.
        log_event(self, std::string("refused a malformed HELLO (") +
                            (initiate ? "dialed" : "accepted") + ")");
        link->stream->close();
        co_return;
    }
    if (peer.node_id == my_node_id) {
        link->stream->close();  // we dialed ourselves
        co_return;
    }
    link->stream->set_maximum_frame_bytes(
        zetta::transport::FramedStreamOptions{}.maximum_frame_bytes);
    pending_setup.reset();  // setup succeeded; release admission before holding the link
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
        link->stream->close();  // a canonical link already holds this peer
        co_return;
    }
    if (superseded) superseded->stream->close();  // displaced a stale link
    self->live_connections.fetch_add(1, std::memory_order_relaxed);
    // The endpoint we reached this peer on, from THIS vantage point: the address we
    // dialed (the reachable candidate) or, when we accepted, its advertised front.
    const std::string reached =
        !dialed_endpoint.empty() ? dialed_endpoint : peer.address;
    self->add_peer(peer.node_id, reached, peer.endpoints, peer.dial_out_only);
    {
        const std::lock_guard lock(self->mutex);
        for (auto& record : self->peers) {
            if (record.node_id == peer.node_id) {
                record.audit_protocol_version = peer.audit_protocol_version;
            }
        }
    }
    if (peer.audit_protocol_version != self->audit_protocol_version) {
        std::fprintf(stderr,
            "zetta: audit replication refused for peer %s: protocol %u, local %u; "
            "upgrade all nodes together. Catalog replication remains enabled.\n",
            peer.node_id.c_str(), peer.audit_protocol_version, self->audit_protocol_version);
    }
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
    // FramedStream owns buffering and TLS read/write serialization.
    for (;;) {
        std::optional<std::string> line = co_await link->stream->read_frame();
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
    link->stream->close();
}

capy::task<> accept_loop(Cluster::State* self, std::string my_node_id,
                         std::vector<std::string> my_endpoints) {
    net::timer retry(self->io_context);
    for (;;) {
        net::tcp_socket peer(self->io_context);
        if (auto [accept_error] = co_await self->acceptor->accept(peer); accept_error) {
            if (!self->acceptor->is_open() ||
                accept_error == std::errc::operation_canceled ||
                accept_error == std::errc::bad_file_descriptor ||
                accept_error == std::errc::invalid_argument) co_return;
            // Includes descriptor/memory pressure and transient network errors.
            // Back off so resource exhaustion cannot turn into a busy loop.
            log_event(self, "peer accept failed; retrying: " + accept_error.message());
            retry.expires_after(std::chrono::milliseconds(100));
            if (auto [wait_error] = co_await retry.wait(); wait_error) co_return;
            continue;
        }
        auto pending_setup = reserve_setup(*self);
        if (!pending_setup) {
            log_event(self, "peer setup capacity reached; refusing connection");
            peer.close();
            continue;
        }
        capy::run_async(self->io_context.get_executor())(handshake_and_hold(
            self, std::move(peer), my_node_id, my_endpoints,
            /*initiate=*/false, /*dialed_endpoint=*/std::string{},
            /*seed_key=*/std::string{}, std::move(pending_setup)));
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
    if (auto [connect_error] = co_await socket.connect(endpoint); connect_error) co_return false;
    co_await handshake_and_hold(self, std::move(socket), std::move(my_node_id),
                                std::move(my_endpoints), /*initiate=*/true,
                                std::move(dialed_label), std::move(seed_key),
                                /*pending_setup=*/nullptr);
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

// Dial a learned member (a peer we're not linked to, or a node known only
// transitively via gossip), unless a dial for it is already in flight. Deduped by
// the member's NODE ID (not a seed string): begin_dial claims the id, dial_seed's
// guard releases it on exit. register_link still reaps any duplicate that races a
// concurrent dial reaching the same node via another path (a config seed / a mutual
// dial), so links never leak (#789 stacks on the #741 registry). Network thread.
void spawn_member_dial(Cluster::State* self, const std::string& member_id,
                       const std::vector<std::string>& endpoints,
                       const std::string& my_node_id,
                       const std::vector<std::string>& my_endpoints) {
    if (endpoints.empty()) return;
    if (!self->begin_dial(member_id)) return;  // a dial for this member is in flight
    log_event(self, "dial member " + short_id(member_id));
    capy::run_async(self->io_context.get_executor())(
        dial_seed(self, endpoints, my_node_id, my_endpoints, member_id));
}

// Periodically re-dial any seed whose node we don't currently hold a live link to,
// AND maintain a link to every LEARNED member (not just config seeds), so a dropped
// link re-establishes itself and a hub-seeded star fills into a full mesh (#789).
// Both dedup by NODE ID, not by matching a seed string against advertised addresses:
// a seed that never matches (a 127.0.0.1 loopback or a load-balancer VIP vs. the
// peer's advertised address) used to redial every interval; now, once it has
// resolved to a node that's linked, it's skipped — ending the connect+handshake
// churn (#739). Runs for the cluster's life.
capy::task<> reconnect_loop(Cluster::State* self, std::vector<std::string> seeds,
                            std::string my_node_id,
                            std::vector<std::string> my_endpoints) {
    net::timer timer(self->io_context);
    for (;;) {
        timer.expires_after(self->reconnect_interval);
        [[maybe_unused]] const auto wait_result = co_await timer.wait();
        // Redial configured seeds we've lost (deduped by the node id each resolved
        // to, #739).
        for (const std::string& seed : seeds) {
            const std::string resolved = self->seed_resolved_node(seed);
            if (!resolved.empty() && self->has_live_link(resolved)) {
                continue;  // this seed's node already holds a live link — no redial
            }
            spawn_dial(self, seed, my_node_id, my_endpoints);
        }
        // Dial every learned member we don't already hold a live link to — a direct
        // peer whose link dropped, or a node we know only transitively via gossip.
        // This is what turns a star into an equal-peer mesh (#789).
        for (const auto& [member_id, endpoints] :
             self->known_member_endpoints(my_node_id)) {
            if (self->has_live_link(member_id)) continue;  // already linked — skip
            spawn_member_dial(self, member_id, endpoints, my_node_id, my_endpoints);
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
    state_->audit_protocol_version = config_.audit_protocol_version;
    state_->setup_timeout = std::max(config_.peer_setup_timeout, std::chrono::milliseconds(1));
    state_->maximum_pending_setups = std::max<std::size_t>(1, config_.maximum_pending_setups);
    state_->idle_timeout =
        std::chrono::seconds(config_.peer_idle_timeout_seconds);
    state_->reconnect_interval =
        std::chrono::seconds(config_.reconnect_interval_seconds);
    state_->membership_grace =
        std::chrono::seconds(config_.membership_grace_seconds);
    state_->log_events = config_.log_events;
    state_->dial_out_only = config_.dial_out_only;
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
        std::string reason;
        std::optional<net::tls_context> tls =
            zetta::transport::make_mutual_tls_context(config_.security, &reason);
        if (!tls) {
            std::fprintf(stderr,
                         "zetta: cluster security credentials failed to load; "
                         "refusing to start networking: %s\n",
                         reason.c_str());
            return;
        }
        state_->tls = std::move(*tls);
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
    // public API's boundary discipline) — a failure here only forfeits the fast
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
    // Keep re-dialing seeds that drop AND dial members learned via gossip, so the
    // cluster self-heals into a full equal-peer mesh (deduped by node id, #789).
    // Always run it — even a seedless node (a hub with no --peer) must dial the
    // members it learns, so a star fills into a mesh.
    capy::run_async(state_->io_context.get_executor())(
        reconnect_loop(state_.get(), config_.seeds, node_id_, my_endpoints));
    // Age peers out of membership only after the hysteresis grace window (#739).
    capy::run_async(state_->io_context.get_executor())(
        membership_loop(state_.get()));

    state_->network_thread = std::thread([this] {
        state_->network_thread_id.store(std::this_thread::get_id());
        state_->io_context.run();
    });
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

namespace {

// "zetta --peer <best address>", plus "--join-token <token>" when security is on.
std::string build_peer_command(const std::vector<std::string>& addresses,
                               const ClusterSecurity& security,
                               std::string_view token_text) {
    std::string command = "zetta --peer " + addresses.front();
    if (security.enabled()) {
        command += " --join-token ";
        command += token_text;
    }
    return command;
}

}  // namespace

std::string Cluster::peer_command() const {
    // Prefer the first reachable address — local_ipv4_addresses() already puts
    // non-loopback (LAN) addresses ahead of 127.0.0.1. The join token is what the
    // next node needs, but it is the cluster root secret, and this command is
    // printed on every headless start (#1393): name where it goes, never the value.
    return build_peer_command(listen_addresses(), config_.security,
                              kJoinTokenPlaceholder);
}

std::string Cluster::peer_command_with_join_token() const {
    return build_peer_command(listen_addresses(), config_.security,
                              config_.security.join_token);
}

std::vector<std::string> Cluster::advertised_endpoints() const {
    // A dial-out-only node advertises NO dialable candidate (#803): it is a pure
    // initiator, reached only over the outbound link it opens, so it must never
    // gossip its (unroutable, cross-cluster-colliding) bound/private IP.
    if (config_.dial_out_only) return {};
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
    // Peers refuse a HELLO with more than kMaxAdvertisedEndpoints (#771), so the
    // set is capped here, in order. Every --advertise extra is kept (main.cpp
    // refuses more than the cap); a wildcard bind on a host with many interfaces
    // gives up its last listen addresses to make room for them.
    if (endpoints.size() <= kMaxAdvertisedEndpoints) return endpoints;
    const auto is_explicit = [&](const std::string& endpoint) {
        return std::find(config_.advertise.begin(), config_.advertise.end(),
                         endpoint) != config_.advertise.end();
    };
    const std::size_t explicit_count = static_cast<std::size_t>(
        std::ranges::count_if(endpoints, is_explicit));
    const std::size_t explicit_kept =
        std::min(explicit_count, kMaxAdvertisedEndpoints);
    const std::size_t listen_budget = kMaxAdvertisedEndpoints - explicit_kept;
    std::vector<std::string> capped;
    std::size_t listen_kept = 0;
    std::size_t extras_kept = 0;
    for (std::string& endpoint : endpoints) {
        if (is_explicit(endpoint)) {
            if (extras_kept == explicit_kept) continue;
            ++extras_kept;
        } else {
            if (listen_kept == listen_budget) continue;
            ++listen_kept;
        }
        capped.push_back(std::move(endpoint));
    }
    return capped;
}

bool Cluster::dial_out_only() const { return config_.dial_out_only; }

std::string Cluster::node_type() const { return config_.node_type; }

std::string Cluster::primary_advertised_address() const {
    // The single most-dialable address recorded in the replicated node-list for
    // transitive learners (#803/#800): empty for a dial-out-only node; else the
    // operator-supplied --advertise front (a routable LB / public address) when
    // set, falling back to the bound listen address (a LAN node dialable at its
    // own IP).
    if (config_.dial_out_only) return {};
    if (!config_.advertise.empty()) return config_.advertise.front();
    return listen_addresses().front();
}

std::vector<ClusterPeer> Cluster::peers() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<ClusterPeer> result;
    result.reserve(state_->peers.size());
    for (const State::PeerRecord& peer : state_->peers) {
        if (!peer.connected) continue;  // live-link view; hysteresis-only excluded
        result.push_back(ClusterPeer{.node_id = peer.node_id,
                                     .address = peer.address,
                                     .endpoints = peer.endpoints,
                                     .dial_out_only = peer.dial_out_only,
                                     .audit_protocol_version = peer.audit_protocol_version});
    }
    return result;
}

std::vector<ClusterMember> Cluster::membership() const {
    // The complete membership from the replicated node-list (#800): every node:<id>
    // entry, annotated with reachability and whether we hold a live link to it now.
    // On a spanning cluster no single node is directly linked to all N, but the CRDT
    // node-list converges everywhere — so this is the full view from any node.
    std::vector<std::string> connected;  // node_ids we hold a live link to
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        for (const State::PeerRecord& peer : state_->peers) {
            if (peer.connected) connected.push_back(peer.node_id);
        }
    }
    std::vector<ClusterMember> members;
    if (!state_->replication) return members;
    const nlohmann::json document = state_->replication->snapshot();
    if (!document.is_object()) return members;
    for (const auto& [key, value] : document.items()) {
        if (!key.starts_with(zetta::catalog::kNodePrefix)) continue;
        if (!value.is_object()) continue;
        ClusterMember member;
        member.node_id = key.substr(zetta::catalog::kNodePrefix.size());
        if (const auto address = value.find("address");
            address != value.end() && address->is_string()) {
            member.address = address->get<std::string>();
        }
        if (const auto reach = value.find("reachability");
            reach != value.end() && reach->is_string()) {
            member.dial_out_only = reach->get<std::string>() == "dial-out-only";
        }
        if (const auto type = value.find("node_type");
            type != value.end() && type->is_string()) {
            member.node_type = type->get<std::string>();
        }
        member.is_self = member.node_id == node_id_;
        member.directly_connected =
            member.is_self ||
            std::find(connected.begin(), connected.end(), member.node_id) !=
                connected.end();
        members.push_back(std::move(member));
    }
    std::sort(members.begin(), members.end(),
              [](const ClusterMember& a, const ClusterMember& b) {
                  return a.node_id < b.node_id;
              });
    return members;
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

GossipStats Cluster::gossip_stats() const {
    constexpr auto order = std::memory_order_relaxed;
    return GossipStats{
        .rounds = state_->gossip_rounds.load(order),
        .sync_frames = state_->gossip_sync_frames.load(order),
        .full_syncs = state_->gossip_full_syncs.load(order),
        .incremental_syncs = state_->gossip_incremental_syncs.load(order),
        .sync_bytes = state_->gossip_sync_bytes.load(order),
    };
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
    self.audit_protocol_version = state_->audit_protocol_version;
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
        node.audit_protocol_version = peer.audit_protocol_version;
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

void Cluster::publish_audit_watermarks(
    const std::map<std::string, zetta::governance::AuditWatermark>& watermarks) {
    std::string status = "ZETTA/1 AUSTATUS";
    for (const auto& [origin, watermark] : watermarks) {
        if (origin != "legacy" && zetta::governance::valid_audit_origin(origin)) {
            status += " " + origin + " " + std::to_string(watermark.high) +
                      " " + std::to_string(watermark.low) +
                      " " + std::to_string(watermark.summary_low);
        }
    }
    const std::lock_guard lock(state_->audit_mutex);
    state_->audit_status = std::move(status);
}

std::vector<AuditMessage> Cluster::take_audit_messages() {
    const std::lock_guard lock(state_->audit_mutex);
    state_->audit_message_bytes = 0;
    return std::exchange(state_->audit_messages, {});
}

bool Cluster::send_audit(const AuditMessage& message) {
    bool compatible = false;
    for (const auto& node : nodes()) {
        if (node.node_id == message.peer && node.connected) {
            compatible = node.audit_protocol_version == state_->audit_protocol_version &&
                state_->audit_protocol_version == zetta::governance::kAuditProtocolVersion;
        }
    }
    if (!compatible) {
        return false;
    }
    const auto& batch = message.batch;
    if (!zetta::governance::valid_audit_origin(batch.origin) || batch.origin == "legacy" ||
        message.kind == AuditMessage::Kind::Status) {
        return false;
    }
    std::string line = "ZETTA/1 ";
    line += message.kind == AuditMessage::Kind::Request ? "AUREQ " : "AURES ";
    line += batch.origin + " " + std::to_string(batch.after);
    if (message.kind == AuditMessage::Kind::Response) {
        line += " " + std::to_string(batch.next) + " " + std::to_string(batch.watermark.high) +
                " " + std::to_string(batch.watermark.low) + " " +
                std::to_string(batch.watermark.summary_low) + " " +
                (batch.frames.empty() ? "-" : detail::base64_encode_bytes(batch.frames));
    }
    const std::lock_guard lock(state_->outbox_mutex);
    auto& queue = state_->outbox[message.peer];
    std::size_t bytes = 0;
    for (const auto& queued : queue) {
        bytes += queued.size();
    }
    if (bytes + line.size() > 24 * 1024 * 1024) {
        return false;
    }
    queue.push_back(std::move(line));
    return true;
}

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
    state_->signal_handler.store(handler, std::memory_order_seq_cst);
    // Wait out any dispatch that may have loaded the previous handler (#1131),
    // so the caller may destroy it once this returns. On the network thread
    // itself (a handler swapping handlers inside a dispatch) the wait would
    // count its own dispatch, so it is skipped there.
    if (std::this_thread::get_id() == state_->network_thread_id.load()) return;
    while (state_->dispatching.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
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

std::vector<SignalResponse> Cluster::request_each(
    const std::vector<std::string>& node_ids, const std::string& method,
    const std::string& payload, std::chrono::milliseconds timeout) {
    struct Pending {
        std::string id;
        std::future<SignalResponse> future;
    };
    std::vector<SignalResponse> responses(node_ids.size());
    std::vector<std::optional<Pending>> pending(node_ids.size());
    for (std::size_t index = 0; index < node_ids.size(); ++index) {
        if (!is_connected(*state_, node_ids[index])) {
            responses[index] = SignalResponse{.ok = false, .error = "peer not connected"};
            continue;
        }
        Signal request;
        request.kind = SignalKind::Request;
        request.id = zetta::util::new_uuid();
        request.method = method;
        request.payload = payload;
        // Register the waiter BEFORE queuing, as request() does.
        std::future<SignalResponse> future =
            state_->add_pending(request.id, node_ids[index]);
        state_->queue_outbound(node_ids[index], signal_line(request));
        pending[index] = Pending{request.id, std::move(future)};
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (std::size_t index = 0; index < node_ids.size(); ++index) {
        if (!pending[index]) continue;
        Pending& waiting = *pending[index];
        if (waiting.future.wait_until(deadline) == std::future_status::ready) {
            responses[index] = waiting.future.get();
        } else if (state_->take_pending(waiting.id)) {
            responses[index] = SignalResponse{.ok = false, .error = "request timed out"};
        } else {
            responses[index] = waiting.future.get();
        }
    }
    return responses;
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
