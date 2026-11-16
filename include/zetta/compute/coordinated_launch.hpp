// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/signal.hpp"
#include "zetta/observability/trace.hpp"

// The **coordinated cross-process SPMD launch** (#601) — the co-launch half of
// the distributed compute story. The collectives already handle the *combine*
// (allreduce / gather / broadcast at every round boundary), but an SPMD job only
// converges when **every participating node runs the same job body
// concurrently**: a single node submitting over a multi-node cluster whose peers
// are not co-running fails its collective rounds. Until now the co-launch
// existed only in-process (the testkit spawning one thread per loopback node);
// this seam makes it work **across processes**, over the live membership.
//
// The mechanism is a `run.launch` **Task** signal (signal.hpp — the
// fire-and-forget work envelope that was purpose-built as "the substrate for
// job dispatch" and had no consumer until now):
//
//   1. The initiator snapshots the roster (the collective participants — this
//      node + every connected peer, node-id ascending, identical everywhere),
//      builds a CoordinatedLaunchRequest naming the roster + a kind-specific
//      payload, and `fan_out()`s it: one `run.launch` task to every roster
//      peer. It then runs its own shard of the work on the calling thread.
//   2. Each peer's listener receives the task on the cluster's **network
//      thread**, where it only schema-validates the envelope and enqueues it
//      (the steal-handler discipline — a blocking SPMD body on the network
//      thread would deadlock the very collectives it participates in).
//   3. The node-owning loop drains the queue from `Node::tick()` and executes
//      the registered runner for the request's kind **on the owner thread** —
//      the same "synchronous on the thread that owns the Node" pattern every
//      interactive submitter already uses — so the co-run's collectives block
//      an application thread, never the signal handler.
//
// **Ephemeral by design.** A launch is a *message*, not durable state (the
// signal-plane carve-out from the "everything is replicated" rule): delivery is
// best-effort, at-most-once. The durable artifacts of the launched work — the
// ComputeJobs, the Run, the Model — replicate through their own CRDT records as
// always; a peer that misses the launch simply doesn't co-run, and the
// initiator's collective rounds fail with the bounded timeout (observable, not
// silent).
//
// **New JSON boundary.** The launch envelope is a new wire format, so it gets
// the full treatment: an embedded draft-07 schema
// (`coordinated_launch_request_schema()`, generated + drift-checked as
// `schemas/coordinated_launch_request.schema.json`), pboettch-validated in
// `decode_coordinated_launch` before anything reaches the queue. The
// kind-specific `payload` is validated by its kind's runner at ITS boundary
// (the churn payload has its own schema — churn_solution.cpp).
//
// New launch kind = a new `set_runner` registration (the churn solution is the
// first: usecases/churn/churn_solution.hpp `install_churn_launch_runner`) —
// never a bespoke fan-out path. The near-data partitioned jobs (#531) are the
// intended second consumer.
//
// See docs/compute/coordinated-launch.md.
namespace zetta::cluster { class Cluster; }

namespace zetta::compute {

// One coordinated launch: "every node in `participants` co-runs the `kind` work
// described by `payload`, now". The roster is the agreement — every participant
// derives its shard rank from its position in it, so it must be identical in
// every copy of the request (the initiator builds it once from the collective
// participants and sends the same envelope to everyone).
struct CoordinatedLaunchRequest {
    std::string id;         // unique launch id (initiator-minted UUID)
    std::string kind;       // which runner executes this (e.g. "churn_solution")
    std::string initiator;  // the initiating node's cluster id
    std::vector<std::string> participants;  // the roster, node-id ascending
    nlohmann::json payload = nlohmann::json::object();  // kind-specific body
};

// The launch-envelope JSON Schema (draft-07) — the **code-owned source of
// truth**; `schemas/coordinated_launch_request.schema.json` is generated from it
// and drift-checked by `tests/test_schemas.cpp`.
const nlohmann::json& coordinated_launch_request_schema();

// Serialize a request to the `run.launch` task payload (compact JSON).
std::string encode_coordinated_launch(const CoordinatedLaunchRequest& request);

// Parse + validate a `run.launch` payload. The **boundary**: validated against
// the schema above before any field is read, so a malformed / hostile payload
// fails fast with a path-qualified reason (written to `error` when non-null)
// instead of reaching a runner. Non-throwing; nullopt on any failure.
std::optional<CoordinatedLaunchRequest> decode_coordinated_launch(
    std::string_view payload, std::string* error = nullptr);

// This node's shard rank in the request's roster: the index of `node_id` in
// `participants`, or nullopt when the node is not named (stale membership — the
// runner must decline rather than double-count a shard).
std::optional<std::size_t> participant_rank(
    const CoordinatedLaunchRequest& request, std::string_view node_id);

// The `run.launch` listener + fan-out. One per clustered node, wired by
// `Node::attach_cluster` as the JobCoordinator's signal-handler **fallback**
// (chained over the telemetry exchange, which it delegates everything else to):
//
//   signal → scheduler (steal) → collectives (collective.*) → THIS (run.launch)
//                                                   └─ else → delegate
//
// **Threading.** `on_task` runs on the cluster's network thread and only
// validates + enqueues (mutex-guarded queue — the sanctioned contained
// cross-thread handoff). `drain()` runs on the node-owning loop (Node::tick)
// and executes the runners there. `set_runner` is owner-thread-only: register
// every runner before the cluster can deliver launches (attach time).
class CoordinatedLaunchListener : public zetta::cluster::SignalHandler {
public:
    // The signal-plane task verb this listener fans out + handles.
    static constexpr std::string_view kMethod = "run.launch";

