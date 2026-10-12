#include "zetta/pubsub/dispatcher.hpp"

#include <algorithm>
#include <cassert>
#include <functional>

namespace zetta::pubsub {
namespace {

thread_local bool running_dispatch_job = false;

class DispatchJobScope {
public:
    DispatchJobScope() {
        assert(!running_dispatch_job);
        running_dispatch_job = true;
    }

    ~DispatchJobScope() {
        running_dispatch_job = false;
    }
};

}  // namespace

std::string_view to_string(DispatchMode mode) {
    switch (mode) {
        case DispatchMode::Auto: return "auto";
        case DispatchMode::PerKey: return "thread per topic";
        case DispatchMode::Pool: return "pool";
        case DispatchMode::Inline: return "inline";
    }
    return "auto";
}

std::optional<DispatchMode> parse_dispatch_mode(std::string_view mode) {
    if (mode == "auto") return DispatchMode::Auto;
    if (mode == "per_key" || mode == "per-key") return DispatchMode::PerKey;
    if (mode == "pool") return DispatchMode::Pool;
    if (mode == "inline") return DispatchMode::Inline;
    return std::nullopt;
}

Dispatcher::Dispatcher(DispatcherOptions options, LogSink log)
    : options_(options), log_(std::move(log)) {
    if (options_.topics_per_thread == 0) options_.topics_per_thread = 1;
    if (options_.minimum_threads == 0) options_.minimum_threads = 1;
    if (options_.maximum_threads < options_.minimum_threads) {
        options_.maximum_threads = options_.minimum_threads;
    }
    if (options_.per_turn_cap == 0) options_.per_turn_cap = 1;
    switch (options_.mode) {
        case DispatchMode::Pool: current_ = DispatchMode::Pool; break;
        case DispatchMode::Inline: current_ = DispatchMode::Inline; break;
        default: current_ = DispatchMode::PerKey; break;
    }
    maybe_resize_pool();
}

Dispatcher::~Dispatcher() { stop(); }

DispatchMode Dispatcher::effective_mode() const {
    std::lock_guard lock(mutex_);
    return current_;
}

std::size_t Dispatcher::key_count() const {
    std::lock_guard lock(mutex_);
    return registered_key_count();
}

std::size_t Dispatcher::pool_thread_count() const {
    std::lock_guard lock(mutex_);
    return pool_.size();
}

bool Dispatcher::blocking_allowed() {
    return !running_dispatch_job;
}

void Dispatcher::assert_blocking_allowed() {
    assert(blocking_allowed() && "a pub/sub dispatch job must never block its worker");
}

void Dispatcher::register_key(const std::string& key) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    auto& lane = lanes_[key];
    lane.registered = true;
    maybe_switch();
    maybe_resize_pool();
}

void Dispatcher::pin(const std::string& key, std::optional<DispatchMode> mode) {
    std::lock_guard lock(mutex_);
    auto& lane = lanes_[key];
    lane.registered = true;
    if (mode) {
        pins_[key] = *mode;
    } else {
        pins_.erase(key);
    }
    maybe_switch();
    maybe_resize_pool();
    rebalance();
}

void Dispatcher::start_per_key_worker(const std::string& key) {
    auto found = per_key_workers_.find(key);
    if (found != per_key_workers_.end() && !found->second->stop) {
        found->second->wake.notify_one();
        return;
    }
    if (found != per_key_workers_.end()) {  // a stopped worker for this key: retire it
        retired_.push_back(std::move(found->second));
        per_key_workers_.erase(found);
    }
    auto worker = std::make_unique<Worker>();
    worker->thread = std::thread([this, key] { run_per_key(key); });
    per_key_workers_.emplace(key, std::move(worker));
}

