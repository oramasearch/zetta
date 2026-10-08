// The object capability over the wire (#1005, docs/data/object-store.md): the raw
// byte routes and their statuses, the bucket aliases, fencing and the waiting
// acquire, the ON BUCKET grant, the quota, the registry renderings (MCP and
// OpenAPI), and the latency budgets — each measured over loopback HTTP against a
// file-backed node, failing above the budget.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/server.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

namespace {

std::uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size;
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident = 0;
    statm >> pages >> resident;
    return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

// Deterministic bytes by absolute offset, so a streamed body can be produced and
// checked a chunk at a time without ever holding it whole.
char byte_at(std::uint64_t offset) {
    std::uint64_t value = offset * 0x9E3779B97F4A7C15ull;
    value ^= value >> 29;
    return static_cast<char>(value & 0xFF);
}

std::string bytes_at(std::uint64_t offset, std::size_t size) {
    std::string out(size, '\0');
    for (std::size_t index = 0; index < size; ++index) out[index] = byte_at(offset + index);
    return out;
}

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0.0;
    std::ranges::sort(samples);
    return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1))];
}

double since_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
}

// A small keep-alive HTTP/1.1 client: one connection, requests in sequence, a
// response read by its Content-Length. Bodies can be streamed both ways.
class HttpClient {
public:
    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers;  // lower-case names
        std::string body;
        std::uint64_t body_bytes = 0;

        nlohmann::json json() const {
            return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
        }
    };

    HttpClient(unsigned short port, std::string bearer) : port_(port), bearer_(std::move(bearer)) {}
    ~HttpClient() { close(); }

    Response request(const std::string& method, const std::string& target,
                     const std::string& body = {},
                     const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        return exchange(method, target, body.size(), headers,
                        [&](std::uint64_t offset, std::size_t size) {
                            return std::string_view(body).substr(offset, size);
                        },
                        nullptr);
    }

    // Stream a request body of `size` bytes produced by `source(offset, n)`, and
    // hand each received body chunk to `sink` instead of keeping it.
    Response exchange(const std::string& method, const std::string& target, std::uint64_t size,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::function<std::string_view(std::uint64_t, std::size_t)>& source,
                      const std::function<void(std::string_view)>& sink) {
        if (descriptor_ < 0 && !connect()) return {};
        std::string head = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        head += "Authorization: Bearer " + bearer_ + "\r\n";
        for (const auto& [name, value] : headers) head += name + ": " + value + "\r\n";
        head += "Content-Length: " + std::to_string(size) + "\r\n\r\n";
        if (!send_all(head)) return {};
        constexpr std::size_t kChunk = 1024 * 1024;
        std::string scratch;
        for (std::uint64_t offset = 0; offset < size; offset += kChunk) {
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, size - offset));
            const std::string_view chunk = source(offset, take);
            if (!send_all(chunk)) return {};
        }
        return read_response(sink);
    }

    void close() {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = -1;
        buffered_.clear();
    }

private:
    bool connect() {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor_ < 0) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close();
            return false;
        }
        const int one = 1;
        ::setsockopt(descriptor_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval timeout{};
        timeout.tv_sec = 30;
        ::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        return true;
    }

    bool send_all(std::string_view bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t wrote = ::send(descriptor_, bytes.data() + sent, bytes.size() - sent, 0);
            if (wrote <= 0) {
                close();
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    bool fill() {
        char buffer[256 * 1024];
        const ssize_t got = ::recv(descriptor_, buffer, sizeof(buffer), 0);
        if (got <= 0) return false;
        buffered_.append(buffer, static_cast<std::size_t>(got));
        return true;
    }

    Response read_response(const std::function<void(std::string_view)>& sink) {
        Response response;
        std::size_t end = 0;
        while ((end = buffered_.find("\r\n\r\n")) == std::string::npos) {
            if (!fill()) {
                close();
                return {};
            }
        }
        const std::string head = buffered_.substr(0, end);
        buffered_.erase(0, end + 4);
        response.status = std::atoi(head.substr(9, 3).c_str());
        std::size_t line = head.find("\r\n");
        while (line != std::string::npos) {
            const std::size_t next = head.find("\r\n", line + 2);
            const std::string field = head.substr(line + 2, next == std::string::npos
                                                                 ? std::string::npos
                                                                 : next - line - 2);
            const std::size_t colon = field.find(':');
            if (colon != std::string::npos) {
                std::string name = field.substr(0, colon);
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string value = field.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                response.headers[name] = value;
            }
            line = next;
        }
        std::uint64_t remaining = std::strtoull(response.headers["content-length"].c_str(), nullptr, 10);
        while (remaining > 0) {
            if (buffered_.empty() && !fill()) {
                close();
                return {};
            }
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffered_.size()));
            if (sink) {
                sink(std::string_view(buffered_).substr(0, take));
            } else {
                response.body.append(buffered_, 0, take);
            }
            response.body_bytes += take;
            buffered_.erase(0, take);
            remaining -= take;
        }
        if (response.headers["connection"] == "close") close();
        return response;
    }

    unsigned short port_;
    std::string bearer_;
    int descriptor_ = -1;
    std::string buffered_;
};