    // What a runner reports back: ok (+ a result summary) or a failure reason —
    // recorded on the drain span so a peer that declines / fails a co-run is
    // observable (RULE: every failure is observable), never silent.
    struct LaunchOutcome {
        bool ok = true;
        std::string detail;
    };

    // Executes one launch of a registered kind, on the owner thread. The runner
    // owns the work's own observability (jobs / runs / spans) and must not
    // throw across the seam.
    using Runner =
        std::function<LaunchOutcome(const CoordinatedLaunchRequest& request)>;

    // `delegate` (optional, non-owning, must outlive this listener) receives
    // every signal that is not a `run.launch` task — the telemetry exchange in
    // the node wiring. `cluster` must outlive this listener.
    explicit CoordinatedLaunchListener(
        zetta::cluster::Cluster& cluster,
        zetta::cluster::SignalHandler* delegate = nullptr);

    // Register the runner for a launch kind (replacing any previous one).
    // Owner-thread-only; register before launches can arrive.
    void set_runner(const std::string& kind, Runner runner);

    // This node's id **in the roster's domain** — the cluster's node id (the
    // collective participants and the launch roster are cluster ids; a Node's
    // own id may differ, e.g. in the testkit's loopback harness). Initiators
    // and runners must use this for participant_rank / initiator stamping.
    const std::string& node_id() const;

    // Wire the node's span ring: each drained launch (and each rejected
    // envelope) records a span. Null = spans no-op.
    void set_span_recorder(zetta::observability::SpanRecorder* recorder) {
        recorder_ = recorder;
    }

    // Initiator side: send `request` as a `run.launch` task to every roster
    // participant except this node (fire-and-forget, best-effort). Returns how
    // many peers it was queued to. The caller then runs its own shard.
    std::size_t fan_out(const CoordinatedLaunchRequest& request);

    // SignalHandler (network thread): validate + enqueue a `run.launch` task;
    // forward everything else to the delegate.
    void on_task(const zetta::cluster::Signal& task) override;
    void on_event(const zetta::cluster::Signal& event) override;
    zetta::cluster::SignalResponse on_request(
        const zetta::cluster::Signal& request) override;

    // Owner thread (Node::tick): execute every queued launch through its kind's
    // runner, in arrival order, recording each as a `run.launch:<kind>` span
    // (ok/failed from the runner's LaunchOutcome). A launch with no registered
    // runner is dropped as a failed span — observable, never silent. Returns
    // how many launches were executed (dropped ones do not count).
    std::size_t drain();

    // Queued launches not yet drained (thread-safe; for tests / introspection).
    std::size_t pending() const;

    // Envelopes rejected at the validation boundary since construction
    // (thread-safe; for tests / introspection).
    std::size_t rejected() const;

private:
    zetta::cluster::Cluster& cluster_;
    zetta::cluster::SignalHandler* delegate_ = nullptr;  // optional, non-owning
    zetta::observability::SpanRecorder* recorder_ = nullptr;  // null = no-op
    std::map<std::string, Runner> runners_;  // owner-thread-only (see above)

    // Contained cross-thread handoff: the network thread enqueues, the owner
    // loop drains — never exposed (the cluster-outbox pattern).
    mutable std::mutex queue_mutex_;
    std::deque<CoordinatedLaunchRequest> queue_;
    std::size_t rejected_ = 0;  // guarded by queue_mutex_
};

}  // namespace zetta::compute