bool Dispatcher::submit(const std::string& key, Job job) {
    std::unique_lock lock(mutex_);
    if (stopping_) return false;
    auto& lane = lanes_[key];
    lane.registered = true;
    maybe_switch();
    maybe_resize_pool();
    if (mode_for_key(key) == DispatchMode::Inline) {
        lock.unlock();
        job();
        return true;
    }
    lane.jobs.push_back(std::move(job));
    ++outstanding_;
    if (mode_for_key(key) == DispatchMode::PerKey) {
        start_per_key_worker(key);
        return true;
    }
    if (lane.pool_worker == nullptr || !active_worker(lane.pool_worker)) {
        lane.pool_worker = desired_worker(key);
    }
    if (lane.pool_worker != nullptr) enqueue_pool_key(key, *lane.pool_worker);
    return true;
}

DispatchMode Dispatcher::mode_for_key(const std::string& key) const {
    if (const auto pinned = pins_.find(key); pinned != pins_.end() && pinned->second != DispatchMode::Auto) {
        return pinned->second;
    }
    return current_;
}

std::size_t Dispatcher::registered_key_count() const {
    return std::ranges::count_if(lanes_, [](const auto& entry) { return entry.second.registered; });
}

std::size_t Dispatcher::pooled_key_count() const {
    return std::ranges::count_if(lanes_, [this](const auto& entry) {
        return entry.second.registered && mode_for_key(entry.first) == DispatchMode::Pool;
    });
}

std::size_t Dispatcher::desired_pool_threads() const {
    const std::size_t pooled_keys = pooled_key_count();
    const bool pool_selected = current_ == DispatchMode::Pool || pooled_keys != 0;
    if (!pool_selected) return 0;
    const std::size_t from_keys = pooled_keys == 0 ? 1 :
        (pooled_keys + options_.topics_per_thread - 1) / options_.topics_per_thread;
    return std::clamp(from_keys, options_.minimum_threads, options_.maximum_threads);
}

Dispatcher::Worker* Dispatcher::desired_worker(const std::string& key) const {
    if (pool_.empty()) return nullptr;
    return pool_[std::hash<std::string>{}(key) % pool_.size()].get();
}

bool Dispatcher::active_worker(const Worker* worker) const {
    return std::ranges::any_of(pool_, [worker](const auto& candidate) {
        return candidate.get() == worker;
    });
}

void Dispatcher::enqueue_pool_key(const std::string& key, Worker& worker) {
    auto lane = lanes_.find(key);
    if (lane == lanes_.end() || lane->second.running || lane->second.queued ||
        lane->second.jobs.empty()) {
        return;
    }
    lane->second.queued = true;
    lane->second.pool_worker = &worker;
    worker.ready.push_back(key);
    worker.wake.notify_one();
}

void Dispatcher::resize_pool(std::size_t threads) {
    const std::size_t previous = pool_.size();
    if (threads == previous) return;
    if (threads > previous) {
        while (pool_.size() < threads) {
            auto worker = std::make_unique<Worker>();
            auto* worker_pointer = worker.get();
            pool_.push_back(std::move(worker));
            worker_pointer->thread = std::thread([this, worker_pointer] {
                run_pool_worker(worker_pointer);
            });
        }
    } else {
        while (pool_.size() > threads) {
            auto worker = std::move(pool_.back());
            pool_.pop_back();
            worker->ready.clear();
            worker->stop = true;
            worker->wake.notify_all();
            retired_.push_back(std::move(worker));
        }
    }
    rebalance();
    if (log_) {
        log_(LogLevel::Info,
             "dispatch: " + std::to_string(registered_key_count()) + " topics; pool " +
                 std::to_string(previous) + " \xE2\x86\x92 " + std::to_string(threads) + " threads");
    }
}

void Dispatcher::rebalance() {
    for (auto& worker : pool_) worker->ready.clear();
    for (auto& [key, lane] : lanes_) {
        lane.queued = false;
        const auto mode = mode_for_key(key);
        if (mode == DispatchMode::Pool) {
            if (!lane.running) lane.pool_worker = desired_worker(key);
            if (!lane.running && lane.pool_worker != nullptr && !lane.jobs.empty()) {
                enqueue_pool_key(key, *lane.pool_worker);
            }
            continue;
        }
        if (!lane.running) lane.pool_worker = nullptr;
        if (mode == DispatchMode::PerKey && !lane.jobs.empty()) start_per_key_worker(key);
    }
    for (auto& [key, worker] : per_key_workers_) worker->wake.notify_all();
    for (auto& worker : pool_) worker->wake.notify_all();
}

