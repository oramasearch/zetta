#include "zetta/api/native_http_transport.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/request.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/static_response.hpp>

#include "system_trust_store.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/runtime.hpp"
#include "zetta/runtime/net.hpp"

// The native coroutine HTTPS client behind the connector transport interface (#827).
//
// One of the three translation units allowed to name Corosio (through
// `zetta::runtime::net`), alongside `src/core/cluster/cluster.cpp` and
// `src/core/api/mysql_protocol.cpp`: the cluster adapter owns inbound/peer
// traffic, this one owns outbound HTTP(S), the MySQL driver outbound MySQL. It
// is compiled into `zetta-api`, not `zetta-core`, so Boost.HTTP never reaches
// the core's link surface — see include/zetta/api/native_http_transport.hpp.
namespace zetta::api {
namespace {

namespace net = zetta::runtime::net;
namespace http = boost::http;

// One connection per exchange, `Connection: close`, no pooling. A connector's
// requests fan out concurrently rather than pipelining down one socket, so a
// pool would buy little and cost a lifetime problem (an idle TLS session held
// across a node's whole uptime). The cost is one handshake per request, which is
// why callers bound their fan-out.
//
// Result partitions from a warehouse are large; the parser's default client body
// limit (1 MiB) would truncate them, so every response raises its own limit.
constexpr std::uint64_t kResponseBodyLimit = 512ull * 1024ull * 1024ull;

// --- URL parsing ---------------------------------------------------------------

struct RequestTarget {
    bool secure = true;
    std::string host;
    std::string port;
    std::string target;  // path + query, as sent on the request line
};

// Split an absolute http/https URL. Deliberately narrow: no userinfo, no
// fragment, no relative form — a connector always builds an absolute URL from a
// validated config. Returns false (never throws) on anything else.
bool parse_request_target(std::string_view url, RequestTarget& parsed) {
    if (url.starts_with("https://")) {
        parsed.secure = true;
        parsed.port = "443";
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        parsed.secure = false;
        parsed.port = "80";
        url.remove_prefix(7);
    } else {
        return false;
    }

    const std::size_t path_start = url.find_first_of("/?#");
    std::string_view authority =
        path_start == std::string_view::npos ? url : url.substr(0, path_start);
    parsed.target = path_start == std::string_view::npos
                        ? std::string("/")
                        : std::string(url.substr(path_start));
    if (parsed.target.empty() || parsed.target.front() != '/') {
        parsed.target.insert(parsed.target.begin(), '/');
    }
    if (authority.find('@') != std::string_view::npos) {
        return false;  // credentials in a URL are never accepted
    }

    // A bracketed IPv6 literal keeps its brackets out of the host we resolve.
    if (authority.starts_with("[")) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        parsed.host = std::string(authority.substr(1, close - 1));
        std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            parsed.port = std::string(rest.substr(1));
        }
    } else {
        const std::size_t colon = authority.find(':');
        if (colon == std::string_view::npos) {
            parsed.host = std::string(authority);
        } else {
            parsed.host = std::string(authority.substr(0, colon));
            parsed.port = std::string(authority.substr(colon + 1));
        }
    }
    return !parsed.host.empty() && !parsed.port.empty();
}

// The Host header value: the authority as the peer expects to see it (the port
// is omitted when it is the scheme default).
std::string host_header_value(const RequestTarget& parsed) {
    const bool default_port =
        (parsed.secure && parsed.port == "443") ||
        (!parsed.secure && parsed.port == "80");
    const bool needs_brackets = parsed.host.find(':') != std::string::npos;
    const std::string host =
        needs_brackets ? "[" + parsed.host + "]" : parsed.host;
    return default_port ? host : host + ":" + parsed.port;
}

// Resolve a redirect `Location` against the URL it came from. HuggingFace sends an
// absolute CDN URL; the root-relative and relative forms are handled for
// correctness. Returns the target verbatim when the base cannot be parsed.
std::string resolve_location(const std::string& base_url,
                             const std::string& location) {
    if (location.starts_with("http://") || location.starts_with("https://")) {
        return location;
    }
    RequestTarget base;
    if (!parse_request_target(base_url, base)) return location;
    const std::string origin =
        (base.secure ? "https://" : "http://") + host_header_value(base);
    if (location.starts_with("/")) return origin + location;  // root-relative
    const std::size_t slash = base.target.find_last_of('/');
    const std::string dir =
        slash == std::string::npos ? "/" : base.target.substr(0, slash + 1);
    return origin + dir + location;
}

