// SPDX-License-Identifier: MIT

#pragma once

// The **native outbound HTTPS client** that fills the
// [`connect::HttpTransport`](../connect/http_transport.hpp) seam
// ([#827](https://github.com/oramasearch/zetta/issues/827)).
//
// **Why it lives in `zetta-api` and not in `zetta-core`.** zetta-core links
// neither Boost.HTTP nor Beast2 — the `zetta-api` target exists precisely to
// hold the HTTP adapters so no HTTP type reaches the core's public surface (the
// same isolation SDL and CUDA get). The connector seam therefore *declares* the
// transport in core and this target *implements* it: a coroutine HTTPS client
// over Corosio sockets + OpenSSL TLS, framing HTTP/1.1 with Boost.HTTP's
// sans-I/O serializer/parser. Nothing here is visible to a connector, which only
// ever sees `HttpRequest` / `HttpResponse`.
//
// **No curl, no subprocess.** zetta is a coroutine-native codebase; an outbound
// exchange is a coroutine like every other I/O in the engine, so N exchanges fan
// out under `runtime::when_all` on one io_context instead of forking N
// processes. That is what makes the distributed connector fetch able to use the
// cluster's aggregate bandwidth.
//
// See docs/data/connector-query.md and docs/api/native-http-transport.md.
namespace zetta::api {

// Install the native client as the process-wide default transport pair
// (`connect::set_default_http_transport`). Call once, early in `main()`, before
// anything can run a connector query. Idempotent: a second call re-installs the
// same singleton client.
//
// The client owns one `io_context` on one dedicated thread; every outbound
// exchange is multiplexed on it, so installing costs one thread for the process
// regardless of how many requests are in flight.
void install_native_http_transport();

// Stop the client's io_context and join its thread, and uninstall the transport
// pair. Call during shutdown, after the last connector query. Safe to call when
// nothing was installed.
void shutdown_native_http_transport();

}  // namespace zetta::api
