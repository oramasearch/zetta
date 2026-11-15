// SPDX-License-Identifier: MIT

#include "topic.hpp"

#include <algorithm>
#include <limits>

namespace zetta::pubsub {
namespace {

using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t kAckedIdMemory = 100'000;  // exactly-once dedupe window

std::string format_duration(std::chrono::seconds seconds) {
    const auto total = seconds.count();
    if (total < 60) return std::to_string(total) + "s";
    return std::to_string(total / 60) + "m" + std::to_string(total % 60) + "s";
}

}  // namespace

Topic::Topic(PubSub* owner, Address address, TopicSettings settings,
             std::optional<Address> inbox_owner)
    : owner_(owner), address_(std::move(address)), inbox_owner_(std::move(inbox_owner)),
      settings_(std::move(settings)) {
    if (settings_.priority_levels == 0) settings_.priority_levels = 1;
    if (settings_.store == StoreKind::Queue) settings_.priority_levels = 1;
    for (std::size_t level = 0; level < settings_.priority_levels; ++level) {
        if (settings_.store == StoreKind::Queue) {
            levels_.push_back(std::make_unique<QueueStore>());
        } else {
            levels_.push_back(std::make_unique<TopicRingStack>(
                RingStackOptions{.ring_capacity = settings_.ring_capacity,
                                 .free_list_limit = 2,
                                 .inline_threshold = settings_.inline_threshold}));
        }
    }
    idle_since_ = SteadyClock::now();
}

TopicSettings Topic::settings() const {
    std::lock_guard lock(settings_mutex_);
    return settings_;
}

void Topic::log(LogLevel level, const std::string& message) const {
    if (auto* owner = this->owner(); owner != nullptr) owner->log(level, message);
}

std::size_t Topic::level_for(std::uint8_t priority) const {
    return std::min(levels_.size() - 1, static_cast<std::size_t>(priority) * levels_.size() / 256);
}

std::filesystem::path Topic::spill_directory() const {
    return owner() != nullptr ? owner()->spill_directory_for(address_) : std::filesystem::path();
}

// --- Limits ------------------------------------------------------------------

std::uint64_t Topic::memory_bytes() const {
    std::uint64_t total = 0;
    for (const auto& level : levels_) total += level->memory_bytes();
    return total;
}

std::uint64_t Topic::disk_bytes() const {
    std::uint64_t total = 0;
    for (const auto& level : levels_) total += level->disk_bytes();
    return total;
}

std::uint64_t Topic::held_events() const {
    std::uint64_t total = 0;
    for (const auto& level : levels_) total += level->held_events();
    return total;
}

bool Topic::over_limit(std::uint64_t incoming) const {
    auto* owner = this->owner();
    if (owner == nullptr) return false;
    const auto settings = this->settings();
    const std::uint64_t cap = settings.memory_cap_bytes.value_or(owner->limits().memory_bytes);
    if (memory_bytes() + incoming > cap) return true;
    return owner->total_memory_bytes() + incoming > owner->limits().memory_bytes;
}

OverflowPolicy Topic::overflow_policy() const {
    const auto settings = this->settings();
    if (settings.overflow) return *settings.overflow;
    const auto guarantee = settings.delivery.guarantee.value_or(Guarantee::AtMostOnce);
    return guarantee == Guarantee::AtMostOnce ? OverflowPolicy::SkipAhead : OverflowPolicy::SpillToDisk;
}

std::uint64_t Topic::min_position(std::size_t level) const {
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    for (const auto& [id, state] : subscriptions_) {
        if (state->closed || state->group) continue;
        minimum = std::min(minimum, state->positions[level]);
    }
    for (const auto& [name, group] : groups_) {
        if (group->members == 0) continue;
        minimum = std::min(minimum, group->positions[level]);
    }
    return minimum;
}

void Topic::release_consumed() {
    // An explicit retention policy keeps history independently of consumption.
    const auto configured = settings();
    if (configured.retention || configured.max_events) return;
    // With no subscribers, events are held (a later subscriber may start at the
    // oldest) until the limits say otherwise.
    for (std::size_t level = 0; level < levels_.size(); ++level) {
        const auto minimum = min_position(level);
        if (minimum == std::numeric_limits<std::uint64_t>::max()) continue;
        levels_[level]->release_before(minimum);
    }
    // Claims and TTL bookkeeping below every level's first position can go.
    std::uint64_t oldest_sequence = std::numeric_limits<std::uint64_t>::max();
    for (const auto& level : levels_) {
        const auto positions = level->sequence_positions();
        if (!positions.empty()) oldest_sequence = std::min(oldest_sequence, positions.front().second);
    }
    if (oldest_sequence == std::numeric_limits<std::uint64_t>::max()) oldest_sequence = next_sequence_.load();
    std::erase_if(claims_, [&](const auto& entry) { return entry.first < oldest_sequence; });
    std::erase_if(ttl_reported_, [&](std::uint64_t sequence) { return sequence < oldest_sequence; });
    for (auto& [id, state] : subscriptions_) {
        std::erase_if(state->delivered_sequences, [&](auto sequence) { return sequence < oldest_sequence; });
    }
}

bool Topic::spill_one() {
    const auto settings = this->settings();
    auto* owner = this->owner();
    if (settings.lifecycle == TopicLifecycle::Ephemeral || owner == nullptr) return false;
    const std::uint64_t disk_cap = settings.disk_cap_bytes.value_or(owner->limits().spill_disk_bytes);
    if (disk_bytes() >= disk_cap || owner->total_disk_bytes() >= owner->limits().spill_disk_bytes) {
        return false;
    }
    // Spill from the level holding the most memory.
    std::size_t fullest = 0;
    for (std::size_t level = 1; level < levels_.size(); ++level) {
        if (levels_[level]->memory_bytes() > levels_[fullest]->memory_bytes()) fullest = level;
    }
    auto* stack = dynamic_cast<TopicRingStack*>(levels_[fullest].get());
    if (stack == nullptr) return false;  // the queue store does not spill
    const auto freed = stack->spill_oldest(spill_directory() / ("level-" + std::to_string(fullest)));
    if (freed == 0) return false;
    note_spill(freed);
    return true;
}

void Topic::note_spill(std::uint64_t freed) {
    std::lock_guard lock(mutex_);
    spilled_total_ += freed;
    if (!spilling_since_) {
        spilling_since_ = SystemClock::now();
        last_spill_log_ = SteadyClock::now();
        std::string slowest = "none";
        std::uint64_t slowest_position = std::numeric_limits<std::uint64_t>::max();
        for (const auto& [id, state] : subscriptions_) {
            if (state->closed) continue;
            const auto position = state->group ? state->group->positions[0] : state->positions[0];
            if (position < slowest_position) {
                slowest_position = position;
                slowest = state->subscriber.to_string() + " at position " + std::to_string(position);
            }
        }
        log(LogLevel::Warn, address_.to_string() + " spilling to disk: memory " +
                                format_bytes(settings().memory_cap_bytes.value_or(
                                    owner() != nullptr ? owner()->limits().memory_bytes : 0)) +
                                " full; slowest subscriber " + slowest + "; expect higher latency");
    }
}

bool Topic::drop_oldest() {
    // Release the oldest ring's worth from the fullest level; readers behind it
    // see a gap notice on their next receive.
    std::size_t fullest = 0;
    for (std::size_t level = 1; level < levels_.size(); ++level) {
        if (levels_[level]->memory_bytes() > levels_[fullest]->memory_bytes()) fullest = level;
    }
    auto& store = *levels_[fullest];
    const auto first = store.first_position();
    const auto next = store.next_position();
    if (first >= next) return false;
    const auto step = std::max<std::uint64_t>(1, std::min<std::uint64_t>(settings().ring_capacity, next - first));
    store.release_before(first + step);
    return true;
}

void Topic::close_state(Subscription::State& state, std::string reason) {
    if (state.closed) return;
    state.closed = true;
    state.close_reason = std::move(reason);
    if (state.group && state.group->members > 0) --state.group->members;
}

void Topic::dead_letter_from_receive(const DeferredDeadLetter& letter) {
    auto* owner = this->owner();
    if (owner == nullptr) return;
    if (address_ == PubSub::dead_letter_address()) {
        owner->log(LogLevel::Warn, "dropped an expired event from " + address_.to_string() +
                                       " (a dead letter is never dead-lettered again)");
        return;
    }
    owner->dead_letter(letter.event, letter.reason);
}

bool Topic::disconnect_slowest() {
    std::shared_ptr<Subscription::State> slowest;
    std::uint64_t slowest_position = std::numeric_limits<std::uint64_t>::max();
    for (const auto& [id, state] : subscriptions_) {
        if (state->closed) continue;
        const auto position = state->group ? state->group->positions[0] : state->positions[0];
        if (position < slowest_position) {
            slowest_position = position;
            slowest = state;
        }
    }
    if (!slowest) return false;
    close_state(*slowest, "disconnected: " + address_.to_string() + " memory full");
    log(LogLevel::Warn, address_.to_string() + ": disconnected slowest subscriber " +
                            slowest->subscriber.to_string() + " (memory full)");
    published_.notify_all();
    return true;
}

// --- Append ------------------------------------------------------------------

Topic::AppendOutcome Topic::append(Event event) {
    AppendOutcome outcome;
    std::vector<std::shared_ptr<runtime::async_event>> waiters;
    std::lock_guard append_lock(append_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (closing_) {
            outcome.status = AppendStatus::Closed;
            outcome.reason = address_.to_string() + " is closed";
            return outcome;
        }
    }
    const auto incoming = event.memory_bytes();
    if (over_limit(incoming)) {
        {
            std::lock_guard lock(mutex_);
            release_consumed();
        }
        const auto policy = overflow_policy();
        const auto hold_limit =
            owner() != nullptr ? owner()->limits().hold_publisher : std::chrono::milliseconds(0);
        auto hold = [&]() -> bool {
            Dispatcher::assert_blocking_allowed();
            if (!Dispatcher::blocking_allowed()) return false;
            std::unique_lock lock(mutex_);
            ++held_publishers_;
            const bool freed = released_.wait_for(lock, hold_limit, [&] {
                return closing_ || !over_limit(incoming);
            });
            --held_publishers_;
            return freed && !closing_;
        };
        while (over_limit(incoming)) {
            bool keep_going = true;
            switch (policy) {
                case OverflowPolicy::Unbounded:
                    keep_going = false;
                    break;
                case OverflowPolicy::SpillToDisk:
                    if (spill_one()) break;
                    if (!hold()) {
                        outcome.status = AppendStatus::Full;
                        outcome.reason = address_.to_string() + " is full: memory and spill disk at their limits, held " +
                                         std::to_string(hold_limit.count()) + " ms";
                        return outcome;
                    }
                    break;
                case OverflowPolicy::HoldPublisher:
                    if (!hold()) {
                        outcome.status = AppendStatus::Full;
                        outcome.reason = address_.to_string() + " is full: held " +
                                         std::to_string(hold_limit.count()) + " ms";
                        return outcome;
                    }
                    break;
                case OverflowPolicy::SkipAhead:
                    if (!drop_oldest()) keep_going = false;
                    break;
                case OverflowPolicy::Disconnect: {
                    std::lock_guard lock(mutex_);
                    if (!disconnect_slowest()) {
                        if (!drop_oldest()) keep_going = false;
                    }
                    release_consumed();
                    break;
                }
                case OverflowPolicy::RejectNew:
                    outcome.status = AppendStatus::Full;
                    outcome.reason = address_.to_string() + " is full (policy reject_new)";
                    return outcome;
            }
            if (!keep_going) break;
        }
    }

    event.sequence = next_sequence_.fetch_add(1);
    if (event.published_at == SystemClock::time_point{}) event.published_at = SystemClock::now();
    const auto settings = this->settings();
    if (!event.expires_at && settings.default_ttl) {
        event.expires_at = event.published_at + *settings.default_ttl;
    }
    const auto level = level_for(event.delivery.priority.value_or(settings.delivery.priority.value_or(0)));
    const auto recipients = event.delivery.recipients;
    const auto sequence = event.sequence;
    levels_[level]->append(std::move(event));
    {
        std::lock_guard lock(mutex_);
        if (recipients && recipients->claims() != 0) claims_[sequence] = recipients->claims();
        if (auto trimmed = trim_retention(); !trimmed.ok) log(LogLevel::Warn, trimmed.reason);
        ++publish_generation_;
        idle_since_.reset();
        if (subscriptions_.empty()) idle_since_ = SteadyClock::now();
        waiters = take_waiters();
    }
    published_.notify_all();
    for (const auto& waiter : waiters) waiter->set();
    outcome.sequence = sequence;
    return outcome;
}

// --- Subscriptions -----------------------------------------------------------

std::shared_ptr<Subscription::State> Topic::add_subscription(const Address& subscriber,
                                                             SubscribeOptions options,
                                                             std::uint64_t id) {
    auto state = std::make_shared<Subscription::State>();
    state->id = id;
    state->subscriber = subscriber;
    state->positions.resize(levels_.size());
    const auto settings = this->settings();

    // A queue store feeds one reader: every subscription shares its position.
    if (settings.store == StoreKind::Queue && !options.group) options.group = "queue";
    // A subscription asking for one/count recipients without a group joins the
    // default group of that mode.
    if (!options.group && options.delivery.recipients &&
        options.delivery.recipients->mode != Recipients::Mode::All) {
        options.group = std::string(to_string(options.delivery.recipients->mode));
    }

    for (std::size_t level = 0; level < levels_.size(); ++level) {
        auto& store = *levels_[level];
        std::uint64_t position = store.next_position();
        if (options.start == StartPosition::Oldest) {
            position = store.first_position();
        } else if (options.start == StartPosition::Sequence) {
            position = store.next_position();
            for (auto candidate = store.first_position(); candidate < store.next_position(); ++candidate) {
                auto event = store.read(candidate);
                if (event && event->sequence >= options.start_sequence) {
                    position = candidate;
                    break;
                }
            }
        }
        state->positions[level] = position;
    }
    state->options = std::move(options);

    std::lock_guard lock(mutex_);
    if (state->options.group) {
        auto& group = groups_[*state->options.group];
        if (!group) {
            group = std::make_shared<GroupState>();
            group->positions = state->positions;
        }
        ++group->members;
        state->group = group;
    }
    subscriptions_.emplace(id, state);
    idle_since_.reset();
    return state;
}

std::uint64_t& Topic::position_for(Subscription::State& state, std::size_t level) {
    return state.group ? state.group->positions[level] : state.positions[level];
}

EffectiveDelivery Topic::effective_for(const Event& event, const Subscription::State& state) const {
    return resolve_delivery(event.delivery, state.options.delivery, settings().delivery);
}

std::optional<std::size_t> Topic::pick_level(Subscription::State& state) {
    // Aging first (D9): the oldest head that has waited past the aging limit,
    // in any level, goes before a fresher higher-priority event.
    const auto aging = settings().priority_aging;
    const auto now = SystemClock::now();
    std::optional<std::size_t> aged;
    std::uint64_t aged_sequence = std::numeric_limits<std::uint64_t>::max();
    std::optional<std::size_t> highest;
    for (std::size_t index = levels_.size(); index-- > 0;) {
        auto& store = *levels_[index];
        auto position = position_for(state, index);
        if (position < store.first_position()) position = store.first_position();
        if (position >= store.next_position()) continue;
        if (!highest) highest = index;
        if (levels_.size() > 1) {
            if (auto head = store.read(position); head && now - head->published_at >= aging &&
                                                  head->sequence < aged_sequence) {
                aged = index;
                aged_sequence = head->sequence;
            }
        }
    }
    return aged ? aged : highest;
}

std::optional<Delivery> Topic::next_event(Subscription::State& state,
                                          std::vector<DeferredDeadLetter>& dead_letters) {
    while (true) {
        const auto level = pick_level(state);
        if (!level) return std::nullopt;
        auto& store = *levels_[*level];
        auto& position = position_for(state, *level);
        const auto first = store.first_position();
        if (position < first) {  // released under us: a gap
            state.missed += first - position;
            state.missed_total += first - position;
            position = first;
        }
        if (position >= store.next_position()) continue;
        auto event = store.read(position);
        ++position;
        if (settings().store == StoreKind::Queue) store.release_before(position);
        if (!event) continue;

        // Time to live (D10): the first reader to find it expired dead-letters it.
        if (event->expires_at && *event->expires_at <= SystemClock::now()) {
            if (ttl_reported_.insert(event->sequence).second) {
                dead_letters.push_back({*event, "time to live expired"});
            }
            continue;
        }
        const auto effective = effective_for(*event, state);
        // Claims (D3): one / count recipients across the topic's subscriptions.
        if (!state.group) {
            if (auto claim = claims_.find(event->sequence); claim != claims_.end()) {
                if (claim->second == 0) continue;
                --claim->second;
            }
        }
        if (effective.guarantee == Guarantee::ExactlyOnce && state.acked_ids.contains(event->id)) {
            continue;
        }
        Delivery delivery;
        delivery.sequence = event->sequence;
        delivery.missed = state.missed;
        state.missed = 0;
        if (effective.guarantee != Guarantee::AtMostOnce) {
            state.in_flight[event->sequence] = {*event, SteadyClock::now() + effective.ack_deadline};
        }
        delivery.event = std::move(*event);
        state.delivered_sequences.insert(delivery.sequence);
        return delivery;
    }
}

std::optional<Delivery> Topic::receive(Subscription::State& state, std::chrono::milliseconds timeout,
                                       std::vector<DeferredDeadLetter>& dead_letters) {
    if (timeout.count() > 0) {
        Dispatcher::assert_blocking_allowed();
        if (!Dispatcher::blocking_allowed()) timeout = std::chrono::milliseconds(0);
    }
    const auto deadline = SteadyClock::now() + timeout;
    std::unique_lock lock(mutex_);
    while (true) {
        if (state.closed || closing_) return std::nullopt;
        if (!state.requested_redeliveries.empty()) {
            auto entry = state.requested_redeliveries.begin();
            Delivery delivery;
            delivery.event = std::move(entry->second);
            delivery.sequence = entry->first;
            delivery.redelivery = true;
            state.requested_redeliveries.erase(entry);
            const auto effective = effective_for(delivery.event, state);
            if (effective.guarantee != Guarantee::AtMostOnce) {
                state.in_flight[delivery.sequence] = {delivery.event, SteadyClock::now() + effective.ack_deadline};
            }
            return delivery;
        }
        // Redeliver an unacknowledged event whose deadline passed (at-least-once).
        const auto now = SteadyClock::now();
        for (auto& [sequence, flight] : state.in_flight) {
            if (flight.deadline <= now) {
                flight.deadline = now + effective_for(flight.event, state).ack_deadline;
                Delivery delivery;
                delivery.event = flight.event;
                delivery.sequence = sequence;
                delivery.redelivery = true;
                return delivery;
            }
        }
        if (auto delivery = next_event(state, dead_letters); delivery) {
            if (held_publishers_ > 0) {
                release_consumed();
                released_.notify_all();
            }
            return delivery;
        }
        if (!dead_letters.empty()) return std::nullopt;  // let the caller send them first
        if (now >= deadline) return std::nullopt;
        auto wake_at = deadline;
        for (const auto& [sequence, flight] : state.in_flight) wake_at = std::min(wake_at, flight.deadline);
        const auto generation = publish_generation_;
        published_.wait_until(lock, wake_at, [&] {
            return publish_generation_ != generation || state.closed || closing_;
        });
    }
}

bool Topic::ack(Subscription::State& state, std::uint64_t sequence) {
    auto* owner = this->owner();
    zetta::observability::ScopedSpan span(owner != nullptr ? owner->spans() : nullptr,
                                          zetta::observability::make_root_trace(), "pubsub:ack");
    span.set_attribute("intent", {{"topic", address_.to_string()},
                                  {"subscriber", state.subscriber.to_string()},
                                  {"sequence", sequence}});
    std::lock_guard lock(mutex_);
    auto flight = state.in_flight.find(sequence);
    if (flight == state.in_flight.end()) return false;
    if (effective_for(flight->second.event, state).guarantee == Guarantee::ExactlyOnce) {
        state.acked_ids.insert(flight->second.event.id);
        state.acked_order.push_back(flight->second.event.id);
        if (state.acked_order.size() > kAckedIdMemory) {
            state.acked_ids.erase(state.acked_order.front());
            state.acked_order.pop_front();
        }
    }
    state.in_flight.erase(flight);
    return true;
}

void Topic::disconnect(Subscription::State& state) {
    std::lock_guard lock(mutex_);
    if (!state.connected) return;
    state.connected = false;
    state.disconnected_at = SteadyClock::now();
    const auto retention = state.options.retention.value_or(
        state.subscriber.kind == "agent" ? std::chrono::milliseconds(kAgentInboxRetention)
                                         : std::chrono::milliseconds(0));
    if (retention.count() == 0) {
        close_state(state, state.close_reason.empty() ? "disconnected" : state.close_reason);
        subscriptions_.erase(state.id);
        if (subscriptions_.empty()) idle_since_ = SteadyClock::now();
        release_consumed();
        released_.notify_all();
    }
    published_.notify_all();
}

bool Topic::closed(const Subscription::State& state) const {
    std::lock_guard lock(mutex_);
    return state.closed || closing_;
}

std::string Topic::close_reason(const Subscription::State& state) const {
    std::lock_guard lock(mutex_);
    return state.close_reason;
}

std::uint64_t Topic::missed(const Subscription::State& state) const {
    std::lock_guard lock(mutex_);
    return state.missed_total;
}

void Topic::close_all(const std::string& reason) {
    std::vector<std::shared_ptr<runtime::async_event>> waiters;
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        for (auto& [id, state] : subscriptions_) {
            state->closed = true;
            state->close_reason = reason;
        }
        waiters = take_waiters();
    }
    published_.notify_all();
    released_.notify_all();
    for (auto& level : levels_) level->wake_all();
    for (const auto& waiter : waiters) waiter->set();
}

