# Beast2 — coroutine-native HTTP for the API surface

Provenance: read directly from `cppalliance/beast2` @ develop (shallow clone,
last commit 2026-05-15) + `cppalliance/http` @ develop (2026-06-14), plus web
search (thin — the stack is too new for good coverage; P4100R0
"Coroutine-Native I/O for C++29" is the standards-track echo of the design).
Date: 2026-07-02. Status: **decision input for #615** — Beast2 chosen as the
HTTP layer behind the API seam.

## What it is

The C++ Alliance's post-Asio stack, coroutines-only, the same family zetta
already vendors for its runtime:

| Library | Role | zetta status |
|---|---|---|
| capy | `task<T>`, executors, buffers, `run_async` | **vendored** (`modules/capy`) — backs `zetta::runtime` |
| corosio | `io_context`, TCP, TLS, timers, signal_set | **vendored** (`modules/corosio`) — backs `zetta::runtime::net` |
| http (`cppalliance/http`) | sans-I/O HTTP/1 parse/serialize + router + middleware (cors, static, json_sink) | new dependency |
| **beast2** | high-level HTTP/1 + WebSocket **server** on all of the above | new dependency |

Using Beast2 means the API server rides the SAME coroutine runtime as the
cluster — one async model in the whole binary, no second event-loop paradigm
(which is what cpp-httplib would have smuggled in).

## The server API (read from `example/server/main.cpp`)

```cpp
corosio::io_context ioc;
http::router rr;
rr.use("/mcp", [&](http::route_params& rp) -> http::route_task {
    if (rp.req.method() != http::method::post) co_return http::route_next;
    http::json_sink js;                                  // Boost.JSON body sink
    auto [ec, n] = co_await capy::push_to(rp.req_body, js);
    if (ec) co_return http::route_error(ec);
    json::value request = js.release();
    // ... dispatch, then:
    rp.status(http::status::ok);
    rp.res.set(http::field::content_type, "application/json");
    auto [wec] = co_await rp.send(serialized_response);
    co_return http::route_done;
});
beast2::http_server server(ioc, num_workers, std::move(rr),
    http::make_parser_config(http::parser_config(true)),
    http::make_serializer_config(http::serializer_config()));
server.bind(corosio::endpoint(addr, port));
server.start();                       // corosio::signal_set for shutdown
```

Also available: `https_server` + `corosio::tls_context` (when corosio is built
with OpenSSL — feeds #466's TLS story later), `http::cors()`,
`http::serve_static`, and a JSON-RPC **client** example
(`example/client/jsonrpc/`) confirming the shape is intended for exactly our
protocol.

## Dependencies + vendoring

Beast2's CMake needs Boost components: `capy corosio http json program_options
scope system url` — via `find_package(Boost)` or an in-tree boost
(`BOOST_SRC_DIR` + `add_subdirectory`). zetta already vendors capy + corosio;
the additions are `cppalliance/http`, `Boost.JSON`, `Boost.URL` (+ small
transitive libs). Two vendoring options for #615, in preference order:

1. **Submodules, like capy/corosio**: `modules/http`, `modules/beast2`, plus
   the minimal boost libs they need — matching how the runtime stack is
   already pinned. Verify the standalone (non-superproject) build actually
   closes over transitive deps before committing to it.
2. If the standalone closure fights back: a pinned boost subset via
   `BOOST_SRC_DIR`.

**JSON impedance:** beast2/http speak **Boost.JSON**; zetta speaks
nlohmann/json everywhere. Conversion happens ONCE, in the HTTP adapter
(serialize/parse at the boundary) — Boost.JSON must not leak past the seam.

## As vendored (2026-07-02, #615)

Option 1 shipped: `modules/http` + `modules/beast2` submodules; the remaining
Boost libs (json, url + 12 header-only transitives) via the official
boost-1.91.0 cmake tarball (FetchContent, `BOOST_INCLUDE_LIBRARIES`).
`modules/http` points at the **oramasearch/http fork**, branch `zetta-patches`
(one patch: portable file-time→epoch conversion; libc++ lacks
`std::chrono::clock_cast`) — upstream the patch when practical.

## Risk + the seam that contains it

Beta, develop-branch, no tags (same maturity class as capy/corosio, which
zetta already accepted — and the same author/org, moving together). The
containment is zetta's existing rule: **backend names stay behind seams** —
exactly one adapter TU names beast2/http types (`src/core/api/http_server.cpp`,
the API sibling of `src/core/cluster/cluster.cpp` for corosio). If Beast2's
API shifts, or we ever want Asio+Beast1, the adapter is the blast radius.

Follow-ups this enables later: WebSocket push (activity events to the GUI or
to MCP SSE) on the same stack; TLS via `https_server` for #466.

See also: [coroutines.md](coroutines.md) (the original runtime deep-dive that
chose Capy/Corosio — this doc extends that decision to HTTP).
