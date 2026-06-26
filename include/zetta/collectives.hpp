#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/signal.hpp"

// Collective communication over the **signal plane** (ADR-28 / ARC-10) — the
// piece the native distributed accelerated-compute pass for UC-1 churn (#264)
// uses to combine per-node compute partials (gradient histograms, partial
// aggregates) across the cluster. Four collectives, modeled on MPI:
//
//   - allreduce — every node contributes a vector; every node receives their
//     elementwise reduction (sum / product / min / max).
//   - broadcast — the root node's vector is delivered to every node.
//   - barrier   — every node blocks until all have arrived, then all proceed.
//   - gather    — every node contributes a vector; the root receives all of
//     them, ordered (classic MPI gather: the result lands only at the root).
//
// **Deterministic, independent of node order.** The participants are this node
// plus its connected peers; the *root* is the lexicographically-smallest node
// id (leaderless — no election, every node computes the same root). A reduction
// runs at the root in node-id order and the single result is pushed back to
// everyone, so every node ends with bit-identical bytes even though
// floating-point addition is not associative. This is why the root reduces once
// and broadcasts the result rather than each node reducing locally.
//
// **Threading.** The collective is driven from an application thread (the one
// that calls allreduce()/…), which blocks until the round completes or times
// out. The signal handler (Collectives::on_request, run on the cluster network
// thread) only records arrivals/results and wakes the waiter — it never blocks,
// honoring the signal plane's "handlers must be cheap" contract. RPC
// (Cluster::request) carries every message, so an unreachable peer fails the
// round fast instead of hanging.
//
// Implemented as a star (depth-1 tree): the root gathers from all participants
// and broadcasts the result. A k-ary tree / ring reduction (to cut the root's
// fan-in at large scale) is a later optimization behind this same API. See
// docs/collectives.md.
namespace zetta {

class Cluster;

// The associative reduction applied elementwise by allreduce(). Mirrors
// zetta::compute::ReduceOp (the compute device's reduce()) — kept as its own
// type so the cluster-coordination layer does not depend on the device memory
// model.
enum class CollectiveOp { Sum, Product, Minimum, Maximum };

// Lowercase canonical name ("sum"/"product"/"minimum"/"maximum").
std::string_view to_string(CollectiveOp op);

// Parse a name back to a CollectiveOp (nullopt on anything unknown).
std::optional<CollectiveOp> collective_op_from_string(std::string_view name);

// The payload of a collective-plane RPC, carried as a signal's opaque payload.
// `tag` names the round, `from` is the sender's node id (on a contribution), and
// `values` is the per-node vector (empty for a barrier). This is the wire seam,
// exposed (like encode_signal/decode_signal) so the schema boundary is directly
// testable; it is otherwise an internal protocol detail.
struct CollectiveMessage {
    std::string tag;
    std::string from;
    std::vector<double> values;
};

// Serialize a collective message to its compact JSON form.
std::string encode_collective_message(const CollectiveMessage& message);

// Parse + validate a collective message at the **boundary**: against the
// draft-07 schema (schemas/collective_message.schema.json, mirrored embedded in
// collectives.cpp via pboettch/json-schema-validator, the same pattern as the
// signal envelope) before anything is returned. Non-throwing — returns nullopt
// with a path-qualified reason in `error` (when non-null) on any failure.
std::optional<CollectiveMessage> decode_collective_message(
    std::string_view payload, std::string* error = nullptr);

// The collectives runtime for one node. Construct it over a started (or
// startable) Cluster; it registers itself as the cluster's SignalHandler so it
// can receive contribute/result RPCs, and restores the previous handler (the
// optional `delegate`) on destruction. A node that also needs to handle its own
// signals passes that handler as `delegate` — Collectives forwards every
// non-collective request/event/task to it.
class Collectives : public SignalHandler {
public:
    // A generous default: a collective round waits this long for every peer to
    // arrive before failing. Compute rounds are short, but a peer may be busy.
    static constexpr std::chrono::milliseconds kDefaultTimeout{10000};

    explicit Collectives(Cluster& cluster, SignalHandler* delegate = nullptr);
    ~Collectives() override;

    Collectives(const Collectives&) = delete;
    Collectives& operator=(const Collectives&) = delete;

    // Reduce `local` elementwise across all participants and return the result
    // (the same on every node). Every participant must call allreduce() with the
    // same `tag` and an equal-length vector. nullopt on a length mismatch, an
    // unreachable peer, or a timeout.
    std::optional<std::vector<double>> allreduce(
        const std::string& tag, const std::vector<double>& local,
        CollectiveOp op = CollectiveOp::Sum,
        std::chrono::milliseconds timeout = kDefaultTimeout);

    // Deliver the root's vector to every participant; returns it on every node.
    // The root (lowest node id) supplies `value`; non-root callers pass any
    // vector (ignored — the root's is the one distributed). nullopt on an
    // unreachable peer or a timeout.
    std::optional<std::vector<double>> broadcast(
        const std::string& tag, const std::vector<double>& value,
        std::chrono::milliseconds timeout = kDefaultTimeout);

    // Block until every participant has reached this barrier `tag`, then release
    // them all. Returns false on an unreachable peer or a timeout.
    bool barrier(const std::string& tag,
                 std::chrono::milliseconds timeout = kDefaultTimeout);

    // Collect every participant's `local` vector at the root, ordered by node
    // id. The **root** returns the full ordered list; **non-root** callers
    // return an empty list (classic MPI gather — the result lands only at the
    // root). nullopt on an unreachable peer or a timeout.
    std::optional<std::vector<std::vector<double>>> gather(
        const std::string& tag, const std::vector<double>& local,
        std::chrono::milliseconds timeout = kDefaultTimeout);

    // The participants of a collective: this node and every connected peer,
    // sorted by node id (so the root — front() — is the same on every node).
    std::vector<std::string> participants() const;

    // --- SignalHandler (runs on the cluster network thread) -----------------
    SignalResponse on_request(const Signal& request) override;
    void on_event(const Signal& event) override;
    void on_task(const Signal& task) override;

private:
    // Per-round state, keyed by tag. The root accumulates `contributions`
    // (node id -> vector); a non-root records the pushed `result`.
    struct Round {
        std::map<std::string, std::vector<double>> contributions;
        std::vector<double> result;
        bool result_ready = false;
    };

    // Root: seed our own `own` vector, then wait until every participant has
    // contributed. Returns the contributions (node id -> vector) or nullopt on
    // timeout.
    std::optional<std::map<std::string, std::vector<double>>> gather_at_root(
        const std::string& tag, const std::vector<std::string>& participants,
        const std::vector<double>& own, std::chrono::milliseconds timeout);

    // Root: push `result` to every non-self participant by RPC. False if any
    // peer is unreachable.
    bool push_result(const std::string& tag,
                     const std::vector<std::string>& participants,
                     const std::string& self, const std::vector<double>& result,
                     std::chrono::milliseconds timeout);

    // Non-root: contribute `values` to the root, then (when expect_result) wait
    // for the pushed result. nullopt on a failed contribution or a timeout.
    std::optional<std::vector<double>> participate(
        const std::string& tag, const std::string& root,
        const std::vector<double>& values, bool expect_result,
        std::chrono::milliseconds timeout);

    // Drop a finished round's state.
    void clear_round(const std::string& tag);

    Cluster& cluster_;
    SignalHandler* delegate_ = nullptr;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Round> rounds_;
};

}  // namespace zetta