void Topic::detach() {
    std::lock_guard lock(mutex_);
    owner_ = nullptr;
}

TopicMutation Topic::retire() {
    std::lock_guard append_lock(append_mutex_);
    close_all("topic dropped");
    std::lock_guard lock(mutex_);
    TopicMutation result{true, {}, {}};
    for (auto& level : snapshot()) {
        for (auto& event : level) result.changes.push_back({std::move(event), std::nullopt});
    }
    return result;
}

std::size_t Topic::subscription_count() const {
    std::lock_guard lock(mutex_);
    return subscriptions_.size();
}

std::optional<SteadyTime> Topic::idle_since() const {
    std::lock_guard lock(mutex_);
    return idle_since_;
}

// --- Maintenance -------------------------------------------------------------

void Topic::maintain(SteadyTime now, std::chrono::milliseconds spill_log_interval) {
    std::lock_guard append_lock(append_mutex_);
    std::lock_guard lock(mutex_);
    if (auto trimmed = trim_retention(); !trimmed.ok) log(LogLevel::Warn, trimmed.reason);
    // Retention (D31): drop disconnected subscriptions whose retention ran out.
    for (auto entry = subscriptions_.begin(); entry != subscriptions_.end();) {
        auto& state = *entry->second;
        const auto retention = state.options.retention.value_or(
            state.subscriber.kind == "agent" ? std::chrono::milliseconds(kAgentInboxRetention)
                                             : std::chrono::milliseconds(0));
        const bool expired = !state.connected && state.disconnected_at &&
                             now - *state.disconnected_at >= retention;
        if (expired || state.closed) {
            if (expired) {
                log(LogLevel::Info, address_.to_string() + ": subscription of " +
                                        state.subscriber.to_string() + " removed after its retention");
            }
            close_state(state, expired ? "retention expired" : state.close_reason);
            entry = subscriptions_.erase(entry);
            continue;
        }
        // Lag limit per subscription (D10).
        if (state.options.max_lag_bytes && !state.group) {
            for (std::size_t level = 0; level < levels_.size(); ++level) {
                auto& store = *levels_[level];
                const auto held = store.next_position() - store.first_position();
                if (held == 0) continue;
                const auto average = std::max<std::uint64_t>(1, (store.memory_bytes() + store.disk_bytes()) / held);
                const auto lag = store.next_position() - std::max(state.positions[level], store.first_position());
                if (lag * average <= *state.options.max_lag_bytes) continue;
                if (state.options.lag_policy == OverflowPolicy::Disconnect) {
                    close_state(state, "disconnected: lag over " + format_bytes(*state.options.max_lag_bytes));
                    log(LogLevel::Warn, address_.to_string() + ": " + state.subscriber.to_string() + " " +
                                            state.close_reason);
                } else {
                    const auto allowed = *state.options.max_lag_bytes / average;
                    const auto target = store.next_position() - allowed;
                    if (target > state.positions[level]) {
                        state.missed += target - state.positions[level];
                        state.missed_total += target - state.positions[level];
                        state.positions[level] = target;
                    }
                }
            }
        }
        ++entry;
    }
    if (subscriptions_.empty() && !idle_since_) idle_since_ = now;
    release_consumed();
    released_.notify_all();

    // Spill logging (D12): every interval while spilling, once when it ends.
    if (spilling_since_) {
        const auto on_disk = disk_bytes();
        const auto since = std::chrono::duration_cast<std::chrono::seconds>(SystemClock::now() - *spilling_since_);
        if (on_disk == 0) {
            log(LogLevel::Info, address_.to_string() + " no longer spilling after " + format_duration(since) +
                                    "; " + format_bytes(spilled_total_) + " spilled in total");
            spilling_since_.reset();
            spilled_total_ = 0;
        } else if (now - last_spill_log_ >= spill_log_interval) {
            last_spill_log_ = now;
            std::string readers;
            for (const auto& [id, state] : subscriptions_) {
                if (state->closed) continue;
                if (!readers.empty()) readers += ", ";
                readers += state->subscriber.to_string();
            }
            log(LogLevel::Warn, address_.to_string() + " still spilling (" + format_duration(since) + "): " +
                                    format_bytes(on_disk) + " on disk; subscribers: " +
                                    (readers.empty() ? "none" : readers));
        }
    }
}

