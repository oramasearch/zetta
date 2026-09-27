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
//  - PerKey: a thread per key (the default while keys are few);
//  - Pool:   a shared pool of threads; each key sticks to one worker (order is
//            kept), workers serve their keys round robin with a per-turn cap;
//  - Inline: the submitting thread runs the work itself.
// Auto moves from PerKey to Pool once the number of keys exceeds a threshold,
// and back only when it falls to half of it (hysteresis). A key can be pinned
// to Inline or PerKey by configuration. Every switch is logged with its reason.
namespace zetta::pubsub {

enum class DispatchMode { Auto, PerKey, Pool, Inline };

std::string_view to_string(DispatchMode mode);

struct DispatcherOptions {
    DispatchMode mode = DispatchMode::Auto;
    std::size_t threshold = 32;    // keys before Auto moves to the pool
    std::size_t pool_threads = 4;
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

    // Pin a key to a mode (PerKey or Inline), or clear the pin with nullopt.
    void pin(const std::string& key, std::optional<DispatchMode> mode);

    // Forget a key (its topic was dropped). Pending jobs still run.
    void remove_key(const std::string& key);

    // The mode currently in effect (PerKey or Pool; Inline when forced).
    DispatchMode effective_mode() const;
    std::size_t key_count() const;

    // Block until every job submitted so far has run.
    void drain();

    // Stop accepting work, run what is queued, join every thread.
    void stop();

private:
    struct Lane {
        std::deque<Job> jobs;
        bool running = false;  // a worker is currently executing this lane
    };
    struct Worker {
        std::thread thread;
        std::condition_variable wake;
        std::deque<std::string> ready;  // keys with queued jobs, round robin
        bool stop = false;
    };

    void start_per_key_worker(const std::string& key);  // mutex held
    void ensure_pool();                                   // mutex held
    std::size_t pool_worker_for(const std::string& key) const;
    void run_per_key(const std::string& key);
    void run_pool_worker(std::size_t index);
    void maybe_switch();                                 // mutex held
    void switch_to(DispatchMode mode, std::string reason);  // mutex held; releases/reacquires
    void note_done(std::unique_lock<std::mutex>& lock);

    DispatcherOptions options_;
    LogSink log_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::map<std::string, Lane> lanes_;
    std::map<std::string, DispatchMode> pins_;
    std::map<std::string, std::unique_ptr<Worker>> per_key_workers_;
    std::vector<std::unique_ptr<Worker>> pool_;
    std::vector<std::unique_ptr<Worker>> retired_;  // per-key workers handed off, joined on stop()
    DispatchMode current_ = DispatchMode::PerKey;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
};

}  // namespace zetta::pubsub
