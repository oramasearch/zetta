// Protocol-layer coverage of the native coroutine HTTP client that fills the
// connector transport interface (#827, docs/api/native-http-transport.md).
//
// The client is exercised against the engine's own REST server on loopback —
// real sockets, real HTTP/1.1 framing through Boost.HTTP's serializer/parser —
// so this is the outbound mirror of tests/api/test_rest_server.cpp. TLS is not
// exercised here (that needs a certificate authority and a real peer); the TLS
// path is the same coroutine with an `openssl_stream` in place of the socket.
//
// The test thread plays the owning loop (drain()), and client work runs on a
// helper thread — the same layer-2 discipline the REST tests use.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/native_http_transport.hpp"
#include "zetta/api/server.hpp"
#include "zetta/connect/egress_policy.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/node.hpp"
#include "zetta/runtime.hpp"

namespace {

struct NativeHttpTransportFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string owner_secret = "native-transport-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;  // ephemeral
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
        ASSERT_NE(server->port(), 0);
        zetta::api::install_native_http_transport();
    }

    void TearDown() override {
        zetta::api::shutdown_native_http_transport();
        server->stop();
        node.stop();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(server->port());
    }

    zetta::connect::HttpRequest get(const std::string& path,
                                    const std::string& bearer) const {
        zetta::connect::HttpRequest request;
        request.method = "GET";
        request.url = base_url() + path;
        request.timeout_seconds = 20;
        if (!bearer.empty()) {
            request.headers.push_back({"Authorization", "Bearer " + bearer});
            request.secret_headers.push_back("Authorization");
        }
        return request;
    }

    // Run `work` on a helper thread while this thread drains the server's
    // bridge, so a capability that must execute on the owning loop can answer.
    void drive(const std::function<void()>& work) {
        std::atomic<bool> finished{false};
        std::thread worker([&] {
            work();
            finished = true;
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        worker.join();
    }
};

TEST_F(NativeHttpTransportFixture, InstallsItselfAsTheDefaultTransport) {
    EXPECT_TRUE(static_cast<bool>(zetta::connect::default_http_transport()));
    EXPECT_TRUE(static_cast<bool>(zetta::connect::default_async_http_transport()));

    zetta::api::shutdown_native_http_transport();
    EXPECT_FALSE(static_cast<bool>(zetta::connect::default_http_transport()));
    EXPECT_FALSE(
        static_cast<bool>(zetta::connect::default_async_http_transport()));
}

TEST_F(NativeHttpTransportFixture, PerformsARealLoopbackExchange) {
    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(
            get("/v1/server_info", owner_secret));
    });
    EXPECT_TRUE(response.error.empty()) << response.error;
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("\"node_id\""), std::string::npos)
        << response.body;
}

TEST_F(NativeHttpTransportFixture, APeerErrorStatusIsNotATransportError) {
    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(
            get("/v1/server_info", /*bearer=*/{}));
    });
    // The exchange completed: no transport error, and a real status from the
    // peer for the caller to interpret.
    EXPECT_TRUE(response.error.empty()) << response.error;
    EXPECT_GE(response.status, 400);
    EXPECT_LT(response.status, 500);
}

TEST_F(NativeHttpTransportFixture, ReportsAnUnreachableHostWithoutLeakingTheToken) {
    zetta::connect::HttpRequest request;
    request.method = "GET";
    // Port 1 on loopback: nothing listens, so the connect fails fast.
    request.url = "http://127.0.0.1:1/v1/server_info";
    request.timeout_seconds = 10;
    request.headers.push_back({"Authorization", "Bearer super-secret-token"});
    request.secret_headers.push_back("Authorization");

    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(request);
    });
    EXPECT_FALSE(response.error.empty());
    EXPECT_EQ(response.status, 0);
    EXPECT_EQ(response.error.find("super-secret-token"), std::string::npos)
        << response.error;
}

TEST_F(NativeHttpTransportFixture, RejectsAUrlThatIsNotAbsoluteHttp) {
    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = "ftp://example.invalid/thing";

    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(request);
    });
    EXPECT_FALSE(response.error.empty());
    EXPECT_EQ(response.status, 0);
}