TopicStats Topic::stats() const {
    TopicStats stats;
    stats.address = address_.to_string();
    const auto settings = this->settings();
    stats.lifecycle = settings.lifecycle;
    stats.exposed = settings.exposed;
    stats.held_events = held_events();
    stats.memory_bytes = memory_bytes();
    stats.disk_bytes = disk_bytes();
    stats.next_sequence = next_sequence_.load();
    std::lock_guard lock(mutex_);
    stats.subscribers = subscriptions_.size();
    stats.spilling_since = spilling_since_;
    std::uint64_t worst = 0;
    for (const auto& [id, state] : subscriptions_) {
        if (state->closed) continue;
        std::uint64_t lag = 0;
        for (std::size_t level = 0; level < levels_.size(); ++level) {
            const auto position = state->group ? state->group->positions[level] : state->positions[level];
            const auto next = levels_[level]->next_position();
            if (next > position) lag += next - position;
        }
        if (lag >= worst) {
            worst = lag;
            stats.slowest_subscriber = state->subscriber.to_string();
        }
    }
    stats.slowest_lag_events = worst;
    return stats;
}

std::vector<Event> Topic::read_from(std::uint64_t from_sequence, std::size_t max,
                                   std::chrono::milliseconds wait) {
    auto collect = [&] {
        std::vector<Event> events;
        for (auto& level : levels_) {
            std::size_t count = 0;
            for (auto& [position, event] : level->entries(from_sequence, max)) {
                if (event.sequence < from_sequence) continue;
                events.push_back(std::move(event));
                if (++count >= max) break;
            }
        }
        std::ranges::sort(events, {}, &Event::sequence);
        if (events.size() > max) events.resize(max);
        return events;
    };
    auto events = collect();
    if (!events.empty() || wait.count() <= 0) return events;
    Dispatcher::assert_blocking_allowed();
    if (!Dispatcher::blocking_allowed()) return {};
    {
        std::unique_lock lock(mutex_);
        const auto generation = publish_generation_;
        published_.wait_for(lock, wait, [&] { return publish_generation_ != generation || closing_; });
    }
    return collect();
}