void Dispatcher::maybe_resize_pool() {
    std::size_t target = desired_pool_threads();
    if (target < pool_.size() && target >= options_.minimum_threads) {
        const std::size_t shrink_boundary = target * options_.topics_per_thread;
        const std::size_t margin = std::min(options_.shrink_hysteresis_keys, shrink_boundary);
        if (pooled_key_count() > shrink_boundary - margin) target = pool_.size();
    }
    resize_pool(target);
}

void Dispatcher::note_done(std::unique_lock<std::mutex>& lock) {
    (void)lock;
    if (outstanding_ > 0) --outstanding_;
    if (outstanding_ == 0) idle_.notify_all();
}

void Dispatcher::run_per_key(const std::string& key) {
    std::unique_lock lock(mutex_);
    auto* worker = per_key_workers_.at(key).get();
    while (true) {
        const bool still_mine = mode_for_key(key) == DispatchMode::PerKey;
        auto lane = lanes_.find(key);
        const bool has_work = lane != lanes_.end() && !lane->second.jobs.empty();
        if (!still_mine && !stopping_) {
            // The pool took over: hand any queued work to it and exit.
            if (has_work) {
                lane->second.pool_worker = desired_worker(key);
                if (lane->second.pool_worker != nullptr) {
                    enqueue_pool_key(key, *lane->second.pool_worker);
                }
            }
            worker->stop = true;
            return;
        }
        if (!has_work || lane->second.running) {
            if (worker->stop || stopping_) {
                if (!has_work) return;
            }
            worker->wake.wait(lock);
            continue;
        }
        auto job = std::move(lane->second.jobs.front());
        lane->second.jobs.pop_front();
        lane->second.running = true;
        lock.unlock();
        {
            DispatchJobScope dispatch_job;
            job();
        }
        lock.lock();
        auto after = lanes_.find(key);
        if (after != lanes_.end()) after->second.running = false;
        note_done(lock);
        if (after != lanes_.end() && !after->second.registered && after->second.jobs.empty()) {
            lanes_.erase(after);
        }
    }
}

void Dispatcher::run_pool_worker(Worker* worker) {
    std::unique_lock lock(mutex_);
    while (true) {
        if (worker->ready.empty()) {
            if (worker->stop) return;
            worker->wake.wait(lock);
            continue;
        }
        const std::string key = worker->ready.front();
        worker->ready.pop_front();
        auto lane = lanes_.find(key);
        if (lane != lanes_.end()) lane->second.queued = false;
        if (lane == lanes_.end() || lane->second.jobs.empty()) continue;
        if (mode_for_key(key) != DispatchMode::Pool || lane->second.pool_worker != worker) {
            if (mode_for_key(key) == DispatchMode::PerKey) {
                start_per_key_worker(key);
            } else if (mode_for_key(key) == DispatchMode::Pool) {
                lane->second.pool_worker = desired_worker(key);
                if (lane->second.pool_worker != nullptr) {
                    enqueue_pool_key(key, *lane->second.pool_worker);
                }
            }
            continue;
        }
        if (lane->second.running) {  // a retiring per-key worker is mid-job: come back
            enqueue_pool_key(key, *worker);
            worker->wake.wait_for(lock, std::chrono::milliseconds(1));
            continue;
        }
        for (std::size_t turn = 0; turn < options_.per_turn_cap; ++turn) {
            auto current = lanes_.find(key);
            if (current == lanes_.end() || current->second.jobs.empty()) break;
            auto job = std::move(current->second.jobs.front());
            current->second.jobs.pop_front();
            current->second.running = true;
            lock.unlock();
            {
                DispatchJobScope dispatch_job;
                job();
            }
            lock.lock();
            current = lanes_.find(key);
            if (current == lanes_.end()) break;
            current->second.running = false;
            note_done(lock);
            if (mode_for_key(key) != DispatchMode::Pool || desired_worker(key) != worker) break;
        }
        auto after = lanes_.find(key);
        if (after == lanes_.end()) continue;
        if (!after->second.registered && after->second.jobs.empty()) {
            lanes_.erase(after);
            continue;
        }
        if (mode_for_key(key) == DispatchMode::PerKey) {
            after->second.pool_worker = nullptr;
            if (!after->second.jobs.empty()) start_per_key_worker(key);
        } else if (mode_for_key(key) == DispatchMode::Pool) {
            after->second.pool_worker = desired_worker(key);
            if (after->second.pool_worker != nullptr && !after->second.jobs.empty()) {
                enqueue_pool_key(key, *after->second.pool_worker);
            }
        }
    }
}