// A loopback listener that accepts the TCP connection (the kernel completes the
// handshake into its backlog) and then never says a word — the shape of a peer
// that swallows a TLS ClientHello.
struct SilentListener {
    int fd = -1;
    int port = 0;

    SilentListener() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (fd < 0 ||
            ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd, 4) != 0) {
            close();
            return;
        }
        socklen_t length = sizeof(address);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
    }
    ~SilentListener() { close(); }
    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

// Runs `request` through the async transport under an OUTER deadline of
// `limit` and reports whether the transport answered first. The outer deadline
// only keeps a regression from hanging the suite: the request's own
// timeout_seconds must end it well before `limit`.
bool finishes_within(const zetta::connect::HttpRequest& request,
                     std::chrono::seconds limit,
                     zetta::connect::HttpResponse& response) {
    auto [outer_timeout, answered] = zetta::runtime::block_on(
        zetta::runtime::timeout(
            zetta::connect::default_async_http_transport()(request), limit));
    response = std::move(answered);
    return !outer_timeout;
}

// lake#801: the request's timeout bounds the TLS handshake, not only the
// exchange after it. A peer that accepts and then stays silent used to hold the
// caller until the connection died — a run's report delivery held the
// workspace for minutes.
TEST_F(NativeHttpTransportFixture, TheTimeoutBoundsASilentTlsHandshake) {
    SilentListener listener;
    ASSERT_GT(listener.port, 0);
    zetta::connect::HttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(listener.port) + "/hooks";
    request.body = "{}";
    request.timeout_seconds = 2;

    zetta::connect::HttpResponse response;
    const auto started = std::chrono::steady_clock::now();
    const bool in_time =
        finishes_within(request, std::chrono::seconds(15), response);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(in_time) << "the handshake was not bounded by timeout_seconds";
    EXPECT_LT(elapsed, std::chrono::seconds(10));
    EXPECT_FALSE(response.error.empty());
    EXPECT_EQ(response.status, 0);
    EXPECT_NE(response.error.find("TLS handshake"), std::string::npos)
        << response.error;
}

// A cancelled JWKS waiter must leave the response owned by the I/O coroutine.
TEST_F(NativeHttpTransportFixture, CancelledWaitReturnsNoPartiallyWrittenResponse) {
    SilentListener listener;
    ASSERT_GT(listener.port, 0);
    zetta::connect::HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(listener.port) + "/jwks";
    request.timeout_seconds = 1;
    std::stop_source cancellation;
    cancellation.request_stop();
    auto [error, response] = zetta::runtime::block_on(zetta::runtime::with_stop_token(
        zetta::connect::default_async_http_transport()(request),
        cancellation.get_token()));
    EXPECT_TRUE(error);
    EXPECT_EQ(response.status, 0);
    EXPECT_TRUE(response.body.empty());
    EXPECT_TRUE(response.error.empty());
}

// lake#801: the timeout bounds the TCP connect too. A non-routable address
// models a default-deny egress policy that drops the SYN: without the bound the
// connect waits out the kernel's SYN retries (about 130 s on Linux). Where the
// network refuses it at once instead, the request still ends well inside the
// limit, so this holds on any host.
TEST_F(NativeHttpTransportFixture, TheTimeoutBoundsAConnectThatIsNeverAnswered) {
    zetta::connect::HttpRequest request;
    request.url = "http://10.255.255.1:81/hooks/actions";
    request.body = "{}";
    request.timeout_seconds = 2;

    zetta::connect::HttpResponse response;
    const auto started = std::chrono::steady_clock::now();
    const bool in_time =
        finishes_within(request, std::chrono::seconds(15), response);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(in_time) << "the connect was not bounded by timeout_seconds";
    EXPECT_LT(elapsed, std::chrono::seconds(10));
    EXPECT_FALSE(response.error.empty());
    EXPECT_EQ(response.status, 0);
}

// A large download streams straight to a file instead of buffering the body on
// the heap (the weights fetch relies on this for multi-GB shards).
TEST_F(NativeHttpTransportFixture, StreamsTheBodyToAFileWhenAsked) {
    const std::string path = (dir.path() / "server_info.json").string();
    zetta::connect::HttpRequest request = get("/v1/server_info", owner_secret);
    request.download_to_path = path;

    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(request);
    });
    EXPECT_TRUE(response.error.empty()) << response.error;
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.body.empty());  // streamed to disk, not buffered in memory

    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("\"node_id\""), std::string::npos) << contents;
}