void Topic::register_waiter(
    const std::shared_ptr<runtime::async_event>& waiter) {
    bool closed = false;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(waiters_, [](const auto& registered) {
            return registered.expired();
        });
        closed = closing_;
        if (!closed) waiters_.push_back(waiter);
    }
    if (closed) waiter->set();
}

std::vector<std::shared_ptr<runtime::async_event>> Topic::take_waiters() {
    std::vector<std::shared_ptr<runtime::async_event>> waiters;
    waiters.reserve(waiters_.size());
    for (const auto& registered : waiters_) {
        if (auto waiter = registered.lock()) waiters.push_back(std::move(waiter));
    }
    waiters_.clear();
    return waiters;
}

std::vector<std::vector<Event>> Topic::snapshot() {
    std::vector<std::vector<Event>> result;
    for (auto& level : levels_) {
        if (auto* stack = dynamic_cast<TopicRingStack*>(level.get()); stack != nullptr) {
            result.push_back(stack->snapshot_all());
        } else {
            std::vector<Event> events;
            for (auto& [position, event] : level->entries()) events.push_back(std::move(event));
            result.push_back(std::move(events));
        }
    }
    return result;
}

void Topic::restore(std::vector<std::vector<Event>> restored, std::uint64_t next_sequence) {
    std::lock_guard append_lock(append_mutex_);
    std::uint64_t highest = next_sequence;
    for (std::size_t level = 0; level < restored.size() && level < levels_.size(); ++level) {
        for (const auto& event : restored[level]) highest = std::max(highest, event.sequence + 1);
        if (auto* stack = dynamic_cast<TopicRingStack*>(levels_[level].get()); stack != nullptr) {
            const auto count = restored[level].size();
            stack->restore(std::move(restored[level]), count + 1);
        } else {
            for (auto& event : restored[level]) levels_[level]->append(std::move(event));
        }
    }
    next_sequence_.store(std::max(highest, next_sequence_.load()));
}

