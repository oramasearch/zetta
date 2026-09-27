#pragma once

// Internal: one topic (or inbox) of the pub/sub facade. Not part of the public
// surface; PubSub and Subscription are. See docs/pubsub/topics.md and
// docs/pubsub/delivery.md.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zetta/pubsub/pubsub.hpp"

namespace zetta::pubsub {

struct GroupState {
    std::vector<std::uint64_t> positions;  // one per level
    std::size_t members = 0;
};

struct InFlight {
    Event event;
    SteadyTime deadline;
};

struct Subscription::State {
    std::uint64_t id = 0;
    Address subscriber;
    SubscribeOptions options;
    std::vector<std::uint64_t> positions;  // one per level (unused when grouped)
    std::shared_ptr<GroupState> group;
    std::map<std::uint64_t, InFlight> in_flight;  // by sequence
    std::unordered_set<std::string> acked_ids;    // exactly-once
    std::deque<std::string> acked_order;
    std::uint64_t missed = 0;       // pending gap notice
    std::uint64_t missed_total = 0;
    bool connected = true;
    std::optional<SteadyTime> disconnected_at;
    bool closed = false;
    std::string close_reason;
};

// Work a receive produced that must run after the topic's lock is released
// (dead letters go to another topic).
struct DeferredDeadLetter {
    Event event;
    std::string reason;
};

class Topic : public std::enable_shared_from_this<Topic> {
public:
    Topic(PubSub* owner, Address address, TopicSettings settings,
          std::optional<Address> inbox_owner = std::nullopt);

    const Address& address() const { return address_; }
    TopicSettings settings() const;
    bool is_inbox() const { return inbox_owner_.has_value(); }
    const std::optional<Address>& inbox_owner() const { return inbox_owner_; }

    enum class AppendStatus { Ok, Full, Closed };
    struct AppendOutcome {
        AppendStatus status = AppendStatus::Ok;
        std::uint64_t sequence = 0;
        std::string reason;
    };
    // Enforce limits (spill / hold / skip / reject per policy), then append.
    AppendOutcome append(Event event);

    std::shared_ptr<Subscription::State> add_subscription(const Address& subscriber,
                                                          SubscribeOptions options,
                                                          std::uint64_t id);
    std::optional<Delivery> receive(Subscription::State& state, std::chrono::milliseconds timeout,
                                    std::vector<DeferredDeadLetter>& dead_letters);
    bool ack(Subscription::State& state, std::uint64_t sequence);
    void disconnect(Subscription::State& state);
    bool closed(const Subscription::State& state) const;
    std::string close_reason(const Subscription::State& state) const;
    std::uint64_t missed(const Subscription::State& state) const;

    // Send a dead letter a receive produced (called without the topic's lock).
    void dead_letter_from_receive(const DeferredDeadLetter& letter);

    // Close every subscription (drop, shutdown) and wake every waiter.
    void close_all(const std::string& reason);
    // The facade is going away: stop calling back into it.
    void detach();

    std::size_t subscription_count() const;
    std::optional<SteadyTime> idle_since() const;
    std::uint64_t memory_bytes() const;
    std::uint64_t disk_bytes() const;
    std::uint64_t held_events() const;
    std::uint64_t next_sequence() const { return next_sequence_.load(); }
    TopicStats stats() const;

    // Retention sweep, lag policy, spill logging. Returns expired subscriber
    // addresses (for the log).
    void maintain(SteadyTime now, std::chrono::milliseconds spill_log_interval);

    // Held events with sequence >= from, oldest first, at most `max`; waits up to
    // `wait` when there are none.
    std::vector<Event> read_from(std::uint64_t from_sequence, std::size_t max, std::chrono::milliseconds wait);

    // Every held event per level (the shutdown flush; spilled rings are read back).
    std::vector<std::vector<Event>> snapshot();
    // Restore events per level at start-up.
    void restore(std::vector<std::vector<Event>> levels, std::uint64_t next_sequence);
    std::filesystem::path spill_directory() const;

private:
    std::size_t level_for(std::uint8_t priority) const;
    EffectiveDelivery effective_for(const Event& event, const Subscription::State& state) const;
    std::optional<Delivery> next_event(Subscription::State& state,
                                       std::vector<DeferredDeadLetter>& dead_letters);  // mutex_ held
    std::optional<std::size_t> pick_level(Subscription::State& state);                  // mutex_ held
    std::uint64_t& position_for(Subscription::State& state, std::size_t level);         // mutex_ held
    std::uint64_t min_position(std::size_t level) const;                                // mutex_ held
    void release_consumed();                                                             // mutex_ held
    bool over_limit(std::uint64_t incoming) const;
    OverflowPolicy overflow_policy() const;
    bool spill_one();
    bool drop_oldest();
    bool disconnect_slowest();                                                           // mutex_ held
    void note_spill(std::uint64_t freed);
    void close_state(Subscription::State& state, std::string reason);                    // mutex_ held
    PubSub* owner() const { return owner_.load(); }
    void log(LogLevel level, const std::string& message) const;

    std::atomic<PubSub*> owner_;
    Address address_;
    std::optional<Address> inbox_owner_;
    mutable std::mutex settings_mutex_;
    TopicSettings settings_;

    std::vector<std::unique_ptr<EventStore>> levels_;
    std::atomic<std::uint64_t> next_sequence_{1};

    std::mutex append_mutex_;  // serializes appends (sequence order + limits); taken before mutex_
    mutable std::mutex mutex_;  // subscriptions, groups, claims, spill state
    std::condition_variable published_;
    std::condition_variable released_;
    std::uint64_t publish_generation_ = 0;
    std::size_t held_publishers_ = 0;
    bool closing_ = false;

    std::map<std::uint64_t, std::shared_ptr<Subscription::State>> subscriptions_;
    std::map<std::string, std::shared_ptr<GroupState>> groups_;
    std::unordered_map<std::uint64_t, std::uint32_t> claims_;  // sequence -> claims left
    std::unordered_set<std::uint64_t> ttl_reported_;
    std::optional<SteadyTime> idle_since_;

    std::optional<std::chrono::system_clock::time_point> spilling_since_;
    SteadyTime last_spill_log_{};
    std::uint64_t spilled_total_ = 0;
};

}  // namespace zetta::pubsub
