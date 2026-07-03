#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/capability.hpp"
#include "zetta/observability/trace.hpp"

namespace zetta {
class Node;
}
namespace zetta::cluster {
class Cluster;
}

// The capability registry — the single declaration point for the programmatic
// control surface (docs/api/registry-design.md, #614/#615). Each Capability is
// registered exactly once and rendered by thin layers: the MCP server lists +
// calls entries, the REST layer (#618) derives routes + OpenAPI from the same
// schemas, and the command palette (#625) is the same list rendered for humans.
//
// The registry itself is HTTP-free (zetta-core); the Beast2 server adapter
// (src/core/api/http_server.cpp) is its first consumer. Handlers are invoked
// on the loop that owns the Node (via MainThreadBridge — see bridge.hpp), are
// non-throwing, and return ApiResult — domain failures carry the engine's
// path-qualified reason verbatim, never a laundered "operation failed".
namespace zetta::api {

// The non-throwing call outcome. `error` codes follow the JSON-RPC mapping in
// registry-design.md: schema/protocol problems become protocol errors at the
// server layer; domain failures stay results with `ok == false` so the caller
// (an agent) can read the reason and adapt.
struct ApiError {
    std::string code;     // "invalid_input" | "unauthorized" | "not_found" |
                          // "engine_error" | "unknown_capability" | "busy"
    std::string message;  // the engine's reason, verbatim where one exists
    std::string path;     // JSON pointer / field path when the input is at fault
};

struct ApiResult {
    bool ok = false;
    nlohmann::json value;  // capability output on success (documented shape)
    ApiError error;        // populated when !ok

    // Observability, threaded back through every surface (the MCP response
    // tells the agent what happened and where to read more — get_trace).
    std::string trace_id;
    std::string span_id;
    double duration_ms = 0.0;

    static ApiResult success(nlohmann::json output) {
        ApiResult result;
        result.ok = true;
        result.value = std::move(output);
        return result;
    }
    static ApiResult failure(std::string code, std::string message,
                             std::string path = {}) {
        ApiResult result;
        result.ok = false;
        result.error = ApiError{std::move(code), std::move(message), std::move(path)};
        return result;
    }
};

// read: no state change. action: mutates and completes within a tick.
// job: submits work and returns the durable engine handle (a Run or ComputeJob
// id) immediately — long work is never awaited inside a call
// (registry-design.md → "no new job infrastructure").
enum class CapabilityKind { Read, Action, Job };

std::string_view to_string(CapabilityKind kind);

class BackgroundActions;

// What a handler sees. `acting_key` is the AccessKey id the bearer token
// mapped to (empty = the audited "unauthenticated local" session — the
// governed-console precedent). The context is only ever constructed on the
// loop that owns the Node, so handlers may touch the node freely but must be
// quick (they run inside a tick).
struct ApiContext {
    Node& node;
    const zetta::cluster::Cluster* cluster = nullptr;  // for system.nodes
    std::string acting_key;                             // AccessKey id or empty

    // The trace this call belongs to (an inbound traceparent, or a fresh root
    // made by the server layer) and the node's span ring. Null recorder =
    // spans become no-ops (bare library use).
    zetta::observability::TraceContext trace;
    zetta::observability::SpanRecorder* recorder = nullptr;

    // Owner for the worker handles `kind: job` capabilities launch (#617,
    // background.hpp). Null = job capabilities fail with a clear reason (an
    // embedding that offers them must prune this each tick, beside the bridge
    // drain).
    BackgroundActions* background = nullptr;
};

using CapabilityHandler =
    std::function<ApiResult(const nlohmann::json& input, ApiContext& context)>;

// One entry on the control surface. `input_schema` is draft-07 JSON Schema
// (schema-as-code — declared next to the handler, validated at the boundary
// with the same validator discipline as the Run record). `description` must
// be one agent-actionable sentence that states side effects.
struct Capability {
    std::string name;         // snake_case: verb-first actions, noun reads
    std::string title;
    std::string description;
    nlohmann::json input_schema;
    CapabilityKind kind = CapabilityKind::Read;
    zetta::governance::Operation required_operation;  // api:query / orchestration:<name>
    bool mutates = false;           // fires the activity hook on success
    bool requires_consent = false;  // cloud-touching: consent challenge (#621)
    CapabilityHandler handler;
};

// Fired after any successful `mutates` call so an embedding GUI can refresh
// immediately and record the action (#620/#622). Invoked on the owning loop.
struct ActivityEvent {
    std::string capability;  // the entry's name
    std::string actor;       // acting key id, or "local"
};
using ActivityHook = std::function<void(const ActivityEvent&)>;

// The registry. Not thread-safe by design: it is populated at startup and
// then only read; calls arrive on the owning loop via the bridge.
class Registry {
public:
    // Registers an entry. Returns false (and registers nothing) on a
    // duplicate name or a missing handler.
    bool add(Capability capability);

    std::shared_ptr<const Capability> find(const std::string& name) const;
    std::vector<std::shared_ptr<const Capability>> list() const;
    std::size_t size() const { return entries_.size(); }

    void set_activity_hook(ActivityHook hook) { activity_ = std::move(hook); }

    // The one call path every rendering uses: resolve -> validate the input
    // against the entry's schema -> authorize (capability check; every call is
    // audited as a data_access event with the decision) -> invoke the handler
    // -> fire the activity hook when a mutating call succeeds. Non-throwing.
    ApiResult call(const std::string& name, const nlohmann::json& input,
                   ApiContext& context) const;

private:
    std::vector<std::shared_ptr<const Capability>> entries_;
    ActivityHook activity_;
};

}  // namespace zetta::api
