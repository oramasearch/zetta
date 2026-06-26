#pragma once

#include <optional>
#include <string>
#include <string_view>

// The **signal plane**: a typed, ephemeral message layer carried over the
// equal-peer cluster transport (alongside replication gossip + large-object
// transfer). A signal is one of:
//
//   - Request  — an RPC call to a peer; the peer replies with a Response.
//   - Response — the reply to a Request, correlated by `id`.
//   - Event    — a fire-and-forget notification (unicast or broadcast).
//   - Task     — a fire-and-forget work envelope (the substrate for job
//                dispatch / work-stealing).
//
// Signals are **ephemeral**: they are NOT routed through the replicated CRDT
// document — they exist only for the lifetime of the message. This is the
// substrate that collectives, the work-stealing scheduler, and the job
// coordinator build on (see docs/signal-plane.md). The transport lives in
// `zetta::Cluster` (src/core/cluster.cpp); the envelope format + its
// boundary validation live here (src/core/signal.cpp).
namespace zetta {

// The four signal kinds. Ordered request→response→event→task.
enum class SignalKind { Request, Response, Event, Task };

// Lowercase wire name ("request"/"response"/"event"/"task").
std::string_view to_string(SignalKind kind);

// Parse a wire name back to a SignalKind (nullopt on anything unknown).
std::optional<SignalKind> signal_kind_from_string(std::string_view name);

// One signal, as delivered to a handler or constructed to send. `payload` is an
// opaque, application-defined byte string (often JSON, but the signal plane does
// not interpret it). `method` is an application-defined verb (the RPC method /
// event name / task type). `id` correlates a Request with its Response. `from`
// is the sender's node id, filled in on receipt (empty when constructed to send).
struct Signal {
    SignalKind kind = SignalKind::Event;
    std::string id;       // correlation id (Request/Response)
    std::string method;   // application verb (Request/Event/Task)
    std::string payload;  // opaque application bytes
    std::string from;     // sender node id (set on receive)
    bool ok = false;          // Response: did the remote handler succeed?
    std::string error;        // Response: failure reason when !ok
};

// The result of an RPC: `ok` with a `payload`, or a failure `reason`. A failure
// can be local (peer not connected, request timed out) or remote (the peer has
// no handler, or its handler returned an error).
struct SignalResponse {
    bool ok = false;
    std::string payload;
    std::string error;
};

// The application's handler for incoming signals, invoked by the cluster on its
// **network thread** — so implementations must be cheap and thread-safe; heavy
// work should be handed off to another executor (a later slice wires the
// work-stealing scheduler in behind this). Each method has a safe default, so a
// handler overrides only the kinds it cares about.
class SignalHandler {
public:
    virtual ~SignalHandler() = default;

    // Handle an RPC request and return its response. The default refuses with a
    // clear reason (a node that registers no handler still answers requests).
    virtual SignalResponse on_request(const Signal& request) {
        (void)request;
        return SignalResponse{.ok = false, .payload = {}, .error = "no handler"};
    }

    // Handle a fire-and-forget event. Default: ignore.
    virtual void on_event(const Signal& event) { (void)event; }

    // Handle a fire-and-forget task envelope. Default: ignore.
    virtual void on_task(const Signal& task) { (void)task; }
};

// Serialize a signal to its wire envelope — a single-line, compact JSON object
// with the opaque payload base64-encoded inside it (binary-safe). This is the
// `<envelope>` carried by the transport's `ZETTA/1 SIGNAL <envelope>` frame.
std::string encode_signal(const Signal& signal);

// Parse + validate a wire envelope back into a Signal. The **boundary**: the
// envelope is validated against the signal-envelope JSON Schema (draft-07) and
// the kind-specific required-field rules before anything is returned, so a
// malformed or hostile envelope fails fast with a path-qualified reason
// (written to `error` when non-null) instead of reaching a handler. Returns
// nullopt on any failure. Non-throwing, mirroring `ir::from_json`.
std::optional<Signal> decode_signal(std::string_view envelope,
                                    std::string* error = nullptr);

}  // namespace zetta
