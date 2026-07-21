# The native outbound HTTP client

zetta's **outbound** HTTP is a coroutine, not a subprocess. The client that fills
the [`connect::HttpTransport`](../data/connector-query.md) seam dials Corosio
sockets, wraps them in OpenSSL, and frames HTTP/1.1 with Boost.HTTP's sans-I/O
serializer and parser ([#827](https://github.com/oramasearch/zetta/issues/827)).

```
include/zetta/connect/http_transport.hpp     the seam: HttpRequest/HttpResponse, both transport shapes, install/read
src/core/connect/http_transport.cpp          the registry (zetta-core; holds no HTTP code at all)
include/zetta/api/native_http_transport.hpp  install_native_http_transport / shutdown_native_http_transport
src/core/api/native_http_transport.cpp       the client (zetta-api target)
tests/connect/test_native_http_transport.cpp real loopback exchanges + a when_all fan-out
```

## Why it is split across two targets

`zetta-core` deliberately links **neither Boost.HTTP nor Beast2** — the
`zetta-api` target exists to hold the HTTP adapters so no HTTP type reaches the
core's public surface (the same isolation SDL and CUDA get). So the seam is
*declared* in core and *implemented* in `zetta-api`:

```
zetta-core   include/zetta/connect/http_transport.hpp   declares HttpRequest/HttpResponse/HttpTransport
             src/core/connect/http_transport.cpp        holds the installed default (a registry, nothing more)
                     ▲  install_native_http_transport()
zetta-api    src/core/api/native_http_transport.cpp     the coroutine client: Corosio + OpenSSL + Boost.HTTP
```

`src/main.cpp` calls `api::install_native_http_transport()` before anything can
serve a connector query, and `api::shutdown_native_http_transport()` after the
node stops. A build that never installs one (a unit test, a tool with no egress)
has **no** default transport, and `run_connector_query` fails with a
path-qualified reason rather than reaching the network by accident.

This makes `src/core/api/native_http_transport.cpp` the **second** translation
unit allowed to name Corosio, alongside `src/core/cluster/cluster.cpp`. The split
is by direction: the cluster adapter owns inbound/peer traffic, this one owns
outbound client traffic. Both go through `zetta::runtime::net`; neither names
`boost::corosio` outside it.

## The two shapes of the seam

```cpp
using HttpTransport      = std::function<HttpResponse(const HttpRequest&)>;
using AsyncHttpTransport = std::function<runtime::io_task<HttpResponse>(const HttpRequest&)>;
```

Both are installed together and backed by the **same** client — the synchronous
one is the async one driven to completion through `runtime::block_on`, so there
is one code path, not two implementations. The async shape exists because a
sequential transport structurally cannot use a cluster's aggregate bandwidth: it
is what the distributed connector fetch fans out with, N partition reads in
flight at once under `runtime::when_all`.

`when_all` and `timeout` were added to `include/zetta/runtime.hpp` for exactly
this — the rule is *extend the seam, don't work around it*, so no consumer names
`boost::capy::when_all`.

## How one exchange runs

One `io_context` on one dedicated thread multiplexes **every** outbound exchange
in the process, so installing the client costs one thread regardless of how many
requests are in flight. A caller's `submit()` posts the exchange coroutine onto
that context and suspends on a `runtime::async_event`; the io_context thread sets
the event when the response is in hand. That is the runtime seam's documented
cross-thread completion primitive — the caller never has to be running on the
io_context.

```
resolve (async DNS)  →  connect  →  TLS handshake  →  serialize request
                                                   →  parse response header
                                                   →  stream the body out of the parser
                                                   →  close_notify
```

- **TLS**: one `tls_context` per peer hostname, built once and never mutated
  afterwards (SNI/verification hostname is per-host state, and mutating a context
  a live stream references is undefined behavior). Verification is
  `tls_verify_mode::peer` against the system CA trust store — a connector talks
  to a public warehouse endpoint, so this is the public-CA path, not the
  cluster's private join-token CA.
- **Framing**: `boost::http::serializer` writes the request through a sink over
  the stream; `boost::http::response_parser` reads the header, then the body is
  pulled incrementally through `source_for()` so a large result partition is
  never required to fit in the parser's buffer. The per-message body limit is
  raised to 512 MiB (the client default of 1 MiB would truncate a warehouse
  result partition).
- **Deadline**: every exchange runs under `runtime::timeout`
  (`HttpRequest::timeout_seconds`), so a hung peer cannot pin a coroutine — and
  therefore a fan-out slot — forever.
- **Connections**: one per exchange, `Connection: close`, no pooling. Requests
  fan out concurrently rather than pipelining, so a pool would buy little and
  cost a lifetime problem; the price is one handshake per request, which is why
  callers bound their fan-out.

## The security contract

`HttpRequest::secret_headers` names the headers whose values are secret. The
client sends them and then forgets them:

- every failure reason is built from the **host, port, and the OS/TLS error
  message** only — never from a header or a request body;
- there is no subprocess, so nothing is visible in the process table;
- a caller-supplied `Host` header is dropped, so it cannot disagree with the
  authority actually dialed;
- a URL carrying userinfo (`https://user:pass@host/`) is rejected outright.

`tests/connect/test_native_http_transport.cpp` asserts the no-leak property
directly: an unreachable host with a bearer set must produce a reason that does
not contain the token.

## Boundaries

No new JSON or wire format — the client speaks HTTP/1.1 and exchanges only
`HttpRequest` / `HttpResponse`. URL parsing is deliberately narrow: absolute
`http://` or `https://` only, no userinfo, no fragment, no relative form (a
connector always builds an absolute URL from a validated config). A plaintext
`http://` URL is reachable only when a caller explicitly writes one — a loopback
fixture or a sidecar proxy; nothing constructs one for a real source.

## Scope notes / later slices

- **No connection pooling** and **no HTTP/2** — both are changes behind the same
  seam, invisible to connectors.
- **TLS is not directly tested**: verifying it needs a certificate authority and
  a real peer. The loopback suite exercises the identical coroutine over a plain
  socket, and the Corosio TLS surface itself is covered by the cluster's mTLS
  peer transport.
- **No proxy support** (`HTTP_PROXY` / `HTTPS_PROXY` are ignored).

See also [../data/connector-query.md](../data/connector-query.md) (what the
transport is for) and [../cluster/README.md](../cluster/README.md) (the inbound
half of the networking seam).
