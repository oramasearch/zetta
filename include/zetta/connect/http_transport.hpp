#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

// The **outbound HTTP transport seam** for source connectors
// ([#827](https://github.com/oramasearch/zetta/issues/827)).
//
// zetta-core embeds no HTTP client library and no warehouse driver (ADR-17): a
// connector expresses *what* request to make, and the transport that actually
// performs it is **injected** — exactly the [`CommandRunner`](../cloud/command_runner.hpp)
// pattern the CLI-based cloud provisioners use. That keeps every connector
// unit-testable against a fake (no network, no credentials, no warehouse) and
// keeps the real egress path in one replaceable place.
//
// The default transport shells `curl` through the shared `CommandRunner` (no
// shell, argv straight to exec) — the same "drive the vendor's CLI" shape as the
// `aws` / `gcloud` / `az` control planes. A native, coroutine-backed transport
// behind `zetta::runtime::net` can replace it later without touching a single
// connector, because both sides only ever see `HttpRequest` / `HttpResponse`.
//
// **Credentials never ride in argv.** The default transport writes auth headers
// to a private, 0600 temp config file consumed via `curl --config`, so a token is
// never visible in the process table. See docs/data/connector-query.md.
namespace zetta::connect {

// One outbound HTTP exchange, fully described. `headers` are name/value pairs
// sent verbatim; `body` is the request entity (empty for GET).
struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_seconds = 60;

    // Headers whose values are secret (an Authorization bearer, say). The
    // default transport routes exactly these through its private config file
    // instead of argv. Names are matched case-insensitively against `headers`.
    std::vector<std::string> secret_headers;
};

// The result of one exchange. `error` is a **transport-level** failure (DNS,
// TLS, timeout, curl missing) — distinct from an HTTP status the peer returned,
// which arrives as a normal `status` + `body` for the caller to interpret.
struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;

    // The exchange completed AND the peer answered 2xx.
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// The injected transport seam. An empty HttpTransport means "use the default".
using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;

// The default real transport: performs the exchange with `curl` through the
// shared CommandRunner. Secret headers go through a 0600 temp config file, never
// argv. Returns a transport `error` when curl is absent or the exchange fails.
HttpTransport default_http_transport();

}  // namespace zetta::connect