// Drop the secret headers (and forget they were secret) before a cross-host
// redirect hop, so a bearer minted for one origin never travels to another.
void drop_secret_headers(connect::HttpRequest& request) {
    const auto is_secret = [&request](const std::string& name) {
        return std::any_of(
            request.secret_headers.begin(), request.secret_headers.end(),
            [&name](const std::string& secret) {
                return name.size() == secret.size() &&
                       std::equal(name.begin(), name.end(), secret.begin(),
                                  [](char left, char right) {
                                      return std::tolower(static_cast<unsigned char>(left)) ==
                                             std::tolower(static_cast<unsigned char>(right));
                                  });
            });
    };
    std::erase_if(request.headers,
                  [&is_secret](const std::pair<std::string, std::string>& header) {
                      return is_secret(header.first);
                  });
    request.secret_headers.clear();
}

// Add every system root to `context`, one certificate per call (see
// system_trust_store.hpp for why). Non-throwing; returns false (+ reason) only
// if not one certificate was accepted.
bool add_system_roots(net::tls_context& context, std::string& reason) {
    const std::vector<std::string> certificates = system_ca_certificates(reason);
    if (certificates.empty()) return false;
    int added = 0;
    for (const std::string& certificate : certificates) {
        if (!context.add_certificate_authority(certificate)) ++added;  // skip a bad cert
    }
    if (added == 0) {
        reason = "no certificates parsed from the CA bundle";
        return false;
    }
    return true;
}

// --- The shared TLS contexts ---------------------------------------------------
//
// One `tls_context` per peer hostname, built once and never mutated afterwards:
// the SNI/verification hostname is per-host state, and mutating a context that a
// live stream references is undefined behavior. Contexts are cached because
// building one loads the trust store configuration; a connector talks to one
// host for the life of a query, so the cache stays tiny.
class TlsContextCache {
public:
    // Returns the context for `host`, or nullptr with `reason` set when the
    // trust store could not be configured. Non-throwing.
    std::shared_ptr<const net::tls_context> get(const std::string& host,
                                                std::string& reason) {
        const std::lock_guard<std::mutex> lock(guard_);
        const auto found = contexts_.find(host);
        if (found != contexts_.end()) return found->second;

        auto context = std::make_shared<net::tls_context>();
        context->set_hostname(host);  // SNI + hostname verification (returns void)
        // Load the system roots explicitly — set_default_verify_paths is a no-op in
        // this backend, so without this the store is empty and verification fails.
        std::string roots_reason;
        if (!add_system_roots(*context, roots_reason)) {
            reason = "could not load the system CA trust store: " + roots_reason;
            return nullptr;
        }
        if (const std::error_code error =
                context->set_verify_mode(net::tls_verify_mode::peer)) {
            reason = "could not enable peer certificate verification: " +
                     error.message();
            return nullptr;
        }
        contexts_.emplace(host, context);
        return context;
    }

private:
    std::mutex guard_;
    std::map<std::string, std::shared_ptr<const net::tls_context>> contexts_;
};

// --- The exchange ---------------------------------------------------------------

// Everything one exchange needs, kept alive by a shared_ptr so the coroutine on
// the io_context thread and the awaiting caller both hold it. `finished` is the
// cross-thread handoff: the caller suspends on it and the io_context thread sets
// it, which is the runtime interface's documented completion primitive.
struct Exchange {
    connect::HttpRequest request;
    connect::HttpResponse response;
    runtime::async_event finished;
};

// Copy the request's headers onto the message. Uses the non-throwing `set`
// overloads: this runs on the io_context thread and nothing may unwind there.
// The secret headers go on the wire exactly like any other — the guarantee is
// that no value ever comes back out in `response.error`.
void apply_headers(http::request& message, const connect::HttpRequest& request,
                   const std::string& host_value) {
    boost::system::error_code ignored;
    message.set(http::field::host, host_value, ignored);
    message.set(http::field::connection, "close", ignored);
    for (const auto& [name, value] : request.headers) {
        // A caller-supplied Host would fight the authority we dialed.
        if (name.size() == 4 &&
            std::equal(name.begin(), name.end(), "host",
                       [](char left, char right) {
                           return static_cast<char>(std::tolower(
                                      static_cast<unsigned char>(left))) == right;
                       })) {
            continue;
        }
        message.set(name, value, ignored);
    }
}

