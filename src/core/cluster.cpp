#include "zetta/cluster.hpp"

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
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy.hpp>

#include "zetta/blob_store.hpp"
#include "zetta/replication.hpp"
#include "zetta/runtime/net.hpp"

// src/core/cluster.cpp is the networking adapter: the one .cpp allowed to use
// the Corosio backend (via zetta::runtime::net). Cluster's public header
// exposes no backend types, so the rest of zetta stays decoupled.
namespace zetta {

namespace capy = boost::capy;
namespace net = zetta::runtime::net;

struct Cluster::State {
    // A connected peer plus the replication status it last reported over gossip.
    struct PeerRecord {
        std::string node_id;
        std::string address;
        bool has_status = false;  // have we received a STATUS from it yet?
        std::size_t entries = 0;
        std::string version;
        std::chrono::steady_clock::time_point last_sync{};
    };

    net::io_context io_context;
    std::optional<net::tcp_acceptor> acceptor;
    std::thread network_thread;
    std::uint16_t bound_port = 0;
    ReplicationManager* replication = nullptr;  // gossip target (optional)
    BlobStore* blobs = nullptr;                 // large-object body transfer (optional)
    std::chrono::steady_clock::duration idle_timeout{};
    std::chrono::steady_clock::duration reconnect_interval{};
    mutable std::mutex mutex;
    std::vector<PeerRecord> peers;

    // Record a peer. One entry per listen address: a node that restarts reuses
    // its address with a fresh node id, so a new HELLO from a known address
    // evicts the stale entry (and any same-id entry from a prior connection).
    void add_peer(std::string node_id, std::string address) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(peers, [&](const PeerRecord& p) {
            return p.node_id == node_id || p.address == address;
        });
        peers.push_back(PeerRecord{.node_id = std::move(node_id),
                                   .address = std::move(address)});
    }
    void remove_peer(const std::string& node_id) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(peers,
                      [&](const PeerRecord& p) { return p.node_id == node_id; });
    }
    // Listen addresses of currently-connected peers (for the reconnect loop).
    std::vector<std::string> connected_addresses() const {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> addresses;
        addresses.reserve(peers.size());
        for (const PeerRecord& peer : peers) addresses.push_back(peer.address);
        return addresses;
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

bool parse_host_port(const std::string& text, net::ipv4_address& address,
                     std::uint16_t& port) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    if (net::parse_ipv4_address(text.substr(0, colon), address)) return false;
    const std::string port_text = text.substr(colon + 1);
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), value);
    if (ec != std::errc{} || value == 0 || value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

// Cap on a single line, generous enough for a gossiped (base64) document blob or
// a transferred large-object body, but bounded so a misbehaving peer can't make
// us buffer without limit. base64 inflates ~4/3, so this admits ~48 MiB bodies;
// larger objects would need chunked transfer (a future step).
constexpr std::size_t kMaxLineBytes = 64 * 1024 * 1024;

// Read one '\n'-terminated line (without the newline). Empty optional on
// EOF/error or if the line exceeds kMaxLineBytes.
capy::task<std::optional<std::string>> read_line(net::tcp_socket& socket) {
    std::string accumulated;
    char buffer[4096];
    for (;;) {
        auto [ec, n] = co_await socket.read_some(
            net::mutable_buffer(buffer, sizeof buffer));
        if (ec || n == 0) co_return std::nullopt;
        accumulated.append(buffer, n);
        if (const auto newline = accumulated.find('\n');
            newline != std::string::npos) {
            accumulated.resize(newline);
            co_return accumulated;
        }
        if (accumulated.size() > kMaxLineBytes) co_return std::nullopt;
    }
}

// Returns whether the whole line was written (false on a closed/broken socket).
capy::task<bool> write_line(net::tcp_socket& socket, std::string line) {
    line.push_back('\n');
    auto [ec, written] =
        co_await net::write(socket, net::const_buffer(line.data(), line.size()));
    co_return !ec && written == line.size();
}

// Parse "ZETTA/1 HELLO <node_id> <listen_address>". Returns an empty node_id on
// anything malformed.
ClusterPeer parse_hello(const std::string& line) {
    std::istringstream parser(line);
    std::string protocol, verb;
    ClusterPeer peer;
    parser >> protocol >> verb >> peer.node_id >> peer.address;
    if (protocol != kProtocol || verb != "HELLO") return ClusterPeer{};
    return peer;
}

std::string hello_line(const std::string& node_id,
                       const std::string& listen_address) {
    return std::string(kProtocol) + " HELLO " + node_id + " " + listen_address;
}

// --- Replication gossip -----------------------------------------------------
//
// Once peers have shaken hands they exchange replicated state on a timer:
//   ZETTA/1 SYNC <base64 of ReplicationManager::save()>   — merge into ours
//   ZETTA/1 STATUS <entries> <version>                    — its status, for nodes()
// SYNC carries the whole document (the ReplicateToAllNodes policy); the CRDT
// merge is conflict-free, so order and duplication don't matter.

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const std::byte> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 3 <= data.size()) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8) |
            static_cast<std::uint32_t>(data[index + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3f]);
        out.push_back(kBase64Alphabet[triple & 0x3f]);
        index += 3;
    }
    if (const std::size_t remaining = data.size() - index; remaining > 0) {
        std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16;
        if (remaining == 2) {
            triple |= static_cast<std::uint32_t>(data[index + 1]) << 8;
        }
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(remaining == 2 ? kBase64Alphabet[(triple >> 6) & 0x3f]
                                     : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<std::byte> base64_decode(std::string_view text) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;  // '=' padding or invalid
    };
    std::vector<std::byte> out;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        const int value = value_of(c);
        if (value < 0) continue;  // skip padding / whitespace
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((accumulator >> bits) & 0xff));
        }
    }
    return out;
}

