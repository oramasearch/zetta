#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "zetta/compute/work_stealing_scheduler.hpp"
#include "zetta/signal.hpp"

// The **cross-node** half of the work-stealing scheduler (ARC-10, #366): idle /
// faster *nodes* steal pending tasks from busy peers over the
// [signal plane](../signal-plane.md), the same way the local
// `WorkStealingScheduler` lets idle worker threads steal from busy ones. A node
// that finishes its own work (or has none) pulls pending tasks from a peer,
// runs them across its local pool, and ships the results back — so a faster
// node (e.g. one with a GPU) naturally processes more, with no fixed
// per-node capability model. See docs/work-stealing-scheduler.md.
namespace zetta {
class Cluster;
}

namespace zetta::compute {

// Pull-based, single-hop work stealing across the cluster, layered on a local
// WorkStealingScheduler and the cluster's signal plane.
//
// `map(method, inputs)` runs `method` over every input across this node's local
// pool **and** any idle peers, returning the results in order. The node that
// calls `map` **owns** the job: it holds the authoritative queue of
// not-yet-started tasks; its own workers drain it, and idle peers steal batches
// from it (RPC `steal` → an offer), run them, and return the results (RPC
// `steal_result`). Stealing is **single-hop** — a node only hands out tasks from
// jobs it owns, never tasks it has itself stolen — so there is no re-steal loop.
// A peer that takes tasks but never returns them (it died, or the link dropped)
// has its assignment **re-queued** after a lease, so `map` never hangs on a lost
// helper as long as *some* compute capacity remains.
//
// All the application supplies is one **executor** — `string(method, payload)` —
// registered identically on every node, so a stolen task runs the same wherever
// it lands. The DistributedScheduler is the cluster's `SignalHandler` (it answers
// `steal` / `steal_result`); a `fallback` handler receives every other signal so
// it can share the cluster's single handler slot with another consumer.
//
// Threading: the per-job drain runs on local worker threads; the steal loop runs
// on its own thread and blocks on `Cluster::request` (never the network thread);
// the `steal` / `steal_result` handlers run on the network thread and stay cheap
// (hand out / store descriptors — the actual compute happens on the *stealer*).
class DistributedScheduler : public SignalHandler {
public:
    // How a (method, payload) task is executed. The payload + the return are raw
    // application bytes (the wire base64s them); this same executor must be
    // registered on every node so a stolen task computes identically anywhere.
    using TaskExecutor =
        std::function<std::string(std::string_view method,
                                  std::string_view payload)>;

    // Tunables for the steal protocol.
    struct Config {
        // How long a stealer's assignment is held before the owner re-queues it
        // (a dropped/dead helper must not strand the tasks it took).
        std::chrono::milliseconds steal_lease{3000};
        // How long a steal / steal_result RPC waits for its peer.
        std::chrono::milliseconds steal_timeout{2000};
        // How often the steal loop polls for work to steal (and sweeps expired
        // assignments on jobs this node owns).
        std::chrono::milliseconds steal_interval{25};
    };

    // `worker_count` local worker threads (0 = a pure source: this node hands its
    // work to peers and runs none itself). `cluster` must outlive this scheduler.
    DistributedScheduler(Cluster& cluster, unsigned worker_count);
    DistributedScheduler(Cluster& cluster, unsigned worker_count, Config config);
    ~DistributedScheduler() override;

    DistributedScheduler(const DistributedScheduler&) = delete;
    DistributedScheduler& operator=(const DistributedScheduler&) = delete;

    // Set the task executor. Call before start() (or before any work flows).
    void set_executor(TaskExecutor executor);

    // Signals other than steal / steal_result are forwarded here, so the
    // scheduler can coexist with another signal consumer. Pass nullptr to clear.
    void set_fallback_handler(SignalHandler* fallback);

    // Register as the cluster's signal handler and start the steal loop.
    void start();
    // Stop the steal loop and restore the fallback handler. Do not call while a
    // map() is in flight on this node.
    void stop();

    // Run `method` over every input across the local pool and idle peers; returns
    // results in input order, blocking until all complete. Needs either local
    // workers or reachable helpers (with a registered executor) to make progress.
    std::vector<std::string> map(const std::string& method,
                                 const std::vector<std::string>& inputs);

    // A snapshot of this node's cross-node activity.
    struct Stats {
        std::size_t local_executed = 0;       // tasks run locally for own jobs
        std::size_t handed_out = 0;           // tasks given to peers (as owner)
        std::size_t stolen_executed = 0;      // tasks run after stealing
        std::size_t requeued = 0;             // expired assignments re-queued
        std::size_t steal_requests_sent = 0;  // steal RPCs sent
    };
    Stats stats() const;

    // The local pool, exposed for observability (e.g. per-worker steal counts).
    const WorkStealingScheduler& local() const { return local_; }

    // SignalHandler — steal / steal_result are handled here; the rest is
    // forwarded to the fallback (if any).
    SignalResponse on_request(const Signal& request) override;
    void on_event(const Signal& event) override;
    void on_task(const Signal& task) override;

private:
    // One owned map() in flight. The pending deque is the authoritative source of
    // not-yet-started work — local drainers and remote stealers both draw from
    // it; `assigned` tracks tasks out at a peer (with a lease for re-queue).
    struct Job {
        std::string method;
        std::vector<std::string> inputs;
        std::vector<std::string> results;
        std::vector<char> done;             // per-index completion (idempotency)
        std::deque<std::size_t> pending;
        struct Assignment {
            std::string peer;
            std::chrono::steady_clock::time_point deadline;
        };
        std::unordered_map<std::size_t, Assignment> assigned;
        std::size_t completed = 0;
        std::mutex mutex;
        std::condition_variable cv;
    };

    void drain_local(std::shared_ptr<Job> job);
    void complete_index(Job& job, std::size_t index, std::string result,
                        bool from_local);

    void steal_loop(std::stop_token stop_token);
    bool try_steal_once();
    void sweep_expired_assignments();

    SignalResponse handle_steal(const Signal& request);
    SignalResponse handle_steal_result(const Signal& request);

    Cluster& cluster_;
    const Config config_;
    WorkStealingScheduler local_;
    TaskExecutor executor_;
    std::atomic<SignalHandler*> fallback_{nullptr};

    std::mutex jobs_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::jthread steal_thread_;
    std::mutex steal_mutex_;
    std::condition_variable_any steal_cv_;
    std::atomic<unsigned> steal_peer_cursor_{0};

    std::atomic<std::size_t> local_executed_{0};
    std::atomic<std::size_t> handed_out_{0};
    std::atomic<std::size_t> stolen_executed_{0};
    std::atomic<std::size_t> requeued_{0};
    std::atomic<std::size_t> steal_requests_sent_{0};
};

}  // namespace zetta::compute
