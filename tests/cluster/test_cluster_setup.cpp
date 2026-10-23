#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/util/uuid.hpp"

using namespace std::chrono_literals;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;

namespace {

class Socket {
 public:
    Socket() : descriptor(::socket(AF_INET, SOCK_STREAM, 0)) {
        timeval timeout{.tv_sec = 3, .tv_usec = 0};
        ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    }
    ~Socket() { if (descriptor >= 0) ::close(descriptor); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    bool connect(std::uint16_t port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
    }
    bool send(std::string_view text) {
        return ::send(descriptor, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size());
    }
    bool closed() {
        std::string buffer(4096, '\0');
        for (;;) {
            const auto count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
            if (count == 0) return true;
            if (count < 0) return errno == ECONNRESET;
        }
    }
    int descriptor;
};

ClusterConfig listening(std::uint16_t port) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.peer_setup_timeout = 1000ms;
    return config;
}

std::string hello() {
    return "ZETTA/1 HELLO " + zetta::util::new_uuid() + " - dial-out 2\n";
}

bool joined(const Cluster& node, std::size_t expected = 1) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (node.peer_count() == expected) return true;
        std::this_thread::sleep_for(10ms);
    }
    return node.peer_count() == expected;
}

// A real TLS peer whose HELLO can be delayed independently of its handshake.
class TlsPeer {
 public:
    explicit TlsPeer(const zetta::cluster::ClusterSecurity& security)
        : context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free),
          session(nullptr, SSL_free) {
        std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bytes(
            BIO_new_mem_buf(security.certificate_pem.data(),
                            static_cast<int>(security.certificate_pem.size())), BIO_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(
            PEM_read_bio_X509(certificate_bytes.get(), nullptr, nullptr, nullptr), X509_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bytes(
            BIO_new_mem_buf(security.private_key_pem.data(),
                            static_cast<int>(security.private_key_pem.size())), BIO_free);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            PEM_read_bio_PrivateKey(key_bytes.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        if (!certificate || !key || !SSL_CTX_use_certificate(context.get(), certificate.get()) ||
            !SSL_CTX_use_PrivateKey(context.get(), key.get())) return;
        session.reset(SSL_new(context.get()));
        SSL_set_fd(session.get(), socket.descriptor);
    }
    bool handshake() { return session && SSL_connect(session.get()) == 1; }
    bool closed() {
        char byte = 0;
        return SSL_read(session.get(), &byte, 1) <= 0 && errno != EAGAIN && errno != EWOULDBLOCK;
    }
    Socket socket;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context;
    std::unique_ptr<SSL, decltype(&SSL_free)> session;
};

// Runs in an isolated subprocess: only that process's descriptor limit changes.
int accept_after_exhaustion() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return 1;
    limit.rlim_cur = std::min<rlim_t>(limit.rlim_cur, 128);
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) return 2;
    auto config = listening(43807);
    config.log_events = true;
    Cluster node(zetta::util::new_uuid(), config);
    node.start();
    if (!node.is_listening()) return 3;
    Socket queued;  // Allocate before exhausting the server process's descriptors.
    std::vector<int> fillers;
    for (;;) {
        const int descriptor = ::open("/dev/null", O_RDONLY);
        if (descriptor < 0) break;
        fillers.push_back(descriptor);
    }
    if (errno != EMFILE || !queued.connect(config.listen_port)) return 4;
    std::this_thread::sleep_for(350ms);  // Allow accept to observe EMFILE.
    for (const int descriptor : fillers) ::close(descriptor);
    Socket healthy;
    if (!healthy.connect(config.listen_port) || !healthy.send(hello())) return 5;
    if (!joined(node)) return 6;
    node.stop();
    return 0;
}

}  // namespace

TEST(ClusterSetup, SilentTlsConnectionExpiresAndSlotRecovers) {
    auto config = listening(43801);
    config.maximum_pending_setups = 1;
    const auto node_id = zetta::util::new_uuid();
    const auto token = zetta::cluster::generate_join_token();
    auto security = zetta::cluster::derive_cluster_security(token, node_id);
    ASSERT_TRUE(security);
    config.security = *security;
    Cluster node(node_id, config);
    node.start();
    ASSERT_TRUE(node.is_listening());
    Socket stalled;
    ASSERT_TRUE(stalled.connect(config.listen_port));
    EXPECT_TRUE(stalled.closed());
    EXPECT_EQ(node.peer_count(), 0u);
    auto peer_config = listening(43802);
    const auto peer_id = zetta::util::new_uuid();
    auto peer_security = zetta::cluster::derive_cluster_security(token, peer_id);
    ASSERT_TRUE(peer_security);
    peer_config.security = *peer_security;
    peer_config.seeds = {node.listen_address()};
    Cluster healthy(peer_id, peer_config);
    healthy.start();
    EXPECT_TRUE(joined(node));
}