nlohmann::json Topic::inspect() const {
    std::lock_guard lock(mutex_);
    nlohmann::json subscribers = nlohmann::json::array();
    std::uint64_t earliest = next_sequence_.load();
    std::uint64_t held = 0;
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> entries;
    for (const auto& level : levels_) {
        entries.push_back(level->sequence_positions());
        held += entries.back().size();
        for (const auto& [position, sequence] : entries.back()) earliest = std::min(earliest, sequence);
    }
    for (const auto& [id, state] : subscriptions_) {
        if (state->closed) continue;
        std::uint64_t next = next_sequence_.load();
        std::uint64_t lag = 0;
        const auto& positions = state->group ? state->group->positions : state->positions;
        for (std::size_t level = 0; level < entries.size(); ++level) {
            for (const auto& [position, sequence] : entries[level]) {
                if (position < positions[level]) continue;
                ++lag;
                next = std::min(next, sequence);
            }
        }
        subscribers.push_back({{"id", id}, {"subscriber", state->subscriber.to_string()},
            {"positions", positions}, {"next_sequence", next}, {"lag", lag},
            {"in_flight", state->in_flight.size()}, {"redeliveries", state->requested_redeliveries.size()},
            {"connected", state->connected}, {"group", state->options.group.value_or("")}});
    }
    return {{"topic", address_.to_string()}, {"settings", topic_settings_json(settings())},
        {"subscriber_count", subscribers.size()}, {"subscriptions", subscribers},
        {"held_events", held}, {"earliest_sequence", earliest}, {"next_sequence", next_sequence_.load()}};
}