// Send the request and read the response over an already-connected stream.
// Returns the reason on failure, empty on success. Never names a secret. When the
// status is a 3xx and `redirects_remaining > 0`, captures `Location` into
// `location` and stops before the body (the caller re-dials). Otherwise the body
// streams to `request.download_to_path` when set, else into `response.body`.
template <class Stream>
runtime::io_task<std::string> exchange_over(Stream& stream,
                                            const connect::HttpRequest& request,
                                            const RequestTarget& parsed,
                                            connect::HttpResponse& response,
                                            int redirects_remaining,
                                            std::string& location) {
    const http::method method =
        http::string_to_method(request.method) == http::method::unknown
            ? http::method::get
            : http::string_to_method(request.method);

    http::request message(method, parsed.target, http::version::http_1_1);
    apply_headers(message, request, host_header_value(parsed));
    message.set_content_length(request.body.size());

    http::serializer serializer(
        http::make_serializer_config(http::serializer_config{}));
    serializer.set_message(message);
    auto sink = serializer.sink_for(stream);
    if (auto [write_error, written] = co_await sink.write_eof(
            boost::capy::make_buffer(std::string_view(request.body)));
        write_error) {
        co_return {{}, "could not send the request: " + write_error.message()};
    }

    // A caller (the weights fetch) may raise the cap above the connector default.
    const std::uint64_t body_limit =
        request.max_response_bytes != 0
            ? static_cast<std::uint64_t>(request.max_response_bytes)
            : kResponseBodyLimit;
    http::parser_config limits(/*server=*/false);
    limits.body_limit = body_limit;
    http::response_parser parser(http::make_parser_config(limits));
    parser.reset();  // prepare for a new stream — required before the first start()
    parser.start();
    if (auto [header_error] = co_await parser.read_header(stream); header_error) {
        co_return {{}, "could not read the response header: " +
                           header_error.message()};
    }
    parser.set_body_limit(body_limit);
    response.status = static_cast<int>(parser.get().status_int());

    // A redirect we will follow: capture Location and stop. The body is not the
    // payload, and `Connection: close` lets us drop the socket without draining.
    if (response.status >= 300 && response.status < 400 && redirects_remaining > 0 &&
        parser.get().count(http::field::location) > 0) {
        location = std::string(parser.get().at(http::field::location));
        if (!location.empty()) co_return {{}, std::string()};
    }

    // Stream the body to a file when asked (keeps a multi-GB download off the
    // heap), else buffer it in response.body. Only a 2xx goes to the file — an
    // error status (404, ...) is buffered so the caller sees the reason and no
    // junk file is left behind.
    std::ofstream file;
    const bool to_file = !request.download_to_path.empty() &&
                         response.status >= 200 && response.status < 300;
    if (to_file) {
        file.open(request.download_to_path,
                  std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file) {
            co_return {{}, "could not open the download file for writing"};
        }
    }

    auto body = parser.source_for(stream);
    boost::capy::const_buffer chunk[16];
    for (;;) {
        auto [pull_error, buffers] = co_await body.pull(chunk);
        if (pull_error == boost::capy::error::eof) break;
        if (pull_error) {
            co_return {{}, "could not read the response body: " +
                               pull_error.message()};
        }
        std::size_t consumed = 0;
        for (const boost::capy::const_buffer& buffer : buffers) {
            if (to_file) {
                file.write(static_cast<const char*>(buffer.data()),
                           static_cast<std::streamsize>(buffer.size()));
            } else {
                response.body.append(static_cast<const char*>(buffer.data()),
                                     buffer.size());
            }
            consumed += buffer.size();
        }
        body.consume(consumed);
    }
    if (to_file) {
        file.flush();
        if (!file) co_return {{}, "could not write the download file"};
    }
    co_return {{}, std::string()};
}

// --- The client -----------------------------------------------------------------

// One io_context on one dedicated thread multiplexes every outbound exchange in
// the process. The thread is kept alive by a keepalive coroutine (an io_context
// with no pending work returns from run() immediately), the same shape the
// cluster's network thread uses.
class NativeHttpClient {
public:
    NativeHttpClient() {
        boost::capy::run_async(io_context_.get_executor())(keepalive());
        worker_ = std::thread([this] { io_context_.run(); });
    }

    ~NativeHttpClient() { stop(); }

