#include "zetta/cluster.hpp"

#include <charconv>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy.hpp>

#include "zetta/runtime/net.hpp"

// src/core/cluster.cpp is the networking adapter: the one .cpp allowed to use
// the Corosio backend (via zetta::runtime::net). Cluster's public header
// exposes no backend types, so the rest of zetta stays decoupled.
namespace zetta {

namespace capy = boost::capy;
namespace net = zetta::runtime::net;

struct Cluster::State {
    net::io_context io_context;
    std::optional<net::tcp_acceptor> acceptor;
    std::thread network_thread;
    std::uint16_t bound_port = 0;
    mutable std::mutex mutex;
    std::vector<ClusterPeer> peers;

    // Add a peer unless one with the same node_id is already present. Returns
    // whether it was newly added (so the matching disconnect removes it once).
    bool add_peer(ClusterPeer peer) {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const ClusterPeer& existing : peers) {
            if (existing.node_id == peer.node_id) return false;
        }
        peers.push_back(std::move(peer));
        return true;
    }
    void remove_peer(const std::string& node_id) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(peers,
                      [&](const ClusterPeer& p) { return p.node_id == node_id; });
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

// Read one '\n'-terminated line (without the newline). Empty optional on
// EOF/error.
capy::task<std::optional<std::string>> read_line(net::tcp_socket& socket) {
    std::string accumulated;
    char buffer[512];
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
        if (accumulated.size() > 8192) co_return accumulated;  // sanity bound
    }
}

capy::task<> write_line(net::tcp_socket& socket, std::string line) {
    line.push_back('\n');
    auto [ec, written] =
        co_await net::write(socket, net::const_buffer(line.data(), line.size()));
    (void)ec;
    (void)written;
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
    const bool added = self->add_peer(peer);
    for (;;) {
        if (!co_await read_line(socket)) break;  // peer disconnected
    }
    if (added) self->remove_peer(peer.node_id);
    socket.close();
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

}  // namespace

Cluster::Cluster(std::string node_id, ClusterConfig config)
    : state_(std::make_unique<State>()),
      node_id_(std::move(node_id)),
      config_(std::move(config)),
      role_(ClusterRole::Follower) {}

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

    const std::string my_listen = listen_address();
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

std::string Cluster::peer_command() const {
    return "zetta --peer " + listen_address();
}

std::vector<ClusterPeer> Cluster::peers() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->peers;
}

std::size_t Cluster::peer_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->peers.size();
}

}  // namespace zetta