// A file-backed node, its API server, and an owner loop on its own thread that
// drains the API on every wake and ticks at 10 Hz — the headless loop's shape.
struct ObjectRoutes : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication};
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string owner_secret = "object-routes-owner-secret";
    std::atomic<bool> stopping{false};
    std::thread loop;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
    }

    // Start the owner loop. Node state the test sets up directly is done first.
    void run_loop() {
        node.set_owner_wakeup([this] { server->wake(); });
        loop = std::thread([this] {
            auto last_tick = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
            while (!stopping) {
                server->drain();
                node.drain_owner_work();
                if (std::chrono::steady_clock::now() - last_tick >= std::chrono::milliseconds(100)) {
                    last_tick = std::chrono::steady_clock::now();
                    node.tick();
                }
                server->wait_for_work(std::chrono::milliseconds(20));
            }
        });
    }

    void TearDown() override {
        stopping = true;
        if (loop.joinable()) loop.join();
        node.set_owner_wakeup({});
        server->stop();
        node.stop();
    }

    HttpClient client() { return HttpClient(server->port(), owner_secret); }
};

using Headers = std::vector<std::pair<std::string, std::string>>;

}  // namespace

// Every byte route's contract over raw bodies: 200, 206, 404, 409 with the size,
// 416 with the size, a retried append that changes nothing, put if_absent, and
// the bucket aliases.
TEST_F(ObjectRoutes, BytesMoveRawOverRestWithTheSpecifiedStatuses) {
    run_loop();
    HttpClient http = client();
    auto created = http.request("POST", "/v1/create_bucket", R"({"bucket":"lake_ws","quota_bytes":1048576})");
    ASSERT_EQ(created.status, 200) << created.body;
    EXPECT_TRUE(created.json()["value"]["created"].get<bool>());

    const std::string binary("\x00\x01\xff\r\n\x7f payload", 14);
    auto put = http.request("PUT", "/v1/objects/lake_ws/index/run-1", binary);
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_EQ(put.json()["value"]["size"], 14);
    auto whole = http.request("GET", "/v1/objects/lake_ws/index/run-1");
    ASSERT_EQ(whole.status, 200);
    EXPECT_EQ(whole.body, binary);
    EXPECT_EQ(whole.headers["content-type"], "application/octet-stream");
    auto range = http.request("GET", "/v1/objects/lake_ws/index/run-1", {}, {{"Range", "bytes=2-5"}});
    ASSERT_EQ(range.status, 206);
    EXPECT_EQ(range.body, binary.substr(2, 4));
    EXPECT_EQ(range.headers["content-range"], "bytes 2-5/14");
    // The connection's next response carries none of the range's headers.
    auto after_range = http.request("GET", "/v1/objects/lake_ws:usage");
    EXPECT_EQ(after_range.status, 200);
    EXPECT_FALSE(after_range.headers.contains("content-range"));
    EXPECT_FALSE(after_range.headers.contains("zetta-generation"));
    EXPECT_EQ(after_range.headers["content-type"], "application/json");
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/index/run-1", {}, {{"Range", "bytes=10-"}}).body,
              binary.substr(10));
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/index/run-1", {}, {{"Range", "bytes=-3"}}).body,
              binary.substr(11));
    auto outside = http.request("GET", "/v1/objects/lake_ws/index/run-1", {}, {{"Range", "bytes=10-20"}});
    EXPECT_EQ(outside.status, 416);
    EXPECT_EQ(outside.json()["error"]["details"]["size"], 14);
    EXPECT_EQ(outside.headers["content-range"], "bytes */14");
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/missing").status, 404);
    EXPECT_EQ(http.request("GET", "/v1/objects/no_bucket/k").status, 404);

    // put if_absent: exactly once.
    EXPECT_EQ(http.request("PUT", "/v1/objects/lake_ws/LIQUID_STORE?if_absent=true", "v1").status, 200);
    EXPECT_EQ(http.request("PUT", "/v1/objects/lake_ws/LIQUID_STORE?if_absent=true", "v2").status, 409);

    // put range: create, append, a retry that changes nothing, and a conflict.
    EXPECT_EQ(http.request("PATCH", "/v1/objects/lake_ws/packs/1?offset=0", "abc").status, 200);
    auto appended = http.request("PATCH", "/v1/objects/lake_ws/packs/1?offset=3", "def");
    ASSERT_EQ(appended.status, 200) << appended.body;
    EXPECT_EQ(appended.json()["value"]["size"], 6);
    auto retried = http.request("PATCH", "/v1/objects/lake_ws/packs/1?offset=3", "def");
    ASSERT_EQ(retried.status, 200);
    EXPECT_TRUE(retried.json()["value"]["unchanged"].get<bool>());
    auto conflict = http.request("PATCH", "/v1/objects/lake_ws/packs/1?offset=2", "zz");
    EXPECT_EQ(conflict.status, 409);
    EXPECT_EQ(conflict.json()["error"]["code"], "conflict");
    EXPECT_EQ(conflict.json()["error"]["details"]["size"], 6);
    // The connection stays usable after a refusal that read its body.
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/packs/1").body, "abcdef");
    EXPECT_EQ(http.request("PATCH", "/v1/objects/lake_ws/packs/1", "x").status, 400);

    // The aliases: list, usage, barrier, delete.
    auto listed = http.request("GET", "/v1/objects/lake_ws?prefix=packs/");
    ASSERT_EQ(listed.status, 200) << listed.body;
    ASSERT_EQ(listed.json()["value"]["objects"].size(), 1u);
    EXPECT_EQ(listed.json()["value"]["objects"][0]["key"], "packs/1");
    EXPECT_EQ(listed.json()["value"]["objects"][0]["size"], 6);
    auto usage = http.request("GET", "/v1/objects/lake_ws:usage");
    ASSERT_EQ(usage.status, 200) << usage.body;
    EXPECT_EQ(usage.json()["value"]["objects"], 3);
    EXPECT_EQ(usage.json()["value"]["bytes"], 14 + 2 + 6);
    EXPECT_EQ(usage.json()["value"]["quota_bytes"], 1048576);
    auto barrier = http.request("POST", "/v1/objects/lake_ws:barrier");
    ASSERT_EQ(barrier.status, 200) << barrier.body;
    EXPECT_EQ(barrier.json()["value"]["nodes"], 1);
    EXPECT_EQ(http.request("DELETE", "/v1/objects/lake_ws/packs/1").status, 200);
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/packs/1").status, 404);
    EXPECT_EQ(http.request("DELETE", "/v1/objects/lake_ws/packs/1").status, 404);
    // Keys keep their slashes and their percent-encoded bytes.
    EXPECT_EQ(http.request("PUT", "/v1/objects/lake_ws/a%20b/c%2Fd", "x").status, 200);
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws?prefix=a").json()["value"]["objects"][0]["key"],
              "a b/c/d");
}

