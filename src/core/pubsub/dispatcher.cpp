#include "zetta/pubsub/dispatcher.hpp"

#include <algorithm>
#include <functional>

namespace zetta::pubsub {

std::string_view to_string(DispatchMode mode) {
    switch (mode) {
        case DispatchMode::Auto: return "auto";
        case DispatchMode::PerKey: return "thread per topic";
        case DispatchMode::Pool: return "pool";
        case DispatchMode::Inline: return "inline";
    }
    return "auto";
}

Dispatcher::Dispatcher(DispatcherOptions options, LogSink log)
    : options_(options), log_(std::move(log)) {
    if (options_.pool_threads == 0) options_.pool_threads = 1;
    if (options_.per_turn_cap == 0) options_.per_turn_cap = 1;
    switch (options_.mode) {
        case DispatchMode::Pool: current_ = DispatchMode::Pool; break;
        case DispatchMode::Inline: current_ = DispatchMode::Inline; break;
        default: current_ = DispatchMode::PerKey; break;
    }
}

Dispatcher::~Dispatcher() { stop(); }

DispatchMode Dispatcher::effective_mode() const {
    std::lock_guard lock(mutex_);
    return current_;
}

std::size_t Dispatcher::key_count() const {
    std::lock_guard lock(mutex_);
    return lanes_.size();
}

void Dispatcher::pin(const std::string& key, std::optional<DispatchMode> mode) {
    std::lock_guard lock(mutex_);
    if (mode) {
        pins_[key] = *mode;
    } else {
        pins_.erase(key);
    }
}

std::size_t Dispatcher::pool_worker_for(const std::string& key) const {
    return std::hash<std::string>{}(key) % pool_.size();
}

void Dispatcher::ensure_pool() {
    if (!pool_.empty()) return;
    for (std::size_t index = 0; index < options_.pool_threads; ++index) {
        pool_.push_back(std::make_unique<Worker>());
    }
    for (std::size_t index = 0; index < pool_.size(); ++index) {
        pool_[index]->thread = std::thread([this, index] { run_pool_worker(index); });
    }
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
    const auto pinned = pins_.find(key);
    const bool run_inline = (pinned != pins_.end() && pinned->second == DispatchMode::Inline) ||
                            (pinned == pins_.end() && current_ == DispatchMode::Inline);
    if (run_inline) {
        lock.unlock();
        job();
        return true;
    }
    auto& lane = lanes_[key];
    lane.jobs.push_back(std::move(job));
    ++outstanding_;
    maybe_switch();
    const bool per_key = (pinned != pins_.end() && pinned->second == DispatchMode::PerKey) ||
                         current_ == DispatchMode::PerKey;
    if (per_key) {
        start_per_key_worker(key);
        return true;
    }
    ensure_pool();
    auto& worker = *pool_[pool_worker_for(key)];
    if (std::ranges::find(worker.ready, key) == worker.ready.end()) worker.ready.push_back(key);
    worker.wake.notify_one();
    return true;
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
        auto pinned = pins_.find(key);
        const bool still_mine = (pinned != pins_.end() && pinned->second == DispatchMode::PerKey) ||
                                current_ == DispatchMode::PerKey;
        auto lane = lanes_.find(key);
        const bool has_work = lane != lanes_.end() && !lane->second.jobs.empty();
        if (!still_mine && !stopping_) {
            // The pool took over: hand any queued work to it and exit.
            if (has_work) {
                ensure_pool();
                auto& pool_worker = *pool_[pool_worker_for(key)];
                if (std::ranges::find(pool_worker.ready, key) == pool_worker.ready.end()) {
                    pool_worker.ready.push_back(key);
                }
                pool_worker.wake.notify_one();
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
        job();
        lock.lock();
        lanes_[key].running = false;
        note_done(lock);
    }
}

void Dispatcher::run_pool_worker(std::size_t index) {
    std::unique_lock lock(mutex_);
    auto* worker = pool_[index].get();
    while (true) {
        if (worker->ready.empty()) {
            if (worker->stop) return;
            worker->wake.wait(lock);
            continue;
        }
        const std::string key = worker->ready.front();
        worker->ready.pop_front();
        auto lane = lanes_.find(key);
        if (lane == lanes_.end() || lane->second.jobs.empty()) continue;
        if (lane->second.running) {  // a retiring per-key worker is mid-job: come back
            worker->ready.push_back(key);
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
            job();
            lock.lock();
            lanes_[key].running = false;
            note_done(lock);
        }
        auto after = lanes_.find(key);
        if (after != lanes_.end() && !after->second.jobs.empty()) worker->ready.push_back(key);
    }
}

void Dispatcher::maybe_switch() {
    if (options_.mode != DispatchMode::Auto) return;
    const std::size_t keys = lanes_.size();
    if (current_ == DispatchMode::PerKey && keys > options_.threshold) {
        switch_to(DispatchMode::Pool, std::to_string(keys) + " topics > threshold " +
                                          std::to_string(options_.threshold) + "; moving to a pool of " +
                                          std::to_string(options_.pool_threads) + " threads");
    } else if (current_ == DispatchMode::Pool && keys <= options_.threshold / 2) {
        switch_to(DispatchMode::PerKey, std::to_string(keys) + " topics <= " +
                                            std::to_string(options_.threshold / 2) +
                                            " (half the threshold); back to a thread per topic");
    }
}

void Dispatcher::switch_to(DispatchMode mode, std::string reason) {
    current_ = mode;
    if (log_) log_(LogLevel::Info, "dispatch: " + reason);
    if (mode == DispatchMode::Pool) {
        ensure_pool();
        for (auto& [key, worker] : per_key_workers_) worker->wake.notify_one();  // they hand off and exit
    }
}

void Dispatcher::remove_key(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto lane = lanes_.find(key);
    if (lane != lanes_.end() && lane->second.jobs.empty() && !lane->second.running) lanes_.erase(lane);
    if (auto worker = per_key_workers_.find(key); worker != per_key_workers_.end()) {
        worker->second->stop = true;
        worker->second->wake.notify_one();
    }
    pins_.erase(key);
    maybe_switch();
}

void Dispatcher::drain() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return outstanding_ == 0; });
}

void Dispatcher::stop() {
    {
        std::unique_lock lock(mutex_);
        if (stopping_ && per_key_workers_.empty() && pool_.empty() && retired_.empty()) return;
        stopping_ = true;
        idle_.wait(lock, [&] { return outstanding_ == 0; });
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