// The property Part 3's distributed fetch rests on: N exchanges in flight at
// once on one node, joined through the runtime interface's when_all.
TEST_F(NativeHttpTransportFixture, FansOutConcurrentExchangesThroughWhenAll) {
    constexpr int kRequests = 8;
    std::vector<zetta::connect::HttpResponse> responses;

    drive([&] {
        responses = zetta::runtime::block_on(
            [](zetta::connect::AsyncHttpTransport transport,
               std::vector<zetta::connect::HttpRequest> requests)
                -> zetta::runtime::task<
                    std::vector<zetta::connect::HttpResponse>> {
                std::vector<zetta::runtime::io_task<zetta::connect::HttpResponse>>
                    exchanges;
                exchanges.reserve(requests.size());
                for (const zetta::connect::HttpRequest& request : requests) {
                    exchanges.push_back(transport(request));
                }
                auto [error, gathered] =
                    co_await zetta::runtime::when_all(std::move(exchanges));
                if (error) co_return {};
                co_return std::move(gathered);
            }(zetta::connect::default_async_http_transport(), [&] {
                std::vector<zetta::connect::HttpRequest> requests;
                for (int index = 0; index < kRequests; ++index) {
                    requests.push_back(get("/v1/server_info", owner_secret));
                }
                return requests;
            }()));
    });

    ASSERT_EQ(responses.size(), static_cast<std::size_t>(kRequests));
    for (const zetta::connect::HttpResponse& response : responses) {
        EXPECT_TRUE(response.error.empty()) << response.error;
        EXPECT_EQ(response.status, 200);
    }
}

// --- The egress policy at the dialler (CTL-4, #75) -------------------------

// Installs a policy and a recording denial observer for one test, and puts the
// process back to the default (deny, loopback only, no observer) afterwards.
struct EgressPolicyScope {
    std::vector<zetta::connect::EgressDenial> denials;
    std::mutex guard;

    explicit EgressPolicyScope(const std::string& allowlist) {
        std::string error;
        const auto policy = zetta::connect::EgressPolicy::parse(allowlist, &error);
        EXPECT_TRUE(policy) << error;
        zetta::connect::set_egress_policy(policy.value_or(zetta::connect::EgressPolicy{}));
        zetta::connect::set_egress_denial_observer(
            [this](const zetta::connect::EgressDenial& denial) {
                const std::lock_guard<std::mutex> lock(guard);
                denials.push_back(denial);
            });
    }
    ~EgressPolicyScope() {
        zetta::connect::set_egress_policy({});
        zetta::connect::set_egress_denial_observer({});
    }
};

zetta::connect::HttpRequest plain_get(const std::string& url) {
    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.timeout_seconds = 5;
    return request;
}

TEST_F(NativeHttpTransportFixture, RefusesAnOutsideAddressByDefault) {
    EgressPolicyScope scope("");
    zetta::connect::HttpResponse response;
    drive([&] {
        // TEST-NET-1: never routed. Without the policy this would be dialled.
        response = zetta::connect::default_http_transport()(
            plain_get("http://192.0.2.1:9/anything"));
    });
    EXPECT_EQ(response.status, 0);
    EXPECT_TRUE(response.error.starts_with("egress denied: 192.0.2.1:9"))
        << response.error;
    ASSERT_EQ(scope.denials.size(), 1U);
    EXPECT_EQ(scope.denials[0].host, "192.0.2.1");
    EXPECT_EQ(scope.denials[0].port, 9);
}

TEST_F(NativeHttpTransportFixture, RefusesAnUnlistedNameBeforeResolvingIt) {
    EgressPolicyScope scope("api.example.com:443");
    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(
            plain_get("https://exfiltrate.example.invalid/data"));
    });
    // Refused by the policy, not by DNS: the name was never looked up.
    EXPECT_TRUE(response.error.starts_with("egress denied: exfiltrate.example.invalid:443"))
        << response.error;
    EXPECT_EQ(response.error.find("could not resolve"), std::string::npos);
    EXPECT_EQ(scope.denials.size(), 1U);
}