// The lease: a write without the fence is refused, a stale fence after a takeover
// is refused, and acquire waits for a release and for an expiry.
TEST_F(ObjectRoutes, TheLeaseFencesWritesAndAcquireWaitsForReleaseAndExpiry) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"lake_ws"})").status, 200);
    auto first = http.request("POST", "/v1/objects/lake_ws:acquire", R"({"holder":"lake-1","ttl_ms":60000})");
    ASSERT_EQ(first.status, 200) << first.body;
    const std::uint64_t fence = first.json()["value"]["fence"].get<std::uint64_t>();
    EXPECT_EQ(fence, 1u);
    auto unfenced = http.request("PUT", "/v1/objects/lake_ws/k", "x");
    EXPECT_EQ(unfenced.status, 409);
    EXPECT_EQ(unfenced.json()["error"]["code"], "fenced");
    EXPECT_EQ(unfenced.json()["error"]["details"]["fence"], 1);
    const Headers with_fence{{"Zetta-Fence", std::to_string(fence)}};
    EXPECT_EQ(http.request("PUT", "/v1/objects/lake_ws/k", "x", with_fence).status, 200);
    EXPECT_EQ(http.request("PATCH", "/v1/objects/lake_ws/k?offset=1", "y", with_fence).status, 200);
    EXPECT_EQ(http.request("DELETE", "/v1/objects/lake_ws/k", {}, {{"Zetta-Fence", "7"}}).status, 409);

    // No wait: held at once, naming the holder.
    auto held = http.request("POST", "/v1/objects/lake_ws:acquire", R"({"holder":"lake-2","ttl_ms":60000})");
    EXPECT_EQ(held.status, 409);
    EXPECT_EQ(held.json()["error"]["code"], "held");
    EXPECT_EQ(held.json()["error"]["details"]["holder"], "lake-1");

    // A successor waits while the predecessor drains, and gets the lease on release.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        HttpClient other = client();
        const auto released = other.request(
            "POST", "/v1/objects/lake_ws:release",
            nlohmann::json{{"holder", "lake-1"}, {"fence", fence}}.dump());
        EXPECT_EQ(released.status, 200) << released.body;
    });
    auto started = std::chrono::steady_clock::now();
    auto second = http.request("POST", "/v1/objects/lake_ws:acquire",
                               R"({"holder":"lake-2","ttl_ms":500,"wait_ms":5000})");
    const double waited = since_ms(started);
    releaser.join();
    ASSERT_EQ(second.status, 200) << second.body;
    EXPECT_EQ(second.json()["value"]["fence"], 2);
    EXPECT_GE(waited, 250.0);
    EXPECT_LT(waited, 3000.0);
    // The predecessor is fenced off for good.
    auto stale = http.request("PATCH", "/v1/objects/lake_ws/k?offset=2", "z", with_fence);
    EXPECT_EQ(stale.status, 409);
    EXPECT_EQ(stale.json()["error"]["code"], "fenced");

    // A holder that stops renewing loses the lease at expiry; the waiter gets it.
    started = std::chrono::steady_clock::now();
    auto third = http.request("POST", "/v1/objects/lake_ws:acquire",
                              R"({"holder":"lake-3","ttl_ms":60000,"wait_ms":5000})");
    ASSERT_EQ(third.status, 200) << third.body;
    EXPECT_EQ(third.json()["value"]["fence"], 3);
    EXPECT_LT(since_ms(started), 3000.0);
    // Renewal keeps the lease; a renewal with a stale fence is refused.
    auto renewed = http.request("POST", "/v1/objects/lake_ws:renew",
                                R"({"holder":"lake-3","fence":3,"ttl_ms":60000})");
    EXPECT_EQ(renewed.status, 200) << renewed.body;
    EXPECT_EQ(http.request("POST", "/v1/objects/lake_ws:renew",
                           R"({"holder":"lake-2","fence":2,"ttl_ms":60000})")
                  .json()["error"]["code"],
              "fenced");
    // The waiting acquire answers through MCP the same way.
    const nlohmann::json mcp{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "acquire_object_lease"},
          {"arguments", {{"bucket", "lake_ws"}, {"holder", "lake-4"}, {"ttl_ms", 1000}, {"wait_ms", 200}}}}}};
    started = std::chrono::steady_clock::now();
    auto over_mcp = http.request("POST", "/mcp", mcp.dump());
    ASSERT_EQ(over_mcp.status, 200);
    EXPECT_EQ(over_mcp.json()["result"]["structuredContent"]["error"]["code"], "held");
    EXPECT_GE(since_ms(started), 150.0);
}

