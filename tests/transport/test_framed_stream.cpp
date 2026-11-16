// SPDX-License-Identifier: MIT

// FramedStream over mutual TLS keeps both directions moving at once (#835).
//
// The cluster runs one read pump and one write pump per link. Both peers write
// their whole replicated document every gossip round, so on a busy cluster both
// ends of a link are often writing a frame larger than the socket buffers at
// the same moment. These tests reproduce that on one loopback connection and
// require every frame to arrive in both directions before a deadline.

#include <gtest/gtest.h>

#include <atomic>
#include <boost/capy.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "zetta/cluster/security.hpp"
#include "zetta/runtime/net.hpp"
#include "zetta/transport/framed_stream.hpp"
#include "zetta/transport/security.hpp"

namespace {

namespace capy = boost::capy;
namespace net = zetta::runtime::net;
using zetta::transport::FramedStream;
using zetta::transport::HandshakeRole;

net::tls_context make_context(const std::string& token, const std::string& node_id) {
    std::string reason;
    std::optional<zetta::cluster::ClusterSecurity> security =
        zetta::cluster::derive_cluster_security(token, node_id, &reason);
    EXPECT_TRUE(security) << reason;
    std::optional<net::tls_context> context =
        zetta::transport::make_mutual_tls_context(security.value(), &reason);
    EXPECT_TRUE(context) << reason;
    return std::move(context.value());
}

// One end of the connection: what it sends, and what it has received so far.
struct Endpoint {
    std::shared_ptr<FramedStream> stream;
    std::atomic<int> frames_received{0};
    std::atomic<int> frames_sent{0};
    std::atomic<bool> corrupt{false};
    std::atomic<bool> failed{false};
};

// The frame a side sends as its `index`th message: recognisable per side and per
// index, so a reordered, truncated or interleaved frame is detected.
std::string frame_for(char side, int index, std::size_t size) {
    std::string frame(size, side);
    const std::string tag = std::to_string(index) + ":";
    frame.replace(0, tag.size(), tag);
    return frame;
}

capy::task<> write_all(Endpoint& endpoint, char side, int count, std::size_t size) {
    for (int index = 0; index < count; ++index) {
        if (!co_await endpoint.stream->write_frame(frame_for(side, index, size))) {
            endpoint.failed = true;
            co_return;
        }
        endpoint.frames_sent.fetch_add(1);
    }
}

capy::task<> read_all(Endpoint& endpoint, char peer_side, int count, std::size_t size) {
    for (int index = 0; index < count; ++index) {
        std::optional<std::string> frame = co_await endpoint.stream->read_frame();
        if (!frame) {
            endpoint.failed = true;
            co_return;
        }
        if (*frame != frame_for(peer_side, index, size)) endpoint.corrupt = true;
        endpoint.frames_received.fetch_add(1);
    }
}

// Both ends run one reader and one writer concurrently, as a cluster link does.
// A side that cannot make progress shows up as a missed deadline; the watchdog
// then closes both sockets so every pending operation completes and the
// io_context drains before the test returns.
struct DuplexRun {
    net::io_context io_context;
    Endpoint client;
    Endpoint server;
    std::atomic<int> handshakes{0};
    std::atomic<bool> watchdog_fired{false};
};

capy::task<> serve(DuplexRun& run, net::tcp_acceptor& acceptor, net::tls_context context,
                   int count, std::size_t size) {
    net::tcp_socket socket(run.io_context);
    if (auto [error] = co_await acceptor.accept(socket); error) {
        run.server.failed = true;
        co_return;
    }
    run.server.stream = std::make_shared<FramedStream>(std::move(socket), std::move(context));
    if (co_await run.server.stream->handshake(HandshakeRole::Server)) {
        run.server.failed = true;
        co_return;
    }
    run.handshakes.fetch_add(1);
    capy::run_async(run.io_context.get_executor())(write_all(run.server, 's', count, size));
    co_await read_all(run.server, 'c', count, size);
}

capy::task<> dial(DuplexRun& run, net::endpoint target, net::tls_context context, int count,
                  std::size_t size) {
    net::tcp_socket socket(run.io_context);
    socket.open();
    if (auto [error] = co_await socket.connect(target); error) {
        run.client.failed = true;
        co_return;
    }
    run.client.stream = std::make_shared<FramedStream>(std::move(socket), std::move(context));
    if (co_await run.client.stream->handshake(HandshakeRole::Client)) {
        run.client.failed = true;
        co_return;
    }
    run.handshakes.fetch_add(1);
    capy::run_async(run.io_context.get_executor())(write_all(run.client, 'c', count, size));
    co_await read_all(run.client, 's', count, size);
}

capy::task<> watchdog(DuplexRun& run, int count, std::chrono::seconds deadline) {
    net::timer timer(run.io_context);
    const auto started = std::chrono::steady_clock::now();
    while (run.client.frames_received.load() < count ||
           run.server.frames_received.load() < count) {
        if (run.client.failed || run.server.failed) break;
        if (std::chrono::steady_clock::now() - started > deadline) {
            run.watchdog_fired = true;
            break;
        }
        timer.expires_after(std::chrono::milliseconds(20));
        [[maybe_unused]] const auto waited = co_await timer.wait();
    }
    if (run.client.stream) run.client.stream->close();
    if (run.server.stream) run.server.stream->close();
}

struct Outcome {
    int client_received = 0;
    int server_received = 0;
    bool corrupt = false;
    bool failed = false;
    bool deadline_missed = false;
};

Outcome run_duplex(std::uint16_t port, int count, std::size_t size, std::chrono::seconds deadline) {
    const std::string token = zetta::cluster::generate_join_token();
    DuplexRun run;
    net::tcp_acceptor acceptor(run.io_context);
    acceptor.open();
    acceptor.set_option(net::reuse_address(true));
    const net::endpoint target(net::ipv4_address::loopback(), port);
    EXPECT_FALSE(acceptor.bind(target));
    EXPECT_FALSE(acceptor.listen());

    capy::run_async(run.io_context.get_executor())(
        serve(run, acceptor, make_context(token, "framed-server"), count, size));
    capy::run_async(run.io_context.get_executor())(
        dial(run, target, make_context(token, "framed-client"), count, size));
    capy::run_async(run.io_context.get_executor())(watchdog(run, count, deadline));
    run.io_context.run();

    return Outcome{.client_received = run.client.frames_received.load(),
                   .server_received = run.server.frames_received.load(),
                   .corrupt = run.client.corrupt || run.server.corrupt,
                   .failed = run.client.failed || run.server.failed,
                   .deadline_missed = run.watchdog_fired.load()};
}

}  // namespace