std::string base64_encode_bytes(const std::string& data) {
    return base64_encode(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

std::string base64_decode_to_string(std::string_view text) {
    const std::vector<std::byte> bytes = base64_decode(text);
    std::string out(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i] = static_cast<char>(bytes[i]);
    }
    return out;
}

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

// Per-connection state shared by the read pump and the write pump. Both run on
// the single io_context thread (cooperative, never truly concurrent), so the
// queue needs no lock. The reader queues large-object responses (LORES) here;
// the writer drains them alongside its periodic gossip.
struct GossipLink {
    std::shared_ptr<net::tcp_socket> socket;
    bool alive = true;
    std::deque<std::string> outbound;
    std::chrono::steady_clock::time_point last_activity{};  // last line received
};

// Apply one gossip line to our state. `peer_node_id` is who sent it; replies
// (LORES for a LOREQ) are queued on `link` for the writer to send.
void apply_gossip(Cluster::State* self, const std::string& line,
                  const std::string& peer_node_id, GossipLink& link) {
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
        self->update_peer_status(peer_node_id, entries, std::move(version),
                                 std::chrono::steady_clock::now());
    } else if (verb == "LOREQ") {
        std::string id;
        parser >> id;
        if (self->blobs && !id.empty()) {
            if (auto body = self->blobs->read_blob(id)) {
                link.outbound.push_back(lores_line(id, *body));
            }
        }
    } else if (verb == "LORES") {
        std::string id, encoded;
        parser >> id >> encoded;
        if (self->blobs && !id.empty() && !encoded.empty()) {
            self->blobs->store_blob(id, base64_decode_to_string(encoded));
        }
    }
}