// lake#543: the lease and barrier answers report the durability format lake
// checks before it opens a store, every write, lease and barrier answer carries
// the bucket's incarnation (server_info reports the node's), and a write or
// barrier sent with another incarnation in Zetta-Incarnation is refused
// `incarnation_changed` without being applied.
TEST_F(ObjectRoutes, AnswersCarryTheDurabilityFormatAndTheIncarnation) {
    run_loop();
    HttpClient http = client();
    auto info = http.request("GET", "/v1/server_info");
    ASSERT_EQ(info.status, 200) << info.body;
    EXPECT_EQ(info.json()["value"]["object_log_prefix_durability"], 1);
    const std::string node_incarnation = info.json()["value"]["incarnation"].get<std::string>();
    ASSERT_EQ(node_incarnation.size(), 32u);
    const std::string incarnation = node_incarnation + ".0";

    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"lake_ws"})").status, 200);
    auto acquired = http.request("POST", "/v1/objects/lake_ws:acquire", R"({"holder":"lake-1","ttl_ms":60000})");
    ASSERT_EQ(acquired.status, 200) << acquired.body;
    EXPECT_EQ(acquired.json()["value"]["object_log_prefix_durability"], 1);
    EXPECT_EQ(acquired.json()["value"]["incarnation"], incarnation);
    const Headers with_fence{{"Zetta-Fence", std::to_string(acquired.json()["value"]["fence"].get<std::uint64_t>())}};
    auto put = http.request("PUT", "/v1/objects/lake_ws/k", "x", with_fence);
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_EQ(put.json()["value"]["incarnation"], incarnation);
    // A body larger than a slice goes through an upload; its answer carries it too.
    auto large = http.request("PUT", "/v1/objects/lake_ws/large", std::string(5 * 1024 * 1024, 'l'), with_fence);
    ASSERT_EQ(large.status, 200) << large.body;
    EXPECT_EQ(large.json()["value"]["incarnation"], incarnation);
    auto patched = http.request("PATCH", "/v1/objects/lake_ws/k?offset=1", "y", with_fence);
    ASSERT_EQ(patched.status, 200) << patched.body;
    EXPECT_EQ(patched.json()["value"]["incarnation"], incarnation);
    auto removed = http.request("DELETE", "/v1/objects/lake_ws/large", {}, with_fence);
    ASSERT_EQ(removed.status, 200) << removed.body;
    EXPECT_EQ(removed.json()["value"]["incarnation"], incarnation);
    auto barrier = http.request("POST", "/v1/objects/lake_ws:barrier");
    ASSERT_EQ(barrier.status, 200) << barrier.body;
    EXPECT_EQ(barrier.json()["value"]["incarnation"], incarnation);
    EXPECT_EQ(barrier.json()["value"]["object_log_prefix_durability"], 1);
    auto renewed = http.request("POST", "/v1/objects/lake_ws:renew",
                                R"({"holder":"lake-1","fence":1,"ttl_ms":60000})");
    ASSERT_EQ(renewed.status, 200) << renewed.body;
    EXPECT_EQ(renewed.json()["value"]["incarnation"], incarnation);

    // Zetta-Incarnation is a precondition: another incarnation is refused and nothing applied.
    Headers expecting = with_fence;
    expecting.emplace_back("Zetta-Incarnation", incarnation);
    Headers stale = with_fence;
    stale.emplace_back("Zetta-Incarnation", std::string(32, '0') + ".0");
    EXPECT_EQ(http.request("PUT", "/v1/objects/lake_ws/k", "same", expecting).status, 200);
    for (const auto& [method, target, body] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"PUT", "/v1/objects/lake_ws/k", "other"},
             {"PUT", "/v1/objects/lake_ws/k2", std::string(5 * 1024 * 1024, 'u')},
             {"PATCH", "/v1/objects/lake_ws/k?offset=4", "more"},
             {"DELETE", "/v1/objects/lake_ws/k", ""},
             {"POST", "/v1/objects/lake_ws:barrier", ""}}) {
        auto refused = http.request(method, target, body, stale);
        EXPECT_EQ(refused.status, 409) << method << " " << target << ": " << refused.body;
        EXPECT_EQ(refused.json()["error"]["code"], "incarnation_changed") << method << " " << target;
        EXPECT_EQ(refused.json()["error"]["details"]["incarnation"], incarnation) << method << " " << target;
    }
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/k").body, "same");
    EXPECT_EQ(http.request("GET", "/v1/objects/lake_ws/k2").status, 404);
    EXPECT_EQ(http.request("POST", "/v1/objects/lake_ws:barrier", {}, expecting).status, 200);

    auto released = http.request("POST", "/v1/objects/lake_ws:release", R"({"holder":"lake-1","fence":1})");
    ASSERT_EQ(released.status, 200) << released.body;
    EXPECT_EQ(released.json()["value"]["incarnation"], incarnation);
}