nlohmann::json Topic::browse(std::optional<std::uint64_t> from, std::size_t limit, bool backward) {
    std::lock_guard lock(mutex_);
    std::vector<Event> events;
    std::uint64_t earliest = next_sequence_.load();
    for (const auto& level : levels_) {
        const auto positions = level->sequence_positions();
        if (!positions.empty()) earliest = std::min(earliest, positions.front().second);
        for (auto& [position, event] : level->entries(from, limit, backward)) {
            events.push_back(std::move(event));
        }
        std::ranges::sort(events, [backward](const Event& left, const Event& right) {
            return backward ? left.sequence > right.sequence : left.sequence < right.sequence;
        });
        if (events.size() > limit) events.resize(limit);
    }
    nlohmann::json result = nlohmann::json::array();
    for (const auto& event : events) {
        nlohmann::json headers = nlohmann::json::array();
        for (const auto& [name, value] : event.headers) headers.push_back({{"name", name}, {"value", value}});
        result.push_back({{"sequence", event.sequence}, {"id", event.id}, {"revision", event.revision},
            {"from", event.envelope.from ? event.envelope.from->to_string() : ""},
            {"published_at", std::chrono::duration_cast<std::chrono::milliseconds>(event.published_at.time_since_epoch()).count()},
            {"payload", event.payload.bytes()}, {"size", event.payload.size()},
            {"encoding", to_string(event.encoding)}, {"headers", headers},
            {"edited_at", event.edited_at ? nlohmann::json(std::chrono::duration_cast<std::chrono::milliseconds>(
                event.edited_at->time_since_epoch()).count()) : nlohmann::json()}, {"edited_by", event.edited_by}});
    }
    return {{"topic", address_.to_string()}, {"events", result}, {"earliest_sequence", earliest},
        {"next_sequence", next_sequence_.load()}};
}

