// SPDX-License-Identifier: MIT

// Zetta-to-Zetta peer-gateway acceptance (#1282): two nodes whose private
// endpoints overlap form one cluster through opaque TCP forwarders. Their
// node-to-node mutual TLS terminates only at the Zetta nodes, and CRDT gossip
// converges in both directions. See docs/network/peer-gateway-links.md.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterSecurity;
using zetta::cluster::ReplicationManager;

class TcpForwarder {
public:
  TcpForwarder(std::uint16_t listen_port, std::uint16_t target_port)
      : target_port_(target_port) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
      error_ = "socket";
      return;
    }

    const int enabled = 1;
    if (::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof enabled) != 0) {
      error_ = "setsockopt";
      close_listener();
      return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(listen_port);
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof address) != 0) {
      error_ = "bind";
      close_listener();
      return;
    }
    if (::listen(listener_, 4) != 0) {
      error_ = "listen";
      close_listener();
      return;
    }

    const int listener = listener_;
    worker_ = std::thread([this, listener] { run(listener); });
  }

  ~TcpForwarder() {
    stopping_.store(true);
    close_listener();
    shutdown_active_session();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  TcpForwarder(const TcpForwarder &) = delete;
  TcpForwarder &operator=(const TcpForwarder &) = delete;

  bool ready() const { return listener_ >= 0 && error_.empty(); }

  const std::string &error() const { return error_; }

private:
  static void forward_bytes(int source, int destination) {
    std::array<std::byte, 16 * 1024> buffer{};
    for (;;) {
      const ssize_t received = ::recv(source, buffer.data(), buffer.size(), 0);
      if (received == 0) {
        break;
      }
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      ssize_t sent = 0;
      while (sent < received) {
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const ssize_t count =
            ::send(destination, buffer.data() + sent,
                   static_cast<std::size_t>(received - sent), send_flags);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          ::shutdown(destination, SHUT_RDWR);
          return;
        }
        sent += count;
      }
    }
    ::shutdown(destination, SHUT_WR);
  }

  int connect_target() const {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
      return -1;
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(target_port_);
    if (::connect(socket, reinterpret_cast<sockaddr *>(&address),
                  sizeof address) != 0) {
      ::close(socket);
      return -1;
    }
    return socket;
  }

  void run(int listener) {
    while (!stopping_.load()) {
      const int inbound = ::accept(listener, nullptr, nullptr);
      if (inbound < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
#ifdef SO_NOSIGPIPE
      const int enabled = 1;
      ::setsockopt(inbound, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif

      const int outbound = connect_target();
      if (outbound < 0) {
        ::close(inbound);
        continue;
      }
      {
        const std::lock_guard<std::mutex> lock(session_mutex_);
        inbound_ = inbound;
        outbound_ = outbound;
      }

      std::thread to_target(
          [inbound, outbound] { forward_bytes(inbound, outbound); });
      std::thread to_source(
          [inbound, outbound] { forward_bytes(outbound, inbound); });
      to_target.join();
      to_source.join();

      {
        const std::lock_guard<std::mutex> lock(session_mutex_);
        inbound_ = -1;
        outbound_ = -1;
      }
      ::close(inbound);
      ::close(outbound);
    }
  }

  void close_listener() {
    if (listener_ < 0) {
      return;
    }
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    listener_ = -1;
  }

  void shutdown_active_session() {
    const std::lock_guard<std::mutex> lock(session_mutex_);
    if (inbound_ >= 0) {
      ::shutdown(inbound_, SHUT_RDWR);
    }
    if (outbound_ >= 0) {
      ::shutdown(outbound_, SHUT_RDWR);
    }
  }

  std::uint16_t target_port_ = 0;
  int listener_ = -1;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex session_mutex_;
  int inbound_ = -1;
  int outbound_ = -1;
  std::string error_;
};

ClusterSecurity derive_security(const std::string &token,
                                const std::string &node_id) {
  std::string reason;
  const auto security =
      zetta::cluster::derive_cluster_security(token, node_id, &reason);
  EXPECT_TRUE(security) << reason;
  return security.value_or(ClusterSecurity{});
}

template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = 20000) {
  for (int waited = 0; waited < timeout_ms; waited += 50) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return predicate();
}

} // namespace