// `GRANT objects:read, objects:write ON BUCKET lake_`: the holder reaches the
// lake_ buckets and no other; creating buckets is objects:manage; every byte call
// is audited.
TEST_F(ObjectRoutes, BucketGrantsReserveANamespace) {
    zetta::query::QueryService admin(node);
    for (const char* statement :
         {"CREATE ROLE lake_service", "GRANT mcp:call TO ROLE lake_service",
          "GRANT objects:read, objects:write ON BUCKET lake_ TO ROLE lake_service"}) {
        const auto outcome = admin.run(statement, "");
        ASSERT_TRUE(outcome.ok()) << statement << ": " << outcome.error;
    }
    auto service = node.create_user("lake-service");
    ASSERT_TRUE(service);
    ASSERT_TRUE(node.assign_role(service->id(), node.find_role("lake_service")->id()));
    const auto token = node.create_token(service->id(), zetta::governance::TokenType::Api);
    ASSERT_TRUE(token.token);
    run_loop();

    HttpClient owner = client();
    ASSERT_EQ(owner.request("POST", "/v1/create_bucket", R"({"bucket":"lake_ws"})").status, 200);
    ASSERT_EQ(owner.request("POST", "/v1/create_bucket", R"({"bucket":"other"})").status, 200);
    HttpClient lake(server->port(), token.secret);
    EXPECT_EQ(lake.request("PUT", "/v1/objects/lake_ws/k", "mine").status, 200);
    EXPECT_EQ(lake.request("GET", "/v1/objects/lake_ws/k").body, "mine");
    EXPECT_EQ(lake.request("GET", "/v1/objects/lake_ws").status, 200);
    EXPECT_EQ(lake.request("POST", "/v1/objects/lake_ws:barrier").status, 200);
    EXPECT_EQ(lake.request("POST", "/v1/objects/lake_ws:acquire", R"({"holder":"l","ttl_ms":1000})").status,
              200);
    auto refused = lake.request("PUT", "/v1/objects/other/k", "not mine");
    EXPECT_EQ(refused.status, 403);
    EXPECT_EQ(refused.json()["error"]["code"], "unauthorized");
    EXPECT_EQ(lake.request("GET", "/v1/objects/other/k").status, 403);
    EXPECT_EQ(lake.request("GET", "/v1/objects/other").status, 403);
    EXPECT_EQ(lake.request("POST", "/v1/create_bucket", R"({"bucket":"lake_more"})").status, 403);
    // The connection is still in step after a refused upload.
    EXPECT_EQ(lake.request("GET", "/v1/objects/lake_ws/k").body, "mine");

    auto audit = owner.request(
        "POST", "/v1/sql",
        R"({"statement":"SELECT * FROM system.audit WHERE target = 'api:put_object'"})");
    ASSERT_EQ(audit.status, 200) << audit.body;
    const auto rows = audit.json()["value"]["rows"];
    ASSERT_GE(rows.size(), 2u) << audit.body;
    bool allowed = false;
    bool denied = false;
    for (const auto& row : rows) {
        const auto& decision = row.at("allowed");
        const bool yes = decision.is_boolean() ? decision.get<bool>()
                                               : decision.dump() == "\"true\"" || decision == 1;
        (yes ? allowed : denied) = true;
    }
    EXPECT_TRUE(allowed);
    EXPECT_TRUE(denied);
}