// Each side writes one frame far larger than the loopback socket buffers (even
// after macOS autotuning) while its peer does the same. Neither write can finish
// until the other side reads, so a stream that blocks its read behind its own
// unfinished write never delivers either frame.
TEST(FramedStream, TlsPeersWritingFramesLargerThanTheSocketBuffersAtOnceBothDeliver) {
    constexpr int kFrames = 2;
    constexpr std::size_t kFrameBytes = 16 * 1024 * 1024;
    const Outcome outcome = run_duplex(42470, kFrames, kFrameBytes, std::chrono::seconds(60));
    EXPECT_FALSE(outcome.deadline_missed)
        << "TLS stream stalled with both peers writing: client received "
        << outcome.client_received << "/" << kFrames << ", server received "
        << outcome.server_received << "/" << kFrames;
    EXPECT_FALSE(outcome.failed);
    EXPECT_FALSE(outcome.corrupt);
    EXPECT_EQ(outcome.client_received, kFrames);
    EXPECT_EQ(outcome.server_received, kFrames);
}

// The cluster's steady state: a stream of gossip-sized frames (a whole document,
// a few TLS records each) flowing both ways at once. Every frame must arrive
// intact and in order, with no reader left waiting on bytes already received.
TEST(FramedStream, TlsPeersExchangingManyGossipSizedFramesBothDeliverInOrder) {
    constexpr int kFrames = 400;
    constexpr std::size_t kFrameBytes = 150 * 1024;
    const Outcome outcome = run_duplex(42471, kFrames, kFrameBytes, std::chrono::seconds(60));
    EXPECT_FALSE(outcome.deadline_missed)
        << "TLS stream stalled: client received " << outcome.client_received << "/" << kFrames
        << ", server received " << outcome.server_received << "/" << kFrames;
    EXPECT_FALSE(outcome.failed);
    EXPECT_FALSE(outcome.corrupt);
    EXPECT_EQ(outcome.client_received, kFrames);
    EXPECT_EQ(outcome.server_received, kFrames);
}
