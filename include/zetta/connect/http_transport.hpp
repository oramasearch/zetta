#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/connect/destination_policy.hpp"
#include "zetta/runtime.hpp"

// The **outbound HTTP transport interface** for source connectors
// ([#827](https://github.com/oramasearch/zetta/issues/827)).
//
// zetta-core embeds no HTTP client library and no warehouse driver (ADR-17): a
// connector expresses *what* request to make, and the transport that actually
// performs it is **injected**. That keeps every connector unit-testable against a
// fake (no network, no credentials, no warehouse) and keeps the real egress path
// in one replaceable place.
//
// **The real transport lives in `zetta-api`, not here.** zetta-core deliberately
// does not link Beast2/Boost.HTTP — that isolation is the whole point of the
// `zetta-api` target — so the native coroutine HTTPS client is implemented in
// `src/core/api/native_http_transport.cpp` and **installed** as this injected dependency at
// startup (`api::install_native_http_transport`, called from `src/main.cpp`).
// zetta-core therefore never sees an HTTP type: only `HttpRequest` /
// `HttpResponse` cross the boundary. A build that never installs one (a unit
// test, a headless tool) simply has no default transport, and a connector call
// that needs one fails cleanly rather than reaching the network by accident.
//
// **Two shapes, one interface.** `HttpTransport` is the synchronous shape a
// single-request caller wants; `AsyncHttpTransport` is the coroutine shape the
// distributed fetch fans out with (`runtime::when_all` over N in-flight
// exchanges). The installed pair is backed by the same client.
//
// **Credentials never ride in a log line.** `HttpRequest::secret_headers` names
// the headers whose values are secret (the bearer token); no transport may put
// such a value in an error string, a span attribute, or a log. See
// docs/data/connector-query.md.
namespace zetta::connect {

// One outbound HTTP exchange, fully described. `headers` are name/value pairs
// sent verbatim; `body` is the request entity (empty for GET).
struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_seconds = 60;

    // Headers whose values are secret (an Authorization bearer, say). A
    // transport must send them and then forget them: they may never appear in a
    // reason string, a log line, or a process argument. Names are matched
    // case-insensitively against `headers`.
    std::vector<std::string> secret_headers;

    // --- Large-download controls (used by the native model-weights fetch,
    // connect/model_fetch.hpp). All default to off, so a connector is unchanged.

    // Follow up to this many 3xx redirects. Each hop re-dials the `Location`
    // (a new host + TLS is fine), dropping the secret headers on a cross-host
    // hop so a bearer never reaches a different origin. 0 = return the 3xx as-is.
    // A HuggingFace LFS URL 302-redirects to a CDN, so a weights fetch needs it.
    int max_redirects = 0;

    // Stream the 2xx response body straight to this file instead of buffering it
    // in `HttpResponse::body` (which stays empty). Keeps a multi-GB download off
    // the heap. A non-2xx, or a body that is only a redirect, writes nothing.
    std::string download_to_path;

    // Raise the transport's default response-body cap (bytes); 0 keeps the
    // default. A model shard exceeds the connector default, so the fetch lifts it.
    std::size_t max_response_bytes = 0;

    // Which resolved addresses may be dialled (#1394, #1414).
    // `Connector` checks the separate exact connector authority allowance on
    // every resolved address and every redirect hop; metadata, link-local and
    // reserved addresses never pass that policy. It requires HTTPS unless
    // `connector_plaintext` says the connector's configured protocol is http.
    // `PublicOnly` is for a URL a caller chose (an action connector's endpoint): the
    // transport resolves the host once, refuses loopback, private, link-local,
    // metadata and reserved addresses unless the node allows their class, connects
    // only to an address it checked, and checks again on every redirect hop.
    DestinationPolicy destinations = DestinationPolicy::Any;

    // With `DestinationPolicy::Connector` only: the connector was configured with
    // an explicit `http://` URL (a lake service inside the cluster, #1414), so a
    // plaintext hop is allowed. False (the default) keeps a Connector request on
    // HTTPS on every hop, so an HTTPS source cannot be redirected to plaintext.
    bool connector_plaintext = false;

    // Stream a 2xx body to a caller-owned destination instead of opening a path
    // (#1412). Takes precedence over download_to_path; false reports a write
    // failure. The callback and its destination must survive the exchange.
    std::function<bool(std::string_view)> download_sink;
};

// The result of one exchange. `error` is a **transport-level** failure (DNS,
// TLS, timeout, connection reset) — distinct from an HTTP status the peer
// returned, which arrives as a normal `status` + `body` for the caller to
// interpret.
struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;

    // The exchange completed AND the peer answered 2xx.
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// The injected transport, synchronous shape. An empty HttpTransport means
// "use the installed default".
using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;

// The injected transport, coroutine shape — the one the distributed fetch
// fans out with. Returns an `io_task` so it composes under `runtime::when_all` /
// `runtime::timeout`; the error_code half is always clear (a transport failure
// is reported in `HttpResponse::error`, keeping the non-throwing contract in one
// place).
using AsyncHttpTransport =
    std::function<runtime::io_task<HttpResponse>(const HttpRequest&)>;

// Install the process-wide default transport pair. Called once at startup by
// `api::install_native_http_transport`. Passing empty functions uninstalls,
// which is what a unit test wants so a stray connector call cannot reach the
// network.
void set_default_http_transport(HttpTransport synchronous,
                                AsyncHttpTransport asynchronous);

// The installed defaults. **Empty when nothing was installed** — a caller must
// check before invoking (`run_connector_query` turns an empty one into a
// path-qualified reason rather than a crash).
const HttpTransport& default_http_transport();
const AsyncHttpTransport& default_async_http_transport();

}  // namespace zetta::connect