TEST_F(ObjectRoutes, QuotaRefusalsAnswer507) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"small","quota_bytes":1000})").status, 200);
    EXPECT_EQ(http.request("PUT", "/v1/objects/small/a", std::string(600, 'a')).status, 200);
    auto over = http.request("PUT", "/v1/objects/small/b", std::string(600, 'b'));
    EXPECT_EQ(over.status, 507);
    EXPECT_EQ(over.json()["error"]["code"], "quota_exceeded");
    EXPECT_EQ(http.request("PATCH", "/v1/objects/small/a?offset=600", std::string(401, 'a')).status, 507);
    EXPECT_EQ(http.request("PATCH", "/v1/objects/small/a?offset=600", std::string(400, 'a')).status, 200);
    // A large body past the quota is refused; nothing of it is kept.
    auto large = http.request("PUT", "/v1/objects/small/c", std::string(8 * 1024 * 1024, 'c'));
    EXPECT_EQ(large.status, 507);
    EXPECT_EQ(http.request("GET", "/v1/objects/small/c").status, 404);
    EXPECT_EQ(http.request("GET", "/v1/objects/small:usage").json()["value"]["bytes"], 1000);
}

// The bucket and lease operations are registry capabilities (REST, MCP, OpenAPI);
// the byte routes are REST only, documented in OpenAPI.
TEST_F(ObjectRoutes, ControlOperationsAreToolsAndTheByteRoutesAreInOpenApi) {
    run_loop();
    HttpClient http = client();
    auto tools = http.request("POST", "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    ASSERT_EQ(tools.status, 200);
    std::vector<std::string> names;
    for (const auto& tool : tools.json()["result"]["tools"]) names.push_back(tool["name"]);
    for (const char* name : {"create_bucket", "list_objects", "object_usage", "object_barrier",
                             "delete_object", "acquire_object_lease", "renew_object_lease",
                             "release_object_lease"}) {
        EXPECT_NE(std::ranges::find(names, name), names.end()) << name;
    }
    EXPECT_EQ(std::ranges::find(names, "put_object"), names.end());
    auto openapi = http.request("GET", "/v1/openapi.json");
    ASSERT_EQ(openapi.status, 200);
    const auto paths = openapi.json()["paths"];
    for (const char* path : {"/v1/objects/{bucket}/{key}", "/v1/objects/{bucket}",
                             "/v1/objects/{bucket}:barrier", "/v1/objects/{bucket}:acquire",
                             "/v1/create_bucket", "/v1/acquire_object_lease"}) {
        EXPECT_TRUE(paths.contains(path)) << path;
    }
    EXPECT_TRUE(paths["/v1/objects/{bucket}/{key}"].contains("patch"));
}

// ---- the latency budget (#1005: tests fail above it) ---------------------------------

// 1 MiB PATCH append, then barrier, one node: p99 under 20 ms.
TEST_F(ObjectRoutes, AnAppendThenBarrierStaysUnderTwentyMilliseconds) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"bench"})").status, 200);
    const std::string mebibyte = bytes_at(0, 1024 * 1024);
    std::vector<double> samples;
    std::uint64_t size = 0;
    for (int round = 0; round < 120; ++round) {
        const auto started = std::chrono::steady_clock::now();
        auto appended = http.request("PATCH", "/v1/objects/bench/packs/1?offset=" + std::to_string(size),
                                     mebibyte);
        auto barrier = http.request("POST", "/v1/objects/bench:barrier");
        const double elapsed = since_ms(started);
        ASSERT_EQ(appended.status, 200) << appended.body;
        ASSERT_EQ(barrier.status, 200) << barrier.body;
        size += mebibyte.size();
        if (round >= 10) samples.push_back(elapsed);  // after warm-up
    }
    const double p50 = percentile(samples, 0.5);
    const double p99 = percentile(samples, 0.99);
    std::printf("[object budget] 1 MiB append + barrier: p50 %.2f ms, p99 %.2f ms (budget 20)\n", p50, p99);
    EXPECT_LT(p99, 20.0);
}

