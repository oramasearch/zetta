#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "zetta/pubsub/log.hpp"

// What moves pub/sub work along (#1026; decision D24 in
// docs/plans/pubsub/decisions.md). See docs/pubsub/dispatch.md.
//
// Work is submitted under a key (a topic or target address); work for one key
// runs in submission order. Modes:
//  - Pool:   the default shared pool; at most `topics_per_thread` active keys
//            per worker, bounded by configured minimum and maximum sizes;
//  - PerKey: a thread per key, only when explicitly selected;
//  - Inline: the submitting thread runs the work itself.
// Auto retains the legacy explicit choice of PerKey below the pool threshold.
// Pool growth and shrink are logged. Shrink uses a key-count margin so a pool
// does not flap at a sizing boundary. A key moves workers only between jobs.
namespace zetta::pubsub {

enum class DispatchMode { Auto, PerKey, Pool, Inline };

std::string_view to_string(DispatchMode mode);
std::optional<DispatchMode> parse_dispatch_mode(std::string_view mode);

struct DispatcherOptions {
    DispatchMode mode = DispatchMode::Pool;
    std::size_t topics_per_thread = 250;
    std::size_t minimum_threads = 1;
    std::size_t maximum_threads = 4;
    std::size_t shrink_hysteresis_keys = 10;
    std::size_t per_turn_cap = 64;  // jobs one key may run per pool turn
};

class Dispatcher {
public:
    using Job = std::function<void()>;

    Dispatcher(DispatcherOptions options, LogSink log);
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Run `job` for `key`. Returns false after stop().
    bool submit(const std::string& key, Job job);

    // Count an active key even before it has work. Topic creation uses this so
    // pool sizing follows topics, not only topics that have already published.
    void register_key(const std::string& key);

    // Pin a key to a mode (PerKey or Inline), or clear the pin with nullopt.
    void pin(const std::string& key, std::optional<DispatchMode> mode);

    // Forget a key (its topic was dropped). Pending jobs still run.
    void remove_key(const std::string& key);

    // The mode currently in effect (PerKey or Pool; Inline when forced).
    DispatchMode effective_mode() const;
    std::size_t key_count() const;
    std::size_t pool_thread_count() const;
    std::size_t topics_per_thread() const {
        return options_.topics_per_thread;
    }
    std::size_t minimum_threads() const {
        return options_.minimum_threads;
    }
    std::size_t maximum_threads() const {
        return options_.maximum_threads;
    }

    // Blocking waits are forbidden inside dispatch jobs. Blocking pub/sub
    // paths test this before waiting and assert in debug builds (#1157).
    static bool blocking_allowed();
    static void assert_blocking_allowed();

    // Called repeatedly while drain() or stop() waits, on the waiting thread.
    // The node passes its owner-queue pump so a job that marshals work onto
    // the owner thread cannot deadlock a shutdown running on that thread.
    void set_idle_pump(std::function<void()> pump);

    // Block until every job submitted so far has run.
    void drain();

    // Stop accepting work, run what is queued, join every thread.
    void stop();

private:
    struct Worker;
    struct Lane {
        std::deque<Job> jobs;
        bool running = false;  // a worker is currently executing this lane
        bool registered = true;
        bool queued = false;
        Worker* pool_worker = nullptr;
    };
    struct Worker {
        std::thread thread;
        std::condition_variable wake;
        std::deque<std::string> ready;  // keys with queued jobs, round robin
        bool stop = false;
    };

    void start_per_key_worker(const std::string& key);  // mutex held
    DispatchMode mode_for_key(const std::string& key) const;  // mutex held
    std::size_t registered_key_count() const;                  // mutex held
    std::size_t pooled_key_count() const;                      // mutex held
    std::size_t desired_pool_threads() const;                  // mutex held
    Worker* desired_worker(const std::string& key) const;      // mutex held
    bool active_worker(const Worker* worker) const;            // mutex held
    void enqueue_pool_key(const std::string& key, Worker& worker);  // mutex held
    void resize_pool(std::size_t threads);                         // mutex held
    void rebalance();                                              // mutex held
    void maybe_resize_pool();                                      // mutex held
    void run_per_key(const std::string& key);
    void run_pool_worker(Worker* worker);
    void maybe_switch();                                 // mutex held
    void switch_to(DispatchMode mode, std::string reason);  // mutex held
    void note_done(std::unique_lock<std::mutex>& lock);
    void wait_idle(std::unique_lock<std::mutex>& lock);  // pumps while waiting

    DispatcherOptions options_;
    LogSink log_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::map<std::string, Lane> lanes_;
    std::map<std::string, DispatchMode> pins_;
    std::map<std::string, std::unique_ptr<Worker>> per_key_workers_;
    std::vector<std::unique_ptr<Worker>> pool_;
    std::vector<std::unique_ptr<Worker>> retired_;  // exited workers, joined on stop()
    DispatchMode current_ = DispatchMode::Pool;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
    std::function<void()> idle_pump_;
};

}  // namespace zetta::pubsub