    NativeHttpClient(const NativeHttpClient&) = delete;
    NativeHttpClient& operator=(const NativeHttpClient&) = delete;

    void stop() {
        if (worker_.joinable()) {
            io_context_.stop();
            worker_.join();
        }
    }

    // Start one exchange on the io_context and return a task the caller awaits
    // from wherever it happens to be running.
    runtime::io_task<connect::HttpResponse> submit(connect::HttpRequest request) {
        auto exchange = std::make_shared<Exchange>();
        exchange->request = std::move(request);
        boost::capy::run_async(io_context_.get_executor())(perform(exchange));
        const auto [wait_error] = co_await exchange->finished.wait();
        // Cancellation must not read a response the I/O coroutine still owns.
        if (wait_error) co_return {wait_error, connect::HttpResponse{}};
        co_return {{}, std::move(exchange->response)};
    }

private:
    // Keeps run() from returning while the process still wants a client. A long
    // wait re-armed forever costs one timer.
    runtime::task<> keepalive() {
        net::timer timer(io_context_);
        for (;;) {
            timer.expires_after(std::chrono::hours(1));
            [[maybe_unused]] const auto waited = co_await timer.wait();
        }
    }

    // One connection to `url`: resolve, connect, (optionally) handshake, exchange.
    // Returns the failure reason (empty on success). On a followed redirect,
    // `location` is set and the body is not read.
    //
    // `deadline` bounds the WHOLE hop — resolving, connecting and the TLS
    // handshake as well as the exchange. An endpoint whose packets are dropped
    // (a default-deny egress policy) otherwise holds a connect for the kernel's
    // SYN retry budget, about 130 s on Linux, whatever timeout the caller asked
    // for (lake#801).
    runtime::task<std::string> perform_url(const std::string& url,
                                           const connect::HttpRequest& request,
                                           connect::HttpResponse& response,
                                           std::chrono::seconds deadline,
                                           int redirects_remaining,
                                           std::string& location) {
        RequestTarget parsed;
        if (!parse_request_target(url, parsed)) {
            co_return std::string("not an absolute http(s) URL");
        }

        const auto expires = std::chrono::steady_clock::now() + deadline;
        const auto remaining = [expires] {
            return std::max(std::chrono::milliseconds(0),
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                expires - std::chrono::steady_clock::now()));
        };

        net::resolver resolver(io_context_);
        auto [resolve_error, results] = co_await runtime::timeout(
            resolver.resolve(parsed.host, parsed.port), remaining());
        if (resolve_error) {
            co_return "could not resolve " + parsed.host + ": " +
                resolve_error.message();
        }

        // The destination policy (#1394): a URL a caller chose may reach only
        // public addresses, plus the classes this node allows. Each resolved
        // address is checked here and only a checked one is dialled, so the
        // connection goes to the address that passed (no second lookup a DNS
        // answer could change), and every redirect hop comes back through here.
        const bool public_only =
            request.destinations == connect::DestinationPolicy::PublicOnly;
        const connect::AllowedDestinations allowed =
            public_only ? connect::allowed_destinations() : connect::AllowedDestinations{};
        bool any_allowed = !public_only;
        std::string refused_reason;

        net::tcp_socket socket(io_context_);
        socket.open();
        bool connected = false;
        std::string connect_reason = "no address resolved for " + parsed.host;
        for (const auto& entry : results) {
            const net::endpoint endpoint = entry.get_endpoint();
            if (public_only) {
                const std::string address = endpoint.is_v4()
                                                ? endpoint.v4_address().to_string()
                                                : endpoint.v6_address().to_string();
                const connect::AddressClass kind = connect::classify_address(address);
                if (!connect::destination_allowed(kind, allowed)) {
                    if (refused_reason.empty()) {
                        refused_reason =
                            connect::destination_refused_reason(parsed.host, address, kind);
                    }
                    continue;
                }
                any_allowed = true;
            }
            auto [connect_error] = co_await runtime::timeout(
                socket.connect(endpoint), remaining());
            if (!connect_error) {
                connected = true;
                break;
            }
            connect_reason = "could not connect to " + parsed.host + ":" +
                             parsed.port + ": " + connect_error.message();
            if (remaining() == std::chrono::milliseconds(0)) {
                connect_reason += " (no answer within " +
                                  std::to_string(deadline.count()) + " s)";
                break;
            }
        }
        if (!any_allowed && !refused_reason.empty()) co_return refused_reason;
        if (!connected) co_return connect_reason;