// 64 KiB ranged GET p99 under 2 ms; 4 MiB ranged GET p99 under 15 ms.
TEST_F(ObjectRoutes, RangedReadsStayInsideTheirBudgets) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"bench"})").status, 200);
    constexpr std::uint64_t kSize = 64ull * 1024 * 1024;
    for (std::uint64_t offset = 0; offset < kSize; offset += 4 * 1024 * 1024) {
        ASSERT_EQ(http.request("PATCH", "/v1/objects/bench/packs/1?offset=" + std::to_string(offset),
                               bytes_at(offset, 4 * 1024 * 1024))
                      .status,
                  200);
    }
    std::mt19937_64 random(11);
    const auto measure = [&](std::uint64_t length, int rounds) {
        std::vector<double> samples;
        for (int round = 0; round < rounds; ++round) {
            const std::uint64_t offset = random() % (kSize - length);
            const auto started = std::chrono::steady_clock::now();
            auto got = http.request("GET", "/v1/objects/bench/packs/1", {},
                                    {{"Range", "bytes=" + std::to_string(offset) + "-" +
                                                   std::to_string(offset + length - 1)}});
            samples.push_back(since_ms(started));
            EXPECT_EQ(got.status, 206);
            if (got.body != bytes_at(offset, length)) {
                ADD_FAILURE() << "wrong bytes at " << offset;
                break;
            }
        }
        return samples;
    };
    measure(64 * 1024, 50);  // warm-up
    const auto small = measure(64 * 1024, 1000);
    const auto large = measure(4 * 1024 * 1024, 200);
    std::printf("[object budget] 64 KiB ranged GET: p50 %.3f ms, p99 %.3f ms (budget 2)\n",
                percentile(small, 0.5), percentile(small, 0.99));
    std::printf("[object budget] 4 MiB ranged GET: p50 %.2f ms, p99 %.2f ms (budget 15)\n",
                percentile(large, 0.5), percentile(large, 0.99));
    EXPECT_LT(percentile(small, 0.99), 2.0);
    EXPECT_LT(percentile(large, 0.99), 15.0);
}