void Dispatcher::maybe_switch() {
    if (options_.mode != DispatchMode::Auto) return;
    const std::size_t keys = registered_key_count();
    if (current_ == DispatchMode::PerKey && keys > options_.topics_per_thread) {
        switch_to(DispatchMode::Pool, std::to_string(keys) + " topics > explicit Auto threshold " +
                                          std::to_string(options_.topics_per_thread));
    } else if (current_ == DispatchMode::Pool &&
               keys + options_.shrink_hysteresis_keys <= options_.topics_per_thread) {
        switch_to(DispatchMode::PerKey,
                  std::to_string(keys) + " topics below explicit Auto threshold by " +
                      std::to_string(options_.shrink_hysteresis_keys));
    }
}

void Dispatcher::switch_to(DispatchMode mode, std::string reason) {
    current_ = mode;
    if (log_) log_(LogLevel::Info, "dispatch: " + reason);
    maybe_resize_pool();
    rebalance();
}

void Dispatcher::remove_key(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto lane = lanes_.find(key);
    if (lane != lanes_.end()) lane->second.registered = false;
    if (lane != lanes_.end() && lane->second.jobs.empty() && !lane->second.running) {
        lanes_.erase(lane);
    }
    if (auto worker = per_key_workers_.find(key); worker != per_key_workers_.end()) {
        worker->second->stop = true;
        worker->second->wake.notify_one();
    }
    pins_.erase(key);
    maybe_switch();
    maybe_resize_pool();
    rebalance();
}

void Dispatcher::set_idle_pump(std::function<void()> pump) {
    std::lock_guard lock(mutex_);
    idle_pump_ = std::move(pump);
}

void Dispatcher::wait_idle(std::unique_lock<std::mutex>& lock) {
    while (outstanding_ != 0) {
        if (!idle_pump_) {
            idle_.wait(lock, [&] { return outstanding_ == 0; });
            return;
        }
        idle_.wait_for(lock, std::chrono::milliseconds(5), [&] { return outstanding_ == 0; });
        if (outstanding_ == 0) return;
        auto pump = idle_pump_;
        lock.unlock();
        pump();
        lock.lock();
    }
}

void Dispatcher::drain() {
    std::unique_lock lock(mutex_);
    wait_idle(lock);
}

void Dispatcher::stop() {
    {
        std::unique_lock lock(mutex_);
        if (stopping_ && per_key_workers_.empty() && pool_.empty() && retired_.empty()) return;
        stopping_ = true;
        wait_idle(lock);
        for (auto& [key, worker] : per_key_workers_) {
            worker->stop = true;
            worker->wake.notify_all();
        }
        for (auto& worker : pool_) {
            worker->stop = true;
            worker->wake.notify_all();
        }
    }
    std::vector<std::unique_ptr<Worker>> joining;
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, worker] : per_key_workers_) joining.push_back(std::move(worker));
        per_key_workers_.clear();
        for (auto& worker : pool_) joining.push_back(std::move(worker));
        pool_.clear();
        for (auto& worker : retired_) joining.push_back(std::move(worker));
        retired_.clear();
    }
    for (auto& worker : joining) {
        if (worker->thread.joinable()) worker->thread.join();
    }
}

}  // namespace zetta::pubsub