void Topic::forget_event(std::uint64_t sequence) {
    claims_.erase(sequence);
    ttl_reported_.erase(sequence);
    for (auto& [id, state] : subscriptions_) {
        state->in_flight.erase(sequence);
        state->requested_redeliveries.erase(sequence);
        state->delivered_sequences.erase(sequence);
    }
}

TopicMutation Topic::trim_retention() {
    const auto configured = settings();
    if (!configured.retention && !configured.max_events) return {true, {}, {}};
    if (!configured.retention && configured.max_events && held_events() <= *configured.max_events) {
        return {true, {}, {}};
    }
    struct Held {
        EventStore* store;
        std::uint64_t position;
        Event event;
    };
    std::vector<Held> held;
    for (const auto& level : levels_) {
        for (auto& [position, event] : level->entries()) held.push_back({level.get(), position, std::move(event)});
    }
    std::ranges::sort(held, [](const Held& left, const Held& right) {
        return left.event.sequence < right.event.sequence;
    });
    const auto excess = configured.max_events && held.size() > *configured.max_events ? held.size() - *configured.max_events : 0;
    TopicMutation result{true, {}, {}};
    bool saved_sequence = false;
    for (std::size_t index = 0; index < held.size(); ++index) {
        auto& entry = held[index];
        const bool expired = configured.retention && SystemClock::now() - entry.event.published_at >= *configured.retention;
        if (index >= excess && !expired) continue;
        if (!saved_sequence && owner() && !owner()->persist_sequence(*this)) {
            return {false, "cannot persist topic sequence before retention trim", {}};
        }
        saved_sequence = true;
        if (!entry.store->replace(entry.position, std::nullopt)) {
            result.ok = false;
            result.reason = "cannot persist retention removal; earlier removals are reported and audited";
            return result;
        }
        forget_event(entry.event.sequence);
        result.changes.push_back({std::move(entry.event), std::nullopt});
    }
    return result;
}

