#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/distributed_scheduler.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/observability/trace.hpp"

// The **job coordinator** (ARC-11, #372) — the single entry the native
// distributed accelerated-compute algorithms (DDF / GBDT / SHAP) submit work
// through. It joins the three pieces of the compute pass:
//
//   - the [work-stealing scheduler](../work-stealing-scheduler.md) (#366) —
//     splits a job's work into tasks and runs them across cores (intra-node) and
//     nodes (cross-node steal);
//   - [collectives](../collectives.md) (#365) — combine per-node partials at a
//     round boundary (allreduce / broadcast / barrier / gather);
//   - the [`ComputeJob`](../compute-jobs.md) record (ARC-11 / ENT-31) — the
//     durable, replicated lifecycle (`Queued → Running → Succeeded | Failed`).
//
// The model is **SPMD** (single program, multiple data), the way distributed
// GBDT / SHAP actually run: every participating node runs the same job body
// against its own data shard; intra-node work fans across the local pool, and the
// cross-node combine is a collective. The coordinator also exposes the cross-node
// `map` (dynamic task stealing) for embarrassingly-parallel work that isn't
// pre-sharded (DDF / batch scoring). It is **the consumer** the
// work-stealing-scheduler docs forward-reference — it submits work *through* the
// scheduler, it is not part of it.
//
// **Composition.** Both `Collectives` and `DistributedScheduler` want the
// cluster's single signal-handler slot; they are designed to chain. The
// coordinator owns both and wires `scheduler → fallback → collectives →
// delegate`, so an incoming signal is routed `steal`/`steal_result` → the
// scheduler, `collective.*` → the collectives, and anything else → the optional
// application fallback. Member declaration order makes construction register the
// chain and destruction unwind it cleanly. This is why the coordinator owns the
// scheduler + collectives rather than taking references — it is the one object
// that holds the handler slot.
//
// **No new wire format.** The coordinator introduces no JSON of its own: `map`
// rides the schema-validated steal messages, the collectives ride the
// schema-validated `CollectiveMessage`, and the job record is schema-validated at
// its own boundary — so malformed data still fails fast at each existing seam.
//
// See docs/compute/job-coordinator.md.
namespace zetta {
namespace cluster { class Cluster; }
}

namespace zetta::compute {

// The work surface an algorithm uses inside a coordinated job. It exposes both
// task-scheduling primitives (local + cross-node) and the round-boundary
// collectives, plus the cluster topology — nothing else, so a job body cannot
// reach past the coordinator's seams. Handed to the algorithm by
// JobCoordinator::run; never constructed directly.
class JobRound {
public:
    // --- Task scheduling ----------------------------------------------------

    // **Cross-node** data-parallel scheduling: run `method` over `inputs` across
    // this node's local pool *and* idle peers (dynamic work stealing), returning
    // the results in input order. For work that is not pre-sharded per node.
    // Requires the coordinator to have been start()ed (the steal loop) and the
    // shared executor registered on every node. Recorded as a child span of the
    // job's trace; the trace rides the steal offers, so a helper's execution
    // spans join it too (#640).
    std::vector<std::string> map(const std::string& method,
                                 const std::vector<std::string>& inputs) {
        zetta::observability::ScopedSpan span(recorder_, trace_,
                                              "map:" + method);
        span.set_attribute("job_id", job_id_);
        span.set_attribute("inputs", inputs.size());
        auto results = scheduler_.map(
            method, inputs,
            recorder_ ? zetta::observability::format_traceparent(trace_)
                      : std::string{});
        span.set_attribute("result_shape",
                           nlohmann::json{{"results", results.size()}});
        return results;
    }

    // **Intra-node** data-parallel scheduling: run `function` over `inputs`
    // across this node's worker pool, results in input order, blocking until all
    // complete. The natural way to compute a node's local partial (the SPMD
    // path). `function` must not throw across the seam.
    template <class Input, class Function>
    auto parallel_map(const std::vector<Input>& inputs, Function function)
        -> std::vector<
            std::decay_t<std::invoke_result_t<Function&, const Input&>>> {
        return scheduler_.local().parallel_map(inputs, std::move(function));
    }

