#pragma once

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
    // When set, every request must carry "Authorization: Bearer <token>", and
    // the token doubles as the acting AccessKey id (registry-design.md: the
    // bearer token IS the key). Empty = the audited "unauthenticated local"
    // session — only sane on a loopback bind.
    std::string bearer_token;
    int worker_coroutines = 4;
};

class ApiServer {
public:
    // The server owns its Registry (populated with the core capabilities), a
    // MainThreadBridge, and a SpanRecorder; `cluster` (optional) enables the
    // system.nodes view through the sql capability.
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

    Registry& registry();
    observability::SpanRecorder& recorder();
    void set_activity_hook(ActivityHook hook);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zetta::api