TEST(PeerGateway, OverlappingPrivateNodesReplicateThroughOpaqueGateways) {
  constexpr std::uint16_t node_a_port = 43310;
  constexpr std::uint16_t node_b_port = 43311;
  constexpr std::uint16_t export_gateway_port = 43312;
  constexpr std::uint16_t import_gateway_port = 43313;
  const std::string colliding_private_endpoint = "10.244.0.7:7777";

  TcpForwarder export_gateway(export_gateway_port, node_a_port);
  ASSERT_TRUE(export_gateway.ready()) << export_gateway.error();
  TcpForwarder import_gateway(import_gateway_port, export_gateway_port);
  ASSERT_TRUE(import_gateway.ready()) << import_gateway.error();

  const std::string token = zetta::cluster::generate_join_token();
  const std::string node_a_id = zetta::util::new_uuid();
  const std::string node_b_id = zetta::util::new_uuid();

  ReplicationManager replication_a(node_a_id);
  ReplicationManager replication_b(node_b_id);
  replication_a.set(
      "node:" + node_a_id,
      nlohmann::json{{"address", ""}, {"reachability", "dial-out-only"}});
  replication_b.set(
      "node:" + node_b_id,
      nlohmann::json{{"address", ""}, {"reachability", "dial-out-only"}});
  replication_a.set("gateway:a", nlohmann::json{{"value", "from-a"}});

  ClusterConfig config_a;
  config_a.listen_host = "127.0.0.1";
  config_a.listen_port = node_a_port;
  config_a.dial_out_only = true;
  config_a.advertise = {colliding_private_endpoint};
  config_a.security = derive_security(token, node_a_id);

  ClusterConfig config_b;
  config_b.listen_host = "127.0.0.1";
  config_b.listen_port = node_b_port;
  config_b.seeds = {"127.0.0.1:" + std::to_string(import_gateway_port)};
  config_b.dial_out_only = true;
  config_b.advertise = {colliding_private_endpoint};
  config_b.security = derive_security(token, node_b_id);

  Cluster node_a(node_a_id, config_a, &replication_a);
  Cluster node_b(node_b_id, config_b, &replication_b);
  node_a.start();
  node_b.start();

  ASSERT_TRUE(wait_for(
      [&] { return node_a.peer_count() == 1 && node_b.peer_count() == 1; }));
  ASSERT_TRUE(node_a.advertised_endpoints().empty());
  ASSERT_TRUE(node_b.advertised_endpoints().empty());
  ASSERT_EQ(node_b.peers().size(), 1u);
  EXPECT_EQ(node_b.peers().front().address,
            "127.0.0.1:" + std::to_string(import_gateway_port));

  ASSERT_TRUE(wait_for(
      [&] {
        return replication_b.snapshot().contains("gateway:a") &&
               replication_a.version() == replication_b.version();
      },
      45000));
  EXPECT_EQ(replication_b.snapshot().at("gateway:a").at("value"), "from-a");

  replication_b.set("gateway:b", nlohmann::json{{"value", "from-b"}});
  ASSERT_TRUE(wait_for(
      [&] {
        const nlohmann::json snapshot_a = replication_a.snapshot();
        const nlohmann::json snapshot_b = replication_b.snapshot();
        return snapshot_a.contains("gateway:b") &&
               snapshot_b.contains("gateway:a") &&
               replication_a.version() == replication_b.version();
      },
      45000))
      << "documents did not converge through the gateway path: a="
      << replication_a.version() << " b=" << replication_b.version()
      << " snapshot_a=" << replication_a.snapshot().dump()
      << " snapshot_b=" << replication_b.snapshot().dump();
  EXPECT_EQ(replication_a.snapshot().at("gateway:b").at("value"), "from-b");

  node_b.stop();
  node_a.stop();
}