    // --- Round-boundary collectives (combine per-node partials) --------------
    // Each round is recorded as a child span of the job's trace on THIS node
    // (every SPMD participant runs the same body, so every node contributes
    // its own round spans to the ONE job trace — #640). A round that returns
    // nothing closes its span as failed with the timeout reason.

    std::optional<std::vector<double>> allreduce(
        const std::string& tag, const std::vector<double>& local,
        zetta::cluster::CollectiveOp op = zetta::cluster::CollectiveOp::Sum,
        std::chrono::milliseconds timeout = zetta::cluster::Collectives::kDefaultTimeout) {
        zetta::observability::ScopedSpan span(recorder_, trace_,
                                              "collective:allreduce");
        span.set_attribute("tag", tag);
        span.set_attribute("values", local.size());
        auto result = collectives_.allreduce(tag, local, op, timeout);
        span.finish(result.has_value(),
                    result ? std::string{} : "allreduce '" + tag +
                                                 "' timed out or failed");
        return result;
    }

    std::optional<std::vector<double>> broadcast(
        const std::string& tag, const std::vector<double>& value,
        std::chrono::milliseconds timeout = zetta::cluster::Collectives::kDefaultTimeout) {
        zetta::observability::ScopedSpan span(recorder_, trace_,
                                              "collective:broadcast");
        span.set_attribute("tag", tag);
        auto result = collectives_.broadcast(tag, value, timeout);
        span.finish(result.has_value(),
                    result ? std::string{} : "broadcast '" + tag +
                                                 "' timed out or failed");
        return result;
    }

    bool barrier(const std::string& tag,
                 std::chrono::milliseconds timeout = zetta::cluster::Collectives::kDefaultTimeout) {
        zetta::observability::ScopedSpan span(recorder_, trace_,
                                              "collective:barrier");
        span.set_attribute("tag", tag);
        const bool reached = collectives_.barrier(tag, timeout);
        span.finish(reached, reached ? std::string{}
                                     : "barrier '" + tag +
                                           "' timed out or failed");
        return reached;
    }

    std::optional<std::vector<std::vector<double>>> gather(
        const std::string& tag, const std::vector<double>& local,
        std::chrono::milliseconds timeout = zetta::cluster::Collectives::kDefaultTimeout) {
        zetta::observability::ScopedSpan span(recorder_, trace_,
                                              "collective:gather");
        span.set_attribute("tag", tag);
        span.set_attribute("values", local.size());
        auto result = collectives_.gather(tag, local, timeout);
        span.finish(result.has_value(),
                    result ? std::string{} : "gather '" + tag +
                                                 "' timed out or failed");
        return result;
    }

    // --- Topology -----------------------------------------------------------

    // This node plus its connected peers, sorted by node id (so the root —
    // front() — is the same on every node). The collective participants.
    std::vector<std::string> participants() const {
        return collectives_.participants();
    }

    // This node's cluster id.
    const std::string& node_id() const { return node_id_; }

    // True when this node is the deterministic collective root (lowest id) — the
    // node that holds a gather's result and seeds a broadcast.
    bool is_root() const {
        const std::vector<std::string> all = participants();
        return !all.empty() && all.front() == node_id_;
    }

    // The id of the ComputeJob being run (for the algorithm's own logging/tags).
    const std::string& job_id() const { return job_id_; }

    // The job's trace on this node (#640) — the parent of the round/map spans
    // above; an algorithm may open its own child spans against it.
    const zetta::observability::TraceContext& trace() const { return trace_; }
    zetta::observability::SpanRecorder* recorder() const { return recorder_; }

private:
    friend class JobCoordinator;
    JobRound(DistributedScheduler& scheduler, zetta::cluster::Collectives& collectives,
             std::string node_id, std::string job_id,
             zetta::observability::SpanRecorder* recorder,
             zetta::observability::TraceContext trace)
        : scheduler_(scheduler),
          collectives_(collectives),
          node_id_(std::move(node_id)),
          job_id_(std::move(job_id)),
          recorder_(recorder),
          trace_(std::move(trace)) {}