// Stream a 256 MiB object up and down at 200 MiB/s or better, with the process's
// resident memory flat: no growth with the object's size.
TEST_F(ObjectRoutes, StreamingA256MiBObjectRunsAt200MiBPerSecondWithFlatMemory) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"bench"})").status, 200);
    // Warm the paths, then take the baseline.
    ASSERT_EQ(http.request("PUT", "/v1/objects/bench/warm", bytes_at(0, 8 * 1024 * 1024)).status, 200);
    (void)http.request("GET", "/v1/objects/bench/warm");
    const std::uint64_t baseline = resident_bytes();
    std::atomic<bool> sampling{true};
    std::atomic<std::uint64_t> peak{baseline};
    std::thread sampler([&] {
        while (sampling) {
            peak = std::max<std::uint64_t>(peak, resident_bytes());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    constexpr std::uint64_t kSize = 256ull * 1024 * 1024;
    std::string chunk;
    auto started = std::chrono::steady_clock::now();
    auto put = http.exchange("PUT", "/v1/objects/bench/big", kSize, {},
                             [&](std::uint64_t offset, std::size_t size) {
                                 chunk = bytes_at(offset, size);
                                 return std::string_view(chunk);
                             },
                             nullptr);
    const double up_ms = since_ms(started);
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_EQ(put.json()["value"]["size"], kSize);

    std::uint64_t checked = 0;
    bool same = true;
    started = std::chrono::steady_clock::now();
    auto get = http.exchange("GET", "/v1/objects/bench/big", 0, {}, nullptr,
                             [&](std::string_view part) {
                                 if (same && part != bytes_at(checked, part.size())) same = false;
                                 checked += part.size();
                             });
    const double down_ms = since_ms(started);
    sampling = false;
    sampler.join();
    ASSERT_EQ(get.status, 200);
    EXPECT_EQ(get.body_bytes, kSize);
    EXPECT_TRUE(same);
    const double up = (kSize / 1048576.0) / (up_ms / 1000.0);
    const double down = (kSize / 1048576.0) / (down_ms / 1000.0);
    const double growth = (static_cast<double>(peak.load()) - static_cast<double>(baseline)) / 1048576.0;
    std::printf("[object budget] 256 MiB stream: up %.0f MiB/s, down %.0f MiB/s (budget 200); "
                "resident memory grew %.1f MiB\n",
                up, down, growth);
    EXPECT_GE(up, 200.0);
    EXPECT_GE(down, 200.0);
    // Flat: a few slices in flight, never the object.
    EXPECT_LT(growth, 64.0);
}

// Push-sized: 64 MiB as 64 × 1 MiB appends plus one barrier, under 1.5 s.
TEST_F(ObjectRoutes, APushOf64MiBAsAppendsFinishesUnderOneAndAHalfSeconds) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"bench"})").status, 200);
    std::vector<std::string> parts;
    for (int part = 0; part < 64; ++part) parts.push_back(bytes_at(part * 1048576ull, 1048576));
    const auto started = std::chrono::steady_clock::now();
    for (int part = 0; part < 64; ++part) {
        auto appended = http.request(
            "PATCH", "/v1/objects/bench/packs/push?offset=" + std::to_string(part * 1048576ull),
            parts[static_cast<std::size_t>(part)]);
        ASSERT_EQ(appended.status, 200) << appended.body;
    }
    ASSERT_EQ(http.request("POST", "/v1/objects/bench:barrier").status, 200);
    const double elapsed = since_ms(started);
    std::printf("[object budget] push of 64 x 1 MiB appends + barrier: %.0f ms (budget 1500)\n", elapsed);
    EXPECT_LT(elapsed, 1500.0);
}

// Pull-sized: 2,000 random 64 KiB reads by 8 concurrent readers, p99 under 5 ms.
TEST_F(ObjectRoutes, APullOf2000RandomReadsByEightReadersStaysUnderFiveMilliseconds) {
    run_loop();
    HttpClient http = client();
    ASSERT_EQ(http.request("POST", "/v1/create_bucket", R"({"bucket":"bench"})").status, 200);
    constexpr std::uint64_t kPack = 32ull * 1024 * 1024;
    for (int pack = 0; pack < 4; ++pack) {
        for (std::uint64_t offset = 0; offset < kPack; offset += 4 * 1024 * 1024) {
            ASSERT_EQ(http.request("PATCH",
                                   "/v1/objects/bench/packs/" + std::to_string(pack) +
                                       "?offset=" + std::to_string(offset),
                                   bytes_at(offset, 4 * 1024 * 1024))
                          .status,
                      200);
        }
    }
    std::vector<std::vector<double>> per_reader(8);
    std::atomic<int> wrong{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 8; ++reader) {
        readers.emplace_back([&, reader] {
            HttpClient mine = client();
            std::mt19937_64 random(100 + reader);
            for (int read = 0; read < 250; ++read) {
                const std::uint64_t offset = random() % (kPack - 65536);
                const std::string key = "packs/" + std::to_string(random() % 4);
                const auto started = std::chrono::steady_clock::now();
                auto got = mine.request("GET", "/v1/objects/bench/" + key, {},
                                        {{"Range", "bytes=" + std::to_string(offset) + "-" +
                                                       std::to_string(offset + 65535)}});
                per_reader[static_cast<std::size_t>(reader)].push_back(since_ms(started));
                if (got.status != 206 || got.body != bytes_at(offset, 65536)) ++wrong;
            }
        });
    }
    for (auto& reader : readers) reader.join();
    std::vector<double> samples;
    for (const auto& reader : per_reader) samples.insert(samples.end(), reader.begin(), reader.end());
    EXPECT_EQ(wrong.load(), 0);
    ASSERT_EQ(samples.size(), 2000u);
    std::printf("[object budget] 2,000 random 64 KiB reads by 8 readers: p50 %.3f ms, p99 %.3f ms "
                "(budget 5)\n",
                percentile(samples, 0.5), percentile(samples, 0.99));
    EXPECT_LT(percentile(samples, 0.99), 5.0);
}
