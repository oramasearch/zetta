#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zetta/observability/trace.hpp"
#include "zetta/pubsub/address.hpp"
#include "zetta/pubsub/authorizer.hpp"
#include "zetta/pubsub/delivery.hpp"
#include "zetta/pubsub/dispatcher.hpp"
#include "zetta/pubsub/event.hpp"
#include "zetta/pubsub/log.hpp"
#include "zetta/pubsub/resources.hpp"
#include "zetta/pubsub/store.hpp"
#include "zetta/runtime.hpp"

// zetta's in-memory pub/sub (epic #1024; leaves #1027–#1032). The design and
// every decision it rests on: docs/plans/pubsub/. What is built:
// docs/pubsub/README.md.
//
// PubSub is the facade: topics are runtime objects; every addressable entity
// has an inbox; events carry an email-style envelope and are routed to topics,
// inboxes and drains; delivery options layer message > subscription > topic
// and are bounded by an injected Authorizer; limits are detected from the
// environment; overflow spills to disk; the shutdown flush and start-up reload
// are best effort and logged. One node only (stage 2 designs the cluster).
namespace zetta::pubsub {

class PubSub;
class Topic;

// --- Settings ----------------------------------------------------------------

enum class OverflowPolicy : std::uint8_t {
    Unbounded,      // no limit
    SkipAhead,      // drop the oldest; readers behind it get a gap notice
    Disconnect,     // disconnect the slowest subscriber
    HoldPublisher,  // publishers wait (up to the hold limit), then fail
    RejectNew,      // the publish fails
    SpillToDisk,    // write the oldest ring to disk, then hold, then fail
};

std::string_view to_string(OverflowPolicy policy);

enum class TopicLifecycle : std::uint8_t {
    Default,    // memory first; flushed on shutdown, spilled on overflow, reloaded (D17)
    Ephemeral,  // never touches disk; events discarded on shutdown
};

enum class StoreKind : std::uint8_t { RingStack, Queue };

// Every setting follows the standing rule: configurable, governed, with a
// sane or detected default.
struct TopicSettings {
    DeliveryOptions delivery;  // the topic layer
    TopicLifecycle lifecycle = TopicLifecycle::Default;
    StoreKind store = StoreKind::RingStack;
    std::size_t ring_capacity = 4096;
    std::size_t inline_threshold = kDefaultInlineThreshold;
    std::size_t priority_levels = 1;  // > 1: priority ordering with this many levels
    std::chrono::milliseconds priority_aging{5'000};  // a waiting event jumps levels after this
    std::optional<std::uint64_t> memory_cap_bytes;    // default: the node's pub/sub memory
    std::optional<std::uint64_t> disk_cap_bytes;      // default: the node's spill disk
    std::optional<OverflowPolicy> overflow;           // default: by guarantee (D10)
    OverBoundPolicy over_bound = OverBoundPolicy::Clamp;  // D8
    bool producer_queue = false;                      // D23: appends go through the dispatcher
    std::optional<DispatchMode> dispatch;             // explicit per-topic dispatch override
    bool exposed = false;                             // D22: MCP/REST exposure
    std::size_t topics_chain_max = kDefaultTopicsChainDepth;
    std::optional<std::chrono::milliseconds> default_ttl;
    std::optional<std::chrono::milliseconds> retention;
    std::optional<std::uint64_t> max_events;
    DeliveryBounds bounds;
};

nlohmann::json topic_settings_json(const TopicSettings& settings);
TopicSettings parse_topic_settings(const nlohmann::json& value);

struct EventChange {
    Event before;
    std::optional<Event> after;
};

struct TopicMutation {
    bool ok = false;
    std::string reason;
    std::vector<EventChange> changes;
};

enum class StartPosition : std::uint8_t { Next, Oldest, Sequence };

struct SubscribeOptions {
    DeliveryOptions delivery;              // the subscription layer
    std::optional<std::string> group;      // members share one position: each event goes to one
    StartPosition start = StartPosition::Next;  // D19
    std::uint64_t start_sequence = 0;
    // How long the subscription outlives a disconnect (D31). Default: 24 h for
    // agent: subscribers, none for everything else.
    std::optional<std::chrono::milliseconds> retention;
    std::optional<std::uint64_t> max_lag_bytes;  // beyond it, lag_policy applies
    OverflowPolicy lag_policy = OverflowPolicy::SkipAhead;
};

inline constexpr std::chrono::hours kAgentInboxRetention{24};

// --- Subscriptions -----------------------------------------------------------

struct Delivery {
    Event event;
    std::uint64_t sequence = 0;
    bool redelivery = false;
    std::uint64_t missed = 0;  // gap notice: events skipped before this one
};

// A handle to one subscription. Destroying it disconnects (the subscription
// is then kept for its retention, or removed at once).
class Subscription {
public:
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    const Address& subscriber() const { return subscriber_; }
    const Address& topic() const { return topic_address_; }
    std::uint64_t id() const { return id_; }