// Periodically push our replicated document + status to a peer, drain queued
// large-object responses, and request the bodies we're missing — until the link
// dies. Runs alongside the read pump (one outstanding write, one read — the
// full-duplex pattern a socket allows).
capy::task<> gossip_writer(Cluster::State* self,
                           std::shared_ptr<GossipLink> link) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    net::timer timer(self->io_context);
    std::map<std::string, Clock::time_point> requested;  // body request cooldown
    while (link->alive) {
        // Keepalive: a healthy peer gossips to us every ~0.5s, so prolonged
        // silence means the link died (e.g. a silent TCP drop). Close it —
        // that unblocks the read pump, which removes the peer.
        if (Clock::now() - link->last_activity > self->idle_timeout) {
            link->socket->close();
            co_return;
        }
        // Send any large-object bodies a peer asked us for.
        while (!link->outbound.empty()) {
            std::string line = std::move(link->outbound.front());
            link->outbound.pop_front();
            if (!co_await write_line(*link->socket, std::move(line))) {
                co_return;
            }
        }
        if (self->replication) {
            if (!co_await write_line(*link->socket,
                                     sync_line(*self->replication)) ||
                !co_await write_line(*link->socket,
                                     status_line(*self->replication))) {
                co_return;
            }
        }
        // Request bodies we lack, but don't re-ask the same peer too often (a
        // transfer in flight shouldn't trigger duplicate sends).
        if (self->blobs) {
            const Clock::time_point now = Clock::now();
            for (const std::string& id : self->blobs->missing_blobs()) {
                const auto it = requested.find(id);
                if (it != requested.end() && now - it->second < 2s) continue;
                requested[id] = now;
                if (!co_await write_line(*link->socket, loreq_line(id))) {
                    co_return;
                }
            }
        }
        timer.expires_after(500ms);
        [[maybe_unused]] const auto wait_result = co_await timer.wait();
    }
}

// Run the symmetric handshake over an established connection and stay joined
// until the connection drops. `initiate` controls who speaks first (the dialer
// sends HELLO first; the acceptor replies) — the recorded result is identical
// either way.
capy::task<> handshake_and_hold(Cluster::State* self, net::tcp_socket socket,
                                std::string my_node_id,
                                std::string my_listen_address, bool initiate) {
    std::optional<std::string> their_hello;
    if (initiate) {
        co_await write_line(socket, hello_line(my_node_id, my_listen_address));
        their_hello = co_await read_line(socket);
    } else {
        their_hello = co_await read_line(socket);
        co_await write_line(socket, hello_line(my_node_id, my_listen_address));
    }
    if (!their_hello) {
        socket.close();
        co_return;
    }
    const ClusterPeer peer = parse_hello(*their_hello);
    if (peer.node_id.empty() || peer.node_id == my_node_id) {
        socket.close();  // malformed, or we dialed ourselves
        co_return;
    }
    self->add_peer(peer.node_id, peer.address);

    // Per-connection link shared between this read pump and the gossip write
    // pump (both on the io thread). The writer handles periodic gossip, queued
    // large-object responses, body requests, and the keepalive timeout.
    auto link = std::make_shared<GossipLink>();
    link->socket = std::make_shared<net::tcp_socket>(std::move(socket));
    link->last_activity = std::chrono::steady_clock::now();
    if (self->replication || self->blobs) {
        capy::run_async(self->io_context.get_executor())(
            gossip_writer(self, link));
    }

    // Read pump: merge gossiped state, record peer status, serve/receive
    // large-object bodies, until disconnect (or the writer's keepalive closes
    // the socket, which surfaces here as a read error).
    for (;;) {
        std::optional<std::string> line = co_await read_line(*link->socket);
        if (!line) break;  // peer disconnected / link timed out
        link->last_activity = std::chrono::steady_clock::now();
        apply_gossip(self, *line, peer.node_id, *link);
    }

    link->alive = false;
    self->remove_peer(peer.node_id);
    link->socket->close();
}

capy::task<> accept_loop(Cluster::State* self, std::string my_node_id,
                         std::string my_listen_address) {
    for (;;) {
        net::tcp_socket peer(self->io_context);
        if (auto [ec] = co_await self->acceptor->accept(peer); ec) co_return;
        capy::run_async(self->io_context.get_executor())(handshake_and_hold(
            self, std::move(peer), my_node_id, my_listen_address,
            /*initiate=*/false));
    }
}

