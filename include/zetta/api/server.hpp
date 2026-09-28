#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "zetta/api/registry.hpp"
#include "zetta/observability/trace.hpp"

namespace zetta {
class Node;
}
namespace zetta::cluster {
class Cluster;
}

// The embedded MCP server (docs/api/registry-design.md, #615): Streamable-HTTP
// JSON-RPC on POST /mcp, serving the capability registry. The HTTP stack is
// Beast2 on the same capy/corosio runtime as the cluster — confined entirely
// to the adapter TU (src/core/api/http_server.cpp); no beast2/http/Boost.JSON
// type appears here or anywhere else (docs/research/beast2-http.md).
//
// Threading: the server's io_context runs on its own thread; every handler
// marshals onto the loop that owns the Node via the MainThreadBridge the
// server owns — the owning loop (headless tick / headed frame) must call
// drain() each tick, right beside Node::tick().
namespace zetta::api {

struct ServerOptions {
    std::string listen_address = "127.0.0.1";  // loopback by default
    unsigned short port = 4800;                // 0 = pick an ephemeral port
    // Connections served at once: each worker coroutine serves one connection at
    // a time, and a long poll (receive_events) holds its connection for the whole
    // wait. At 32, 32 bus long polls left every other request queued for up to
    // 10 s (#1063). Workers are coroutines, not threads, so this is cheap.
    int worker_coroutines = 256;
    // The built Studio web app (the design repo's React bundle). When set, the
    // node serves it statically at /app with an SPA fallback to index.html —
    // the app's only data path is the /v1 surface. Empty = /app is 404.
    std::string webapp_dir;

    // Clerk issuer (#889): when set, a Clerk session JWT is accepted as a bearer
    // (verified against `<clerk_issuer>/.well-known/jwks.json`), mapped to a
    // principal named clerk:<sub>. Clerk is the preferred auth; the native token
    // store / zetta_session cookie stays the fallback. EMPTY = bearer-only, so
    // an on-prem / air-gapped node needs no Clerk egress.
    std::string clerk_issuer;
    // The default-deny auth gate is ALWAYS on (#799) — there is no toggle and no
    // legacy shared-secret. EVERY gated route is closed unless the request
    // carries a bearer that resolves, via the token store (#744: salted-hash +
    // constant-time compare, honoring expiry/revocation), to an active user;
    // anything else is 401. The resolved user becomes the acting identity, and
    // the capability check runs against that user's effective permissions. Seed
    // the first credential with --admin-token (the bootstrap owner, #733); a
    // fresh node with none generates + prints one so it is never locked out. The
    // in-process headed-GUI console authorizes locally, not through this gate.
    // See docs/governance/access-control.md.
};

class ApiServer {
public:
    // The server owns its Registry (populated with the core capabilities) and
    // a MainThreadBridge; spans land in the Node's own SpanRecorder (the one
    // ring behind system.traces / system.spans, #640). `cluster` (optional)
    // enables the system.nodes view through the sql capability.
    ApiServer(Node& node, const zetta::cluster::Cluster* cluster,
              ServerOptions options);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Bind + start serving on the io thread. False on failure (see reason()).
    bool start();
    void stop();
    bool running() const;

    // The actual bound port (differs from options when port was 0).
    unsigned short port() const;
    const std::string& reason() const;

    // Called by the loop that owns the Node, each tick/frame.
    void drain();

    // The owning loop's idle wait (headless): blocks up to `max_idle` but wakes
    // the instant a request is enqueued, so its drain runs immediately instead of
    // on the next 10 Hz tick (#731). Use in place of an unconditional sleep.
    void wait_for_work(std::chrono::milliseconds max_idle);

    // Register a wakeup the owning loop provides so it drains on enqueue (headed:
    // push an SDL event so its event wait returns early). Call before start().
    void set_wakeup(std::function<void()> wakeup);

    // Wake the owning loop now, from any thread, so its next tick runs without
    // waiting out the idle interval (Node::set_owner_wakeup, #1063).
    void wake();

    Registry& registry();
    // The node's span ring (convenience passthrough to Node::span_recorder()).
    observability::SpanRecorder& recorder();
    void set_activity_hook(ActivityHook hook);
    // Wire the provision targets the consent-gated add_nodes capability may
    // drive (#621). The resolved Provisioners must outlive the server; call
    // before start(). Unwired targets fail with a clear reason — nothing is
    // provisioned by default.
    void set_provisioner_resolver(ProvisionerResolver resolver);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zetta::api