    // Wait up to `timeout` for the next event. nullopt on timeout or when the
    // subscription was closed (see closed()).
    std::optional<Delivery> receive(std::chrono::milliseconds timeout);
    // Acknowledge an at-least-once / exactly-once delivery.
    bool ack(std::uint64_t sequence);
    // Disconnect now (retention decides how long it is kept).
    void disconnect();
    bool closed() const;
    std::string close_reason() const;
    std::uint64_t missed() const;

private:
    friend class Topic;
    friend class PubSub;
    struct State;
    Subscription(std::shared_ptr<Topic> topic, std::shared_ptr<State> state, Address subscriber,
                 Address topic_address, std::uint64_t id);

    std::shared_ptr<Topic> topic_;
    std::shared_ptr<State> state_;
    Address subscriber_;
    Address topic_address_;
    std::uint64_t id_;
};

// --- Drains ------------------------------------------------------------------

struct DrainResult {
    bool ok = true;
    std::string error;                     // when !ok
    std::optional<std::string> result;     // the result to reply with, if any
    Encoding result_encoding = Encoding::Json;
};

// What happens to an event delivered to an address of one kind (D35):
// table: inserts a row, function: calls, model: infers, service: delivers.
class InboxDrain {
public:
    virtual ~InboxDrain() = default;
    virtual DrainResult drain(const Address& target, const Event& event) = 0;
    // Where a result goes by default for this kind (D36): e.g. a query replies,
    // an insert does not.
    virtual ReplyPolicy default_reply() const { return ReplyPolicy::None; }
};

// A drain from a callable (tests, engine wiring).
class CallbackDrain final : public InboxDrain {
public:
    using Callback = std::function<DrainResult(const Address&, const Event&)>;
    CallbackDrain(Callback callback, ReplyPolicy reply = ReplyPolicy::None)
        : callback_(std::move(callback)), reply_(reply) {}
    DrainResult drain(const Address& target, const Event& event) override {
        return callback_(target, event);
    }
    ReplyPolicy default_reply() const override { return reply_; }

private:
    Callback callback_;
    ReplyPolicy reply_;
};

// --- Publishing --------------------------------------------------------------

// Who is publishing: the principal the event is for, and the stack of agents
// acting for it, oldest first. The node fills this from authenticated sessions;
// the envelope's from/sender are stamped from it, never taken from the request.
struct Identity {
    Address principal;
    std::vector<Address> acting_chain;
};

struct PublishRequest {
    std::vector<Address> to, cc, bcc, reply_to;
    std::string correlation_id;
    std::vector<Address> topics_chain;  // when forwarding
    Encoding encoding = Encoding::Blob;
    std::string mime_type;
    std::string payload;
    DeliveryOptions delivery;           // the message layer
    std::optional<std::chrono::milliseconds> ttl;
    std::string id;                     // optional: a caller-chosen id (exactly-once retries)
};

struct PublishResult {
    bool accepted = false;
    std::string error;
    std::string event_id;
    std::vector<Address> delivered_to;
    std::vector<std::pair<Address, std::string>> refused;  // target, reason
    std::vector<std::string> clamped;                       // fields lowered by governance
};

// --- Observability -----------------------------------------------------------

struct TopicStats {
    std::string address;
    TopicLifecycle lifecycle = TopicLifecycle::Default;
    bool exposed = false;
    std::size_t subscribers = 0;
    std::uint64_t held_events = 0;
    std::uint64_t memory_bytes = 0;
    std::uint64_t disk_bytes = 0;
    std::optional<std::chrono::system_clock::time_point> spilling_since;
    std::string slowest_subscriber;
    std::uint64_t slowest_lag_events = 0;
    std::uint64_t next_sequence = 0;
};

// An audit record for governed pub/sub decisions (refusals, clamps,
// auto-creation, wildcard sends). The node routes it to the audit trail.
struct AuditRecord {
    std::string action;  // "publish_refused", "clamped", "topic_auto_created", ...
    Address principal;
    Address target;
    std::string detail;
};

struct PubSubConfig {
    std::filesystem::path data_directory;       // pubsub/ lives under it
    ResourceOverrides resource_overrides;
    LimitOverrides limit_overrides;
    DispatchMode dispatch_mode = DispatchMode::Pool;
    // Name prefixes for which publishing or subscribing auto-creates a missing
    // topic (D16). Empty: auto-creation off. "" alone: every name.
    std::vector<std::string> auto_create_prefixes;
    TopicSettings auto_create_settings;
    std::size_t max_wildcard_fanout = 10'000;
    std::size_t wildcard_cache_limit = 1'024;
    std::size_t max_sender_chain = kMaxSenderChainDepth;
    std::chrono::milliseconds spill_log_interval{30'000};  // D12
    std::chrono::milliseconds maintenance_interval{1'000};
    // Called while shutdown or drain_dispatch waits for dispatched work (the
    // node passes its owner-queue pump; see Dispatcher::set_idle_pump).
    std::function<void()> idle_pump;
    // Where spans go (#1034): publish, subscribe, ack, dead letters, topic
    // create/drop. Null: no spans (bare library use). The node passes its ring.
    zetta::observability::SpanRecorder* spans = nullptr;
    bool detect_resources = true;  // false: tests pass limits directly
    std::optional<PubSubLimits> limits;  // used when detect_resources is false
};

// --- The facade --------------------------------------------------------------

class PubSub {
public:
    PubSub(PubSubConfig config, std::shared_ptr<Authorizer> authorizer = nullptr,
           LogSink log = stderr_log_sink(), ResourceProbe probe = system_probe());
    ~PubSub();
    PubSub(const PubSub&) = delete;
    PubSub& operator=(const PubSub&) = delete;

    // Reload topics flushed or spilled by a previous run and start the
    // maintenance thread. Logs what was recovered and lost (D18).
    void start();
    // Stop accepting publishes and flush default topics to disk within the
    // budget (D20). Logs progress and what was lost.
    void shutdown(std::optional<std::chrono::milliseconds> budget = std::nullopt);

    // Topics.
    bool create_topic(const Identity& identity, const Address& topic, TopicSettings settings,
                      std::string* reason = nullptr);
    bool drop_topic(const Identity& identity, const Address& topic, std::string* reason = nullptr);
    std::vector<Address> list_topics() const;
    std::optional<TopicSettings> topic_settings(const Address& topic) const;
    std::vector<TopicStats> topic_stats() const;
    std::optional<nlohmann::json> get_topic(const Identity& identity, const Address& topic,
                                           std::string* reason = nullptr);
    TopicMutation update_topic(const Identity& identity, const Address& topic,
                               const nlohmann::json& settings);
    std::optional<nlohmann::json> browse_topic(const Identity& identity, const Address& topic,
                                              std::optional<std::uint64_t> from_sequence,
                                              std::size_t limit, bool backward,
                                              std::string* reason = nullptr,
                                              std::shared_ptr<runtime::async_event> waiter = {});
    TopicMutation update_event(const Identity& identity, const Address& topic, std::uint64_t sequence,
                               std::string payload, std::uint64_t expected_revision, bool redeliver);
    TopicMutation delete_events(const Identity& identity, const Address& topic,
                                const std::vector<std::uint64_t>& sequences,
                                std::optional<std::uint64_t> before_sequence);

    // Subscribe `subscriber` to a topic (auto-creating it when configured).
    std::unique_ptr<Subscription> subscribe(const Identity& identity, const Address& subscriber,
                                            const Address& topic, SubscribeOptions options = {},
                                            std::string* reason = nullptr);
    // Open the inbox of an addressable entity (e.g. agent:405) and subscribe
    // its owner to it. Events sent to that address wait here.
    std::unique_ptr<Subscription> open_inbox(const Identity& identity, const Address& owner,
                                             SubscribeOptions options = {},
                                             std::string* reason = nullptr);

    PublishResult publish(const Identity& identity, PublishRequest request);

    // Read a topic's held events with sequence >= `from_sequence`, oldest first,
    // at most `max`, waiting up to `wait` when there are none yet. Stateless (no
    // subscription): the long-poll read behind the `receive_events` capability.
    // An optional waiter is registered before the read and signaled by publish or
    // close. Governed by pubsub:subscribe. nullopt with `reason` when refused or
    // missing.
    std::optional<std::vector<Event>> read_events(const Identity& identity, const Address& topic,
                                                  std::uint64_t from_sequence, std::size_t max,
                                                  std::chrono::milliseconds wait,
                                                  std::string* reason = nullptr,
                                                  std::shared_ptr<runtime::async_event> waiter = {});

    // Drains by address kind (D35).
    void register_drain(const std::string& kind, std::shared_ptr<InboxDrain> drain);

    // The limits in effect (detected or configured).
    const PubSubLimits& limits() const { return limits_; }
    MimeRegistry& mime_types() { return mime_types_; }
    DispatchMode dispatch_mode() const { return dispatcher_->effective_mode(); }
    std::size_t dispatch_threads() const {
        return dispatcher_->pool_thread_count();
    }
    std::size_t dispatch_keys() const {
        return dispatcher_->key_count();
    }

    // Audit injection point.
    void set_audit_sink(std::function<void(const AuditRecord&)> sink);

    // Run one maintenance pass now (retention, spill logging). The maintenance
    // thread calls this every maintenance_interval.
    void maintain();
    // Wait until queued dispatch work has run (tests).
    void drain_dispatch();

    static const Address& dead_letter_address();
    static Address error_topic_for(const Address& recipient);

private:
    friend class Topic;
    friend class Subscription;

    std::shared_ptr<Topic> find_topic(const Address& topic) const;
    std::shared_ptr<Topic> find_or_auto_create(const Identity& identity, const Address& topic,
                                               std::string* reason);
    std::shared_ptr<Topic> ensure_system_topic(const Address& topic, TopicSettings settings);
    std::shared_ptr<Topic> inbox_topic_for(const Address& owner, bool create);
    std::vector<Address> resolve_wildcard(const Address& pattern);
    void entities_changed();
    // `trusted`: engine-generated events (drain replies) skip the authorizer.
    PublishResult publish_as(const Identity& identity, PublishRequest request, bool trusted);
    void deliver_to(const Identity& identity, const Event& event, const Address& target,
                    PublishResult& result, bool trusted);
    void run_drain(std::shared_ptr<InboxDrain> drain, Address target, Event event);
    void send_reply(const Address& from, const Event& request, ReplyPolicy policy,
                    std::string payload, Encoding encoding);
    void dead_letter(const Event& event, std::string reason);
    void report_error(const Address& recipient, const Event& event, std::string error);
    void audit(std::string action, const Address& principal, const Address& target,
               std::string detail);
    std::uint64_t total_memory_bytes() const;
    std::uint64_t total_disk_bytes() const;
    std::filesystem::path topics_directory() const;
    std::filesystem::path spill_directory_for(const Address& topic) const;
    bool persist_definition(const Topic& topic, const TopicSettings* replacement = nullptr) const;
    bool persist_sequence(const Topic& topic) const;
    std::shared_ptr<Topic> crud_topic(const Identity& identity, const Address& address,
                                     PubSubOperation operation, std::string* reason) const;
    void audit_changes(const Identity& identity, const Address& topic, const TopicMutation& mutation);
    void log(LogLevel level, std::string message) const;
    zetta::observability::SpanRecorder* spans() const { return config_.spans; }

    PubSubConfig config_;
    std::shared_ptr<Authorizer> authorizer_;
    LogSink log_;
    ResourceProbe probe_;
    PubSubLimits limits_;
    MimeRegistry mime_types_;
    std::unique_ptr<Dispatcher> dispatcher_;

    mutable std::mutex mutex_;  // topics_, inboxes_, drains_, wildcard cache
    std::map<Address, std::shared_ptr<Topic>> topics_;
    std::map<Address, std::shared_ptr<Topic>> inboxes_;  // owner address -> inbox topic
    std::map<std::string, std::shared_ptr<InboxDrain>> drains_;
    std::map<std::string, std::vector<Address>> wildcard_cache_;
    std::deque<std::string> wildcard_cache_order_;
    std::function<void(const AuditRecord&)> audit_sink_;

    std::atomic<bool> accepting_{true};
    std::atomic<bool> started_{false};
    std::jthread maintenance_;
    std::mutex maintenance_mutex_;
    std::condition_variable_any maintenance_wake_;  // stop-token aware: no lost wakeup on shutdown
};

}  // namespace zetta::pubsub