capy::task<> dial_seed(Cluster::State* self, net::ipv4_address address,
                       std::uint16_t port, std::string my_node_id,
                       std::string my_listen_address) {
    net::tcp_socket socket(self->io_context);
    socket.open();
    if (auto [ec] = co_await socket.connect(net::endpoint(address, port)); ec) {
        co_return;
    }
    co_await handshake_and_hold(self, std::move(socket), my_node_id,
                                my_listen_address, /*initiate=*/true);
}

// Periodically re-dial any seed we're not currently connected to, so a dropped
// link re-establishes itself (the keepalive on the other side clears the stale
// entry; this side reconnects). Runs for the cluster's lifetime.
capy::task<> reconnect_loop(Cluster::State* self, std::vector<std::string> seeds,
                            std::string my_node_id,
                            std::string my_listen_address) {
    net::timer timer(self->io_context);
    for (;;) {
        timer.expires_after(self->reconnect_interval);
        [[maybe_unused]] const auto wait_result = co_await timer.wait();
        const std::vector<std::string> connected = self->connected_addresses();
        for (const std::string& seed : seeds) {
            if (std::find(connected.begin(), connected.end(), seed) !=
                connected.end()) {
                continue;  // already connected to this seed
            }
            net::ipv4_address address;
            std::uint16_t port = 0;
            if (parse_host_port(seed, address, port)) {
                capy::run_async(self->io_context.get_executor())(dial_seed(
                    self, address, port, my_node_id, my_listen_address));
            }
        }
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
                 ReplicationManager* replication, BlobStore* blobs)
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
}

Cluster::~Cluster() { stop(); }

void Cluster::start() {
    net::ipv4_address host;
    if (net::parse_ipv4_address(config_.listen_host, host)) return;  // bad host

    // Bind the first free port in the range, synchronously, so listen_address()
    // is accurate before the network thread starts. Use the error-code bind/
    // listen (not the throwing ctor) — "port in use" is expected while probing.
    state_->acceptor.emplace(state_->io_context);
    state_->acceptor->open();  // tcp::v4()
    for (std::uint16_t offset = 0; offset < config_.port_range; ++offset) {
        const std::uint16_t port = config_.listen_port + offset;
        if (!state_->acceptor->bind(net::endpoint(host, port)) &&
            !state_->acceptor->listen()) {
            state_->bound_port = port;
            break;
        }
    }
    if (state_->bound_port == 0) state_->acceptor.reset();  // nothing bound

    // Advertise a *reachable* address in the handshake — never the bare wildcard
    // (0.0.0.0), which a peer can't dial. listen_addresses() puts the best
    // (non-loopback) one first.
    const std::string my_listen = listen_addresses().front();
    if (state_->bound_port != 0) {
        capy::run_async(state_->io_context.get_executor())(
            accept_loop(state_.get(), node_id_, my_listen));
    }
    for (const std::string& seed : config_.seeds) {
        net::ipv4_address address;
        std::uint16_t port = 0;
        if (parse_host_port(seed, address, port)) {
            capy::run_async(state_->io_context.get_executor())(
                dial_seed(state_.get(), address, port, node_id_, my_listen));
        }
    }
    // Keep re-dialing seeds that drop, so the cluster self-heals.
    if (!config_.seeds.empty()) {
        capy::run_async(state_->io_context.get_executor())(
            reconnect_loop(state_.get(), config_.seeds, node_id_, my_listen));
    }

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
    // non-loopback (LAN) addresses ahead of 127.0.0.1.
    const std::vector<std::string> addresses = listen_addresses();
    return "zetta --peer " + addresses.front();
}

std::vector<ClusterPeer> Cluster::peers() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<ClusterPeer> result;
    result.reserve(state_->peers.size());
    for (const State::PeerRecord& peer : state_->peers) {
        result.push_back(ClusterPeer{.node_id = peer.node_id,
                                     .address = peer.address});
    }
    return result;
}

std::size_t Cluster::peer_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->peers.size();
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
        NodeStatus node;
        node.node_id = peer.node_id;
        node.address = peer.address;
        node.role = ClusterRole::Follower;
        node.is_self = false;
        node.connected = true;
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

}  // namespace zetta