        if (!parsed.secure) {
            // Plaintext is only reachable for an explicitly http:// URL — a
            // loopback fixture or a sidecar proxy. Nothing constructs one for a
            // real source.
            auto [error, reason] = co_await runtime::timeout(
                exchange_over(socket, request, parsed, response,
                              redirects_remaining, location),
                remaining());
            co_return error ? timeout_reason(error) : reason;
        }

        std::string tls_reason;
        const std::shared_ptr<const net::tls_context> tls_context =
            tls_contexts().get(parsed.host, tls_reason);
        if (!tls_context) co_return tls_reason;

        // Reference mode: `socket` outlives `secure` (both live in this frame).
        net::openssl_stream secure(&socket, *tls_context);
        if (auto [handshake_error] = co_await runtime::timeout(
                secure.handshake(net::tls_stream::client), remaining());
            handshake_error) {
            co_return "TLS handshake with " + parsed.host +
                " failed: " + handshake_error.message();
        }

        auto [error, reason] = co_await runtime::timeout(
            exchange_over(secure, request, parsed, response, redirects_remaining,
                          location),
            remaining());

        // Best-effort close_notify: the answer is already in hand, so a peer
        // that hangs up first — or never answers within the deadline — is not
        // an error worth reporting.
        [[maybe_unused]] const auto shutdown_result =
            co_await runtime::timeout(secure.shutdown(), remaining());
        co_return error ? timeout_reason(error) : reason;
    }

    // Drive the exchange, following up to `max_redirects` `Location` hops. Sets
    // `finished` exactly once, on every path.
    runtime::task<> perform(std::shared_ptr<Exchange> exchange) {
        connect::HttpResponse& response = exchange->response;
        const std::chrono::seconds deadline(
            exchange->request.timeout_seconds > 0
                ? exchange->request.timeout_seconds
                : 60);

        connect::HttpRequest hop = exchange->request;  // url + headers may change
        int redirects_remaining = exchange->request.max_redirects;
        for (;;) {
            std::string location;
            std::string reason = co_await perform_url(
                hop.url, hop, response, deadline, redirects_remaining, location);
            if (!reason.empty()) {
                response.error = reason;
                break;
            }
            if (location.empty() || redirects_remaining <= 0) break;  // final answer

            const std::string next = resolve_location(hop.url, location);
            RequestTarget before, after;
            const bool cross_host =
                parse_request_target(hop.url, before) &&
                parse_request_target(next, after) && before.host != after.host;
            hop.url = next;
            if (cross_host) drop_secret_headers(hop);
            response.status = 0;
            response.body.clear();
            --redirects_remaining;
        }
        exchange->finished.set();
    }

    static std::string timeout_reason(const std::error_code& error) {
        return "the request timed out: " + error.message();
    }

    static TlsContextCache& tls_contexts() {
        static TlsContextCache cache;
        return cache;
    }

    net::io_context io_context_;
    std::thread worker_;
};

// The process-wide client. A function-local static so nothing is constructed
// (and no thread started) in a build that never installs the transport.
std::unique_ptr<NativeHttpClient>& client_slot() {
    static std::unique_ptr<NativeHttpClient> client;
    return client;
}

}  // namespace

void install_native_http_transport() {
    std::unique_ptr<NativeHttpClient>& client = client_slot();
    if (!client) client = std::make_unique<NativeHttpClient>();
    NativeHttpClient* running = client.get();

    connect::AsyncHttpTransport asynchronous =
        [running](const connect::HttpRequest& request)
        -> runtime::io_task<connect::HttpResponse> {
        return running->submit(request);
    };
    // The synchronous shape is the async one driven to completion on the
    // caller's thread — one client, one code path, no second implementation.
    connect::HttpTransport synchronous =
        [running](const connect::HttpRequest& request) -> connect::HttpResponse {
        auto [error, response] = runtime::block_on(running->submit(request));
        (void)error;
        return std::move(response);
    };
    connect::set_default_http_transport(std::move(synchronous),
                                        std::move(asynchronous));
}

void shutdown_native_http_transport() {
    connect::set_default_http_transport({}, {});
    std::unique_ptr<NativeHttpClient>& client = client_slot();
    if (client) {
        client->stop();
        client.reset();
    }
}

}  // namespace zetta::api