TEST(ClusterSetup, PartialHelloProgressDoesNotExtendDeadline) {
    auto config = listening(43803);
    Cluster node(zetta::util::new_uuid(), config);
    node.start();
    ASSERT_TRUE(node.is_listening());
    Socket stalled;
    ASSERT_TRUE(stalled.connect(config.listen_port));
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(stalled.send("Z"));
        std::this_thread::sleep_for(200ms);
    }
    EXPECT_TRUE(stalled.closed());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1800ms);
    EXPECT_EQ(node.peer_count(), 0u);
    Socket healthy;
    ASSERT_TRUE(healthy.connect(config.listen_port));
    ASSERT_TRUE(healthy.send(hello()));
    EXPECT_TRUE(joined(node));
    // Successful setup cancels its deadline; it must survive that deadline.
    std::this_thread::sleep_for(1200ms);
    EXPECT_EQ(node.peer_count(), 1u);
}

TEST(ClusterSetup, TlsAndHelloShareOneDeadline) {
    auto config = listening(43804);
    config.peer_setup_timeout = 2000ms;
    const auto token = zetta::cluster::generate_join_token();
    const auto node_id = zetta::util::new_uuid();
    auto security = zetta::cluster::derive_cluster_security(token, node_id);
    ASSERT_TRUE(security);
    config.security = *security;
    Cluster node(node_id, config);
    node.start();
    ASSERT_TRUE(node.is_listening());
    auto peer_security = zetta::cluster::derive_cluster_security(token, zetta::util::new_uuid());
    ASSERT_TRUE(peer_security);
    TlsPeer stalled(*peer_security);
    ASSERT_TRUE(stalled.socket.connect(config.listen_port));
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1200ms);
    ASSERT_TRUE(stalled.handshake());
    EXPECT_TRUE(stalled.closed());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2800ms);
    EXPECT_EQ(node.peer_count(), 0u);
}

TEST(ClusterSetup, ExcessPendingConnectionsAreClosedBeforeHello) {
    auto config = listening(43805);
    config.maximum_pending_setups = 1;
    config.peer_setup_timeout = 5000ms;
    Cluster node(zetta::util::new_uuid(), config);
    node.start();
    ASSERT_TRUE(node.is_listening());
    {
        Socket stalled;
        ASSERT_TRUE(stalled.connect(config.listen_port));
        Socket excess;
        ASSERT_TRUE(excess.connect(config.listen_port));
        EXPECT_TRUE(excess.closed());
        EXPECT_EQ(node.peer_count(), 0u);
    }
    // A failed setup releases its slot before its original deadline.
    std::this_thread::sleep_for(100ms);
    Socket healthy;
    ASSERT_TRUE(healthy.connect(config.listen_port));
    ASSERT_TRUE(healthy.send(hello()));
    EXPECT_TRUE(joined(node));
    Socket second;
    ASSERT_TRUE(second.connect(config.listen_port));
    ASSERT_TRUE(second.send(hello()));
    EXPECT_TRUE(joined(node, 2));  // established links no longer occupy the single slot
}

TEST(ClusterSetup, InboundFloodDoesNotBlockOwnDials) {
    auto config = listening(43809);
    config.maximum_pending_setups = 1;
    config.peer_setup_timeout = 5000ms;
    config.reconnect_interval_seconds = 1;
    config.seeds = {"127.0.0.1:43810"};
    Cluster node(zetta::util::new_uuid(), config);
    node.start();
    ASSERT_TRUE(node.is_listening());
    Socket stalled;  // holds the only inbound setup slot for the whole test
    ASSERT_TRUE(stalled.connect(config.listen_port));
    std::this_thread::sleep_for(100ms);
    Cluster seed(zetta::util::new_uuid(), listening(43810));
    seed.start();
    ASSERT_TRUE(seed.is_listening());
    EXPECT_TRUE(joined(node));  // well inside the stalled setup's 5 s deadline
}

TEST(ClusterSetup, DialedConnectionAlsoExpiresBeforeHello) {
    Socket listener;
    int reuse = 1;
    ::setsockopt(listener.descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(43808);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener.descriptor, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    ASSERT_EQ(::listen(listener.descriptor, 1), 0);
    auto config = listening(43806);
    config.seeds = {"127.0.0.1:43808"};
    Cluster node(zetta::util::new_uuid(), config);
    node.start();
    Socket accepted;
    ::close(accepted.descriptor);
    accepted.descriptor = ::accept(listener.descriptor, nullptr, nullptr);
    ASSERT_GE(accepted.descriptor, 0);
    timeval timeout{.tv_sec = 3, .tv_usec = 0};
    ::setsockopt(accepted.descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    EXPECT_TRUE(accepted.closed());
    EXPECT_EQ(node.peer_count(), 0u);
}

TEST(ClusterSetup, AcceptRecoversAfterDescriptorExhaustion) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({ ::_exit(accept_after_exhaustion()); }, ::testing::ExitedWithCode(0),
                "peer accept failed; retrying");
}