TopicMutation Topic::update_settings(const nlohmann::json& patch) {
    for (const auto* field : {"lifecycle", "store", "ring_capacity", "inline_threshold", "priority_levels",
                              "producer_queue", "dispatch", "exposed"}) {
        if (patch.contains(field)) return {false, std::string(field) + " is immutable; recreate the topic", {}};
    }
    std::lock_guard append_lock(append_mutex_);
    std::lock_guard lock(mutex_);
    if (closing_) return {false, "topic is closed", {}};
    auto value = topic_settings_json(settings());
    value.merge_patch(patch);
    const auto updated = parse_topic_settings(value);
    if (owner() && !owner()->persist_definition(*this, &updated)) {
        return {false, "cannot persist topic settings", {}};
    }
    {
        std::lock_guard settings_lock(settings_mutex_);
        settings_ = updated;
    }
    auto result = trim_retention();
    released_.notify_all();
    return result;
}

TopicMutation Topic::update_event(std::uint64_t sequence, std::string payload,
                                  std::uint64_t expected_revision, const Address& actor, bool redeliver) {
    std::lock_guard append_lock(append_mutex_);
    std::unique_lock lock(mutex_);
    if (closing_) return {false, "topic is closed", {}};
    for (const auto& level : levels_) {
        for (auto& [position, event] : level->entries(sequence, 1)) {
            if (event.sequence != sequence) continue;
            if (event.revision != expected_revision) return {false, "stale expected_revision", {}};
            if (event.revision == std::numeric_limits<std::uint64_t>::max()) return {false, "revision exhausted", {}};
            Event updated = event;
            updated.payload = Payload::make(std::move(payload), settings().inline_threshold);
            ++updated.revision;
            updated.edited_at = SystemClock::now();
            updated.edited_by = actor.to_string();
            if (updated.memory_bytes() > event.memory_bytes() && over_limit(updated.memory_bytes() - event.memory_bytes())) {
                return {false, "updated payload exceeds memory limit", {}};
            }
            if (!level->replace(position, updated)) return {false, "cannot persist event update", {}};
            for (auto& [id, state] : subscriptions_) {
                if (auto flight = state->in_flight.find(sequence); flight != state->in_flight.end()) flight->second.event = updated;
                if (auto pending = state->requested_redeliveries.find(sequence); pending != state->requested_redeliveries.end()) pending->second = updated;
                if (!redeliver || state->closed || !state->delivered_sequences.contains(sequence)) continue;
                state->in_flight.erase(sequence);
                state->requested_redeliveries[sequence] = updated;
            }
            ++publish_generation_;
            auto waiters = take_waiters();
            lock.unlock();
            published_.notify_all();
            for (const auto& waiter : waiters) waiter->set();
            return {true, {}, {{std::move(event), std::move(updated)}}};
        }
    }
    return {false, "event is not held", {}};
}

TopicMutation Topic::delete_events(const std::vector<std::uint64_t>& sequences,
                                   std::optional<std::uint64_t> before) {
    std::lock_guard append_lock(append_mutex_);
    std::lock_guard lock(mutex_);
    if (closing_) return {false, "topic is closed", {}};
    const std::unordered_set<std::uint64_t> selected(sequences.begin(), sequences.end());
    if (owner() && !owner()->persist_sequence(*this)) return {false, "cannot persist topic sequence", {}};
    TopicMutation result{true, {}, {}};
    for (const auto& level : levels_) {
        for (auto& [position, event] : level->entries()) {
            if (!(before ? event.sequence < *before : selected.contains(event.sequence))) continue;
            if (!level->replace(position, std::nullopt)) {
                result.ok = false;
                result.reason = "cannot persist event removal; earlier removals are reported and audited";
                return result;
            }
            forget_event(event.sequence);
            result.changes.push_back({std::move(event), std::nullopt});
        }
    }
    released_.notify_all();
    return result;
}

// --- Subscription handle ------------------------------------------------------

Subscription::Subscription(std::shared_ptr<Topic> topic, std::shared_ptr<State> state, Address subscriber,
                           Address topic_address, std::uint64_t id)
    : topic_(std::move(topic)), state_(std::move(state)), subscriber_(std::move(subscriber)),
      topic_address_(std::move(topic_address)), id_(id) {}

Subscription::~Subscription() { disconnect(); }

std::optional<Delivery> Subscription::receive(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        std::vector<DeferredDeadLetter> dead_letters;
        const auto remaining = std::max(std::chrono::milliseconds(0),
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - std::chrono::steady_clock::now()));
        auto delivery = topic_->receive(*state_, remaining, dead_letters);
        for (auto& letter : dead_letters) topic_->dead_letter_from_receive(letter);
        if (delivery || dead_letters.empty()) return delivery;
    }
}

bool Subscription::ack(std::uint64_t sequence) { return topic_->ack(*state_, sequence); }

void Subscription::disconnect() {
    if (topic_ && state_) topic_->disconnect(*state_);
}

bool Subscription::closed() const { return topic_->closed(*state_); }
std::string Subscription::close_reason() const { return topic_->close_reason(*state_); }
std::uint64_t Subscription::missed() const { return topic_->missed(*state_); }

}  // namespace zetta::pubsub