TEST_F(NativeHttpTransportFixture, DialsADestinationOnTheAllowlist) {
    EgressPolicyScope scope("allowed.example.invalid");
    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(
            plain_get("http://allowed.example.invalid/"));
    });
    // The policy let it through to DNS, which then fails for a .invalid name.
    EXPECT_FALSE(response.error.empty());
    EXPECT_FALSE(response.error.starts_with("egress denied")) << response.error;
    EXPECT_TRUE(scope.denials.empty());
}

TEST_F(NativeHttpTransportFixture, LoopbackIsNotEgress) {
    EgressPolicyScope scope("");
    zetta::connect::HttpResponse response;
    drive([&] {
        response = zetta::connect::default_http_transport()(
            get("/v1/server_info", owner_secret));
    });
    EXPECT_TRUE(response.error.empty()) << response.error;
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(scope.denials.empty());
}

// A redirect is a new connection, so it is checked again: an allowed host that
// answers 302 to an outside one cannot carry the request out.
TEST_F(NativeHttpTransportFixture, RefusesARedirectToAnOutsideDestination) {
    EgressPolicyScope scope("");

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    const int port = ntohs(address.sin_port);

    std::thread redirector([listener] {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) return;
        char buffer[4096];
        [[maybe_unused]] const auto received = ::recv(client, buffer, sizeof(buffer), 0);
        const std::string reply =
            "HTTP/1.1 302 Found\r\nLocation: http://192.0.2.1:9/stolen\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        [[maybe_unused]] const auto sent = ::send(client, reply.data(), reply.size(), 0);
        ::close(client);
    });

    zetta::connect::HttpRequest request =
        plain_get("http://127.0.0.1:" + std::to_string(port) + "/start");
    request.max_redirects = 3;
    zetta::connect::HttpResponse response;
    drive([&] { response = zetta::connect::default_http_transport()(request); });
    redirector.join();
    ::close(listener);

    EXPECT_TRUE(response.error.starts_with("egress denied: 192.0.2.1:9"))
        << response.error;
    EXPECT_EQ(scope.denials.size(), 1U);
}

// #1429: a secret header stays on its origin. A redirect to the same host on another
// port is another origin, so the bearer is dropped before that hop.
TEST_F(NativeHttpTransportFixture, ARedirectToAnotherPortDropsTheSecretHeaders) {
    EgressPolicyScope scope("");
    const auto listen_on_loopback = [](int& port) {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener, 1);
        socklen_t length = sizeof(address);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        return listener;
    };
    int first_port = 0;
    int second_port = 0;
    const int first = listen_on_loopback(first_port);
    const int second = listen_on_loopback(second_port);
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);
    std::string second_request;
    std::thread redirector([first, second_port] {
        const int client = ::accept(first, nullptr, nullptr);
        if (client < 0) return;
        char buffer[4096];
        [[maybe_unused]] const auto received = ::recv(client, buffer, sizeof(buffer), 0);
        const std::string reply = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" +
                                  std::to_string(second_port) +
                                  "/next\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        [[maybe_unused]] const auto sent = ::send(client, reply.data(), reply.size(), 0);
        ::close(client);
    });
    std::thread target([second, &second_request] {
        const int client = ::accept(second, nullptr, nullptr);
        if (client < 0) return;
        char buffer[4096];
        const auto received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received > 0) second_request.assign(buffer, static_cast<std::size_t>(received));
        const std::string reply =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        [[maybe_unused]] const auto sent = ::send(client, reply.data(), reply.size(), 0);
        ::close(client);
    });

    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:" + std::to_string(first_port) + "/start";
    request.timeout_seconds = 20;
    request.headers.push_back({"Authorization", "Bearer must-not-travel"});
    request.secret_headers.push_back("Authorization");
    request.max_redirects = 3;
    zetta::connect::HttpResponse response;
    drive([&] { response = zetta::connect::default_http_transport()(request); });
    redirector.join();
    target.join();
    ::close(first);
    ::close(second);

    EXPECT_EQ(response.status, 200) << response.error;
    EXPECT_FALSE(second_request.empty());
    EXPECT_EQ(second_request.find("must-not-travel"), std::string::npos) << second_request;
}

}  // namespace