    DistributedScheduler& scheduler_;
    zetta::cluster::Collectives& collectives_;
    std::string node_id_;
    std::string job_id_;
    zetta::observability::SpanRecorder* recorder_ = nullptr;  // null = no-op spans
    zetta::observability::TraceContext trace_;
};

// The per-node job coordinator. Construct one over a started (or startable)
// Cluster; it owns the collectives + the distributed scheduler bound to that
// cluster (wiring the shared signal-handler chain). The same executor must be
// registered on every node (for `map`); for the SPMD path no executor is needed.
class JobCoordinator {
public:
    // How a (method, payload) `map` task is executed — registered identically on
    // every node so a stolen task computes the same wherever it lands.
    using TaskExecutor = DistributedScheduler::TaskExecutor;

    // What a job body reports back to the coordinator: success (→ Succeeded) or
    // failure (→ Failed), plus a human-readable `detail` (a result summary, or
    // the failure reason) recorded on the job.
    struct Outcome {
        bool ok = true;
        std::string detail;
    };

    // The job body. Given the round's work surface, do the compute and report an
    // Outcome. Runs on the application thread that called run(). MUST NOT throw
    // across the seam (zetta's no-exceptions convention) — a thrown exception
    // would leave the job stuck in Running.
    using Algorithm = std::function<Outcome(JobRound&)>;

    // `worker_count` local worker threads (0 = a pure source for `map`: this node
    // runs no `map` tasks itself, only hands them to peers; not usable for the
    // SPMD parallel_map path). `fallback`, when given, receives every signal that
    // is neither a steal nor a collective, so the coordinator can coexist with
    // another signal consumer. `cluster` must outlive the coordinator.
    JobCoordinator(zetta::cluster::Cluster& cluster, unsigned worker_count,
                   zetta::cluster::SignalHandler* fallback = nullptr);
    JobCoordinator(zetta::cluster::Cluster& cluster, unsigned worker_count,
                   DistributedScheduler::Config config,
                   zetta::cluster::SignalHandler* fallback = nullptr);
    ~JobCoordinator();

    JobCoordinator(const JobCoordinator&) = delete;
    JobCoordinator& operator=(const JobCoordinator&) = delete;

    // Register the `map` task executor. Call before start() (or before any map()
    // work flows). Not needed for SPMD/collective-only jobs.
    void set_executor(TaskExecutor executor) {
        scheduler_.set_executor(std::move(executor));
    }

    // Wire the node's span ring (#640): run() then records the job root span +
    // the JobRound's round/map child spans, and stolen tasks executed FOR a
    // peer record steal spans joining the owner's trace. Null = spans no-op.
    void set_span_recorder(zetta::observability::SpanRecorder* recorder) {
        recorder_ = recorder;
        scheduler_.set_span_recorder(recorder);
    }

    // Register as the cluster's signal handler and start the cross-node steal
    // loop. Required before running jobs that use map(); harmless (recommended
    // for uniformity) for SPMD jobs, whose collectives route through the chain.
    void start() { scheduler_.start(); }
    // Stop the steal loop and restore the underlying collectives handler. Do not
    // call while a job is running on this node.
    void stop() { scheduler_.stop(); }

    // Drive `job` through its lifecycle: set it Running, run `algorithm` against a
    // JobRound, then set the terminal status from the Outcome (Succeeded with the
    // detail, or Failed with the reason). Returns the terminal status. The
    // transitions capture into the CRDT, persist, and emit the `job_finished`
    // audit event through whatever ReplicationManager / AuditSink the job was
    // created with (e.g. via Node::create_job).
    JobStatus run(ComputeJob& job, const Algorithm& algorithm);

    // The owned pieces, for direct use / observability (e.g. scheduler stats).
    DistributedScheduler& scheduler() { return scheduler_; }
    const DistributedScheduler& scheduler() const { return scheduler_; }
    zetta::cluster::Collectives& collectives() { return collectives_; }

private:
    zetta::cluster::Cluster& cluster_;
    // Declared FIRST so it registers as the cluster handler before the scheduler,
    // and is destroyed LAST so it restores the application fallback after the
    // scheduler has restored it as the handler. Do not reorder.
    zetta::cluster::Collectives collectives_;
    DistributedScheduler scheduler_;
    zetta::observability::SpanRecorder* recorder_ = nullptr;  // null = no-op spans
};

}  // namespace zetta::compute
