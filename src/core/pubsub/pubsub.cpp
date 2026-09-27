#include "zetta/pubsub/pubsub.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "topic.hpp"

namespace zetta::pubsub {
namespace {

using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;

const Address kSystemPrincipal{"service", "pubsub"};

std::atomic<std::uint64_t> next_subscription_id{1};

// A directory name for an address: hex of its text, so any name is safe.
std::string encode_name(const std::string& text) {
    static const char* digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(text.size() * 2);
    for (const unsigned char character : text) {
        encoded.push_back(digits[character >> 4]);
        encoded.push_back(digits[character & 0x0F]);
    }
    return encoded;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file_atomically(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

std::string_view lifecycle_name(TopicLifecycle lifecycle) {
    return lifecycle == TopicLifecycle::Ephemeral ? "ephemeral" : "default";
}

std::optional<OverflowPolicy> overflow_from_string(std::string_view text) {
    for (auto policy : {OverflowPolicy::Unbounded, OverflowPolicy::SkipAhead, OverflowPolicy::Disconnect,
                        OverflowPolicy::HoldPublisher, OverflowPolicy::RejectNew, OverflowPolicy::SpillToDisk}) {
        if (to_string(policy) == text) return policy;
    }
    return std::nullopt;
}

nlohmann::json settings_to_json(const TopicSettings& settings) {
    nlohmann::json json = {
        {"lifecycle", lifecycle_name(settings.lifecycle)},
        {"store", settings.store == StoreKind::Queue ? "queue" : "ring_stack"},
        {"ring_capacity", settings.ring_capacity},
        {"inline_threshold", settings.inline_threshold},
        {"priority_levels", settings.priority_levels},
        {"priority_aging_ms", settings.priority_aging.count()},
        {"over_bound", settings.over_bound == OverBoundPolicy::Reject ? "reject" : "clamp"},
        {"producer_queue", settings.producer_queue},
        {"exposed", settings.exposed},
        {"topics_chain_max", settings.topics_chain_max},
    };
    if (settings.memory_cap_bytes) json["memory_cap_bytes"] = *settings.memory_cap_bytes;
    if (settings.disk_cap_bytes) json["disk_cap_bytes"] = *settings.disk_cap_bytes;
    if (settings.overflow) json["overflow"] = to_string(*settings.overflow);
    if (settings.default_ttl) json["default_ttl_ms"] = settings.default_ttl->count();
    if (settings.dispatch) json["dispatch"] = settings.dispatch == DispatchMode::Inline ? "inline" : "per_key";
    const auto& delivery = settings.delivery;
    nlohmann::json delivery_json = nlohmann::json::object();
    if (delivery.recipients) {
        delivery_json["recipients"] = to_string(delivery.recipients->mode);
        delivery_json["count"] = delivery.recipients->count;
    }
    if (delivery.ordering) delivery_json["ordering"] = to_string(*delivery.ordering);
    if (delivery.priority) delivery_json["priority"] = *delivery.priority;
    if (delivery.guarantee) delivery_json["guarantee"] = to_string(*delivery.guarantee);
    if (delivery.ack_deadline) delivery_json["ack_deadline_ms"] = delivery.ack_deadline->count();
    if (delivery.reply) delivery_json["reply"] = to_string(*delivery.reply);
    json["delivery"] = delivery_json;
    return json;
}

// Non-throwing: a malformed field keeps its default.
TopicSettings settings_from_json(const nlohmann::json& json) {
    TopicSettings settings;
    if (!json.is_object()) return settings;
    auto text = [&](const char* key) -> std::optional<std::string> {
        if (json.contains(key) && json[key].is_string()) return json[key].get<std::string>();
        return std::nullopt;
    };
    auto number = [&](const char* key) -> std::optional<long long> {
        if (json.contains(key) && json[key].is_number_integer()) return json[key].get<long long>();
        return std::nullopt;
    };
    auto boolean = [&](const char* key) -> std::optional<bool> {
        if (json.contains(key) && json[key].is_boolean()) return json[key].get<bool>();
        return std::nullopt;
    };
    if (auto value = text("lifecycle")) {
        settings.lifecycle = *value == "ephemeral" ? TopicLifecycle::Ephemeral : TopicLifecycle::Default;
    }
    if (auto value = text("store")) settings.store = *value == "queue" ? StoreKind::Queue : StoreKind::RingStack;
    if (auto value = number("ring_capacity"); value && *value > 0) settings.ring_capacity = *value;
    if (auto value = number("inline_threshold"); value && *value >= 0) settings.inline_threshold = *value;
    if (auto value = number("priority_levels"); value && *value > 0) settings.priority_levels = *value;
    if (auto value = number("priority_aging_ms"); value && *value >= 0) {
        settings.priority_aging = std::chrono::milliseconds(*value);
    }
    if (auto value = text("over_bound")) {
        settings.over_bound = *value == "reject" ? OverBoundPolicy::Reject : OverBoundPolicy::Clamp;
    }
    if (auto value = boolean("producer_queue")) settings.producer_queue = *value;
    if (auto value = boolean("exposed")) settings.exposed = *value;
    if (auto value = number("topics_chain_max"); value && *value > 0) settings.topics_chain_max = *value;
    if (auto value = number("memory_cap_bytes"); value && *value >= 0) settings.memory_cap_bytes = *value;
    if (auto value = number("disk_cap_bytes"); value && *value >= 0) settings.disk_cap_bytes = *value;
    if (auto value = text("overflow")) settings.overflow = overflow_from_string(*value);
    if (auto value = number("default_ttl_ms"); value && *value > 0) {
        settings.default_ttl = std::chrono::milliseconds(*value);
    }
    if (auto value = text("dispatch")) {
        settings.dispatch = *value == "inline" ? DispatchMode::Inline : DispatchMode::PerKey;
    }
    if (json.contains("delivery") && json["delivery"].is_object()) {
        const auto& delivery = json["delivery"];
        auto& options = settings.delivery;
        if (delivery.contains("recipients") && delivery["recipients"].is_string()) {
            const auto mode = delivery["recipients"].get<std::string>();
            Recipients recipients;
            recipients.mode = mode == "one" ? Recipients::Mode::One
                              : mode == "count" ? Recipients::Mode::Count
                                                : Recipients::Mode::All;
            if (delivery.contains("count") && delivery["count"].is_number_unsigned()) {
                recipients.count = delivery["count"].get<std::uint32_t>();
            }
            options.recipients = recipients;
        }
        if (delivery.contains("ordering") && delivery["ordering"].is_string()) {
            options.ordering = delivery["ordering"].get<std::string>() == "priority" ? Ordering::Priority
                                                                                     : Ordering::Fifo;
        }
        if (delivery.contains("priority") && delivery["priority"].is_number_unsigned()) {
            options.priority = delivery["priority"].get<std::uint8_t>();
        }
        if (delivery.contains("guarantee") && delivery["guarantee"].is_string()) {
            options.guarantee = guarantee_from_string(delivery["guarantee"].get<std::string>());
        }
        if (delivery.contains("ack_deadline_ms") && delivery["ack_deadline_ms"].is_number_integer()) {
            options.ack_deadline = std::chrono::milliseconds(delivery["ack_deadline_ms"].get<long long>());
        }
        if (delivery.contains("reply") && delivery["reply"].is_string()) {
            const auto reply = delivery["reply"].get<std::string>();
            options.reply = reply == "reply_to"    ? ReplyPolicy::ReplyTo
                            : reply == "reply_all" ? ReplyPolicy::ReplyAll
                                                   : ReplyPolicy::None;
        }
    }
    return settings;
}

bool matches_prefix(const std::vector<std::string>& prefixes, const std::string& name) {
    return std::ranges::any_of(prefixes, [&](const std::string& prefix) { return name.starts_with(prefix); });
}

}  // namespace

std::string_view to_string(OverflowPolicy policy) {
    switch (policy) {
        case OverflowPolicy::Unbounded: return "unbounded";
        case OverflowPolicy::SkipAhead: return "skip_ahead";
        case OverflowPolicy::Disconnect: return "disconnect";
        case OverflowPolicy::HoldPublisher: return "hold_publisher";
        case OverflowPolicy::RejectNew: return "reject_new";
        case OverflowPolicy::SpillToDisk: return "spill_to_disk";
    }
    return "unbounded";
}

std::string_view to_string(PubSubOperation operation) {
    switch (operation) {
        case PubSubOperation::Publish: return "pubsub:publish";
        case PubSubOperation::Subscribe: return "pubsub:subscribe";
        case PubSubOperation::Create: return "pubsub:create";
        case PubSubOperation::Manage: return "pubsub:manage";
        case PubSubOperation::SendWildcard: return "pubsub:send_wildcard";
    }
    return "pubsub:publish";
}

const Address& PubSub::dead_letter_address() {
    static const Address address{"topic", "dead-letter"};
    return address;
}

Address PubSub::error_topic_for(const Address& recipient) {
    return Address{"topic", "errors." + recipient.kind + "." + recipient.name};
}

// --- Construction, start, shutdown ------------------------------------------

PubSub::PubSub(PubSubConfig config, std::shared_ptr<Authorizer> authorizer, LogSink log, ResourceProbe probe)
    : config_(std::move(config)),
      authorizer_(authorizer ? std::move(authorizer) : std::make_shared<AllowAllAuthorizer>()),
      log_(std::move(log)),
      probe_(std::move(probe)) {
    if (config_.detect_resources) {
        const auto budget = detect_resources(probe_, config_.data_directory, config_.resource_overrides);
        limits_ = derive_limits(budget, config_.limit_overrides, log_);
    } else {
        limits_ = config_.limits.value_or(PubSubLimits{.memory_bytes = std::uint64_t{1} << 30,
                                                       .spill_disk_bytes = std::uint64_t{1} << 30});
    }
    dispatcher_ = std::make_unique<Dispatcher>(
        DispatcherOptions{.mode = config_.dispatch_mode,
                          .threshold = limits_.dispatch_threshold,
                          .pool_threads = limits_.pool_threads},
        log_);
}

PubSub::~PubSub() {
    if (started_.load() || accepting_.load()) shutdown(std::chrono::milliseconds(0));
    std::lock_guard lock(mutex_);
    for (auto& [address, topic] : topics_) topic->detach();
    for (auto& [address, topic] : inboxes_) topic->detach();
}

void PubSub::log(LogLevel level, std::string message) const {
    if (log_) log_(level, message);
}

std::filesystem::path PubSub::topics_directory() const {
    return config_.data_directory / "pubsub" / "topics";
}

std::filesystem::path PubSub::spill_directory_for(const Address& topic) const {
    return config_.data_directory / "pubsub" / "spill" / encode_name(topic.to_string());
}

void PubSub::persist_definition(const Topic& topic) const {
    if (config_.data_directory.empty()) return;
    nlohmann::json definition = {{"address", topic.address().to_string()},
                                 {"settings", settings_to_json(topic.settings())}};
    if (topic.inbox_owner()) definition["inbox_owner"] = topic.inbox_owner()->to_string();
    write_file_atomically(topics_directory() / encode_name(topic.address().to_string()) / "definition.json",
                          definition.dump(2));
}

void PubSub::start() {
    if (started_.exchange(true)) return;
    accepting_.store(true);
    std::uint64_t recovered_events = 0;
    std::size_t recovered_topics = 0;
    std::uint64_t torn = 0;
    std::error_code error;
    if (!config_.data_directory.empty() && std::filesystem::exists(topics_directory(), error)) {
        for (const auto& entry : std::filesystem::directory_iterator(topics_directory(), error)) {
            if (!entry.is_directory()) continue;
            const auto text = read_file(entry.path() / "definition.json");
            if (!text) continue;
            const auto definition = nlohmann::json::parse(*text, nullptr, false);
            if (!definition.is_object() || !definition.contains("address") || !definition["address"].is_string()) {
                log(LogLevel::Warn, "skipped an unreadable topic definition in " + entry.path().string());
                continue;
            }
            auto address = parse_address(definition["address"].get<std::string>());
            if (!address) continue;
            auto settings = settings_from_json(definition.value("settings", nlohmann::json::object()));
            std::optional<Address> inbox_owner;
            if (definition.contains("inbox_owner") && definition["inbox_owner"].is_string()) {
                inbox_owner = parse_address(definition["inbox_owner"].get<std::string>());
            }
            auto topic = std::make_shared<Topic>(this, *address, settings, inbox_owner);

            // Events: the shutdown flush (level-N.frames), then any spill files
            // left by a crash (best effort, D18).
            std::vector<std::vector<Event>> levels(std::max<std::size_t>(1, settings.priority_levels));
            std::uint64_t next_sequence = 1;
            if (auto state_text = read_file(entry.path() / "state.json"); state_text) {
                const auto state = nlohmann::json::parse(*state_text, nullptr, false);
                if (state.is_object() && state.contains("next_sequence") && state["next_sequence"].is_number_unsigned()) {
                    next_sequence = state["next_sequence"].get<std::uint64_t>();
                }
            }
            auto load_frames = [&](const std::filesystem::path& path, std::vector<Event>& into) {
                const auto bytes = read_file(path);
                if (!bytes) return;
                std::string_view rest(*bytes);
                while (!rest.empty()) {
                    auto decoded = decode_event_frame(rest, settings.inline_threshold);
                    if (decoded.status != FrameStatus::Ok) {
                        ++torn;
                        break;
                    }
                    into.push_back(std::move(decoded.event));
                    rest.remove_prefix(decoded.consumed);
                }
            };
            if (settings.lifecycle != TopicLifecycle::Ephemeral) {
                for (std::size_t level = 0; level < levels.size(); ++level) {
                    load_frames(entry.path() / ("level-" + std::to_string(level) + ".frames"), levels[level]);
                    const auto spill_level = spill_directory_for(*address) / ("level-" + std::to_string(level));
                    if (std::filesystem::exists(spill_level, error)) {
                        std::vector<std::filesystem::path> files;
                        for (const auto& file : std::filesystem::directory_iterator(spill_level, error)) {
                            files.push_back(file.path());
                        }
                        std::ranges::sort(files, {}, [](const std::filesystem::path& file) {
                            const auto stem = file.stem().string();  // ring-<first position>
                            return std::stoull(stem.substr(stem.find('-') + 1));
                        });
                        std::vector<Event> spilled;
                        for (const auto& file : files) load_frames(file, spilled);
                        // Events already in the flush file win; spill files only add older ones.
                        std::set<std::string> seen;
                        for (const auto& event : levels[level]) seen.insert(event.id);
                        std::vector<Event> merged;
                        for (auto& event : spilled) {
                            if (!seen.contains(event.id)) merged.push_back(std::move(event));
                        }
                        for (auto& event : levels[level]) merged.push_back(std::move(event));
                        std::ranges::sort(merged, {}, &Event::sequence);
                        levels[level] = std::move(merged);
                    }
                }
            }
            std::filesystem::remove_all(spill_directory_for(*address), error);
            for (std::size_t level = 0; level < levels.size(); ++level) {
                std::filesystem::remove(entry.path() / ("level-" + std::to_string(level) + ".frames"), error);
            }
            std::uint64_t count = 0;
            for (const auto& level : levels) count += level.size();
            topic->restore(std::move(levels), next_sequence);
            if (count > 0) {
                log(LogLevel::Info, address->to_string() + " recovered " + std::to_string(count) +
                                        " events (next sequence " + std::to_string(topic->next_sequence()) + ")");
            }
            recovered_events += count;
            ++recovered_topics;
            std::lock_guard lock(mutex_);
            if (inbox_owner) {
                inboxes_[*inbox_owner] = topic;
            } else {
                topics_[*address] = topic;
            }
            if (settings.dispatch) dispatcher_->pin(address->to_string(), settings.dispatch);
        }
    }
    if (recovered_topics > 0 || torn > 0) {
        log(torn > 0 ? LogLevel::Warn : LogLevel::Info,
            "start-up: reloaded " + std::to_string(recovered_topics) + " topics, " +
                std::to_string(recovered_events) + " events; " + std::to_string(torn) +
                " torn or corrupt tails dropped");
    }
    entities_changed();
    maintenance_ = std::jthread([this](std::stop_token stop) {
        std::unique_lock lock(maintenance_mutex_);
        while (!stop.stop_requested()) {
            maintenance_wake_.wait_for(lock, config_.maintenance_interval);
            if (stop.stop_requested()) break;
            lock.unlock();
            maintain();
            lock.lock();
        }
    });
}

void PubSub::shutdown(std::optional<std::chrono::milliseconds> budget) {
    if (!accepting_.exchange(false) && !started_.load()) return;
    const auto limit = budget.value_or(limits_.shutdown_budget);
    const auto started = SteadyClock::now();
    const auto deadline = started + limit;
    if (maintenance_.joinable()) {
        maintenance_.request_stop();
        maintenance_wake_.notify_all();
        maintenance_.join();
    }
    dispatcher_->stop();

    std::vector<std::shared_ptr<Topic>> all;
    {
        std::lock_guard lock(mutex_);
        for (auto& [address, topic] : topics_) all.push_back(topic);
        for (auto& [address, topic] : inboxes_) all.push_back(topic);
    }
    std::uint64_t held = 0;
    std::size_t to_flush = 0;
    for (auto& topic : all) {
        topic->close_all("shutdown");
        if (topic->settings().lifecycle != TopicLifecycle::Ephemeral) {
            held += topic->memory_bytes() + topic->disk_bytes();
            ++to_flush;
        }
    }
    if (config_.data_directory.empty() || to_flush == 0) {
        started_.store(false);
        return;
    }
    log(LogLevel::Info, "shutdown: flushing " + std::to_string(to_flush) + " topics, " + format_bytes(held) +
                            " held; budget " + std::to_string(limit.count() / 1000) + "s");
    std::size_t flushed = 0;
    for (auto& topic : all) {
        if (topic->settings().lifecycle == TopicLifecycle::Ephemeral) continue;
        const auto directory = topics_directory() / encode_name(topic->address().to_string());
        if (SteadyClock::now() >= deadline) {
            if (const auto held_events = topic->held_events(); held_events > 0) {
                log(LogLevel::Warn, "shutdown: budget spent; " + topic->address().to_string() + " lost " +
                                        std::to_string(held_events) + " events");
            }
            continue;
        }
        persist_definition(*topic);
        const auto levels = topic->snapshot();
        bool complete = true;
        for (std::size_t level = 0; level < levels.size(); ++level) {
            std::string frames;
            for (const auto& event : levels[level]) frames += encode_event_frame(event);
            if (!write_file_atomically(directory / ("level-" + std::to_string(level) + ".frames"), frames)) {
                complete = false;
            }
        }
        nlohmann::json state = {{"next_sequence", topic->next_sequence()}};
        write_file_atomically(directory / "state.json", state.dump());
        std::error_code error;
        if (complete) {
            std::filesystem::remove_all(spill_directory_for(topic->address()), error);
            ++flushed;
        } else {
            log(LogLevel::Error, "shutdown: could not write " + topic->address().to_string() + " to " +
                                     directory.string());
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - started);
    log(flushed == to_flush ? LogLevel::Info : LogLevel::Warn,
        "shutdown: flushed " + std::to_string(flushed) + "/" + std::to_string(to_flush) + " topics in " +
            std::to_string(elapsed.count() / 1000) + "." + std::to_string((elapsed.count() % 1000) / 100) + "s");
    started_.store(false);
}

// --- Topics ------------------------------------------------------------------

std::shared_ptr<Topic> PubSub::find_topic(const Address& topic) const {
    std::lock_guard lock(mutex_);
    auto found = topics_.find(topic);
    return found == topics_.end() ? nullptr : found->second;
}

bool PubSub::create_topic(const Identity& identity, const Address& address, TopicSettings settings,
                          std::string* reason) {
    auto fail = [&](std::string text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (address.kind != "topic" || address.is_wildcard()) {
        return fail("a topic address is topic:<name> with no wildcard, not '" + address.to_string() + "'");
    }
    if (!authorizer_->authorize(identity.principal, PubSubOperation::Create, address)) {
        audit("create_refused", identity.principal, address, "no pubsub:create");
        return fail(identity.principal.to_string() + " may not create " + address.to_string());
    }
    std::shared_ptr<Topic> topic;
    {
        std::lock_guard lock(mutex_);
        if (topics_.contains(address)) return fail(address.to_string() + " already exists");
        topic = std::make_shared<Topic>(this, address, settings);
        topics_[address] = topic;
    }
    if (settings.dispatch) dispatcher_->pin(address.to_string(), settings.dispatch);
    persist_definition(*topic);
    entities_changed();
    return true;
}

bool PubSub::drop_topic(const Identity& identity, const Address& address, std::string* reason) {
    if (!authorizer_->authorize(identity.principal, PubSubOperation::Manage, address)) {
        if (reason != nullptr) *reason = identity.principal.to_string() + " may not drop " + address.to_string();
        audit("drop_refused", identity.principal, address, "no pubsub:manage");
        return false;
    }
    std::shared_ptr<Topic> topic;
    {
        std::lock_guard lock(mutex_);
        auto found = topics_.find(address);
        if (found == topics_.end()) {
            if (reason != nullptr) *reason = address.to_string() + " does not exist";
            return false;
        }
        topic = found->second;
        topics_.erase(found);
    }
    topic->close_all("topic dropped");
    topic->detach();
    dispatcher_->remove_key(address.to_string());
    std::error_code error;
    std::filesystem::remove_all(topics_directory() / encode_name(address.to_string()), error);
    std::filesystem::remove_all(spill_directory_for(address), error);
    entities_changed();
    return true;
}

std::vector<Address> PubSub::list_topics() const {
    std::lock_guard lock(mutex_);
    std::vector<Address> addresses;
    for (const auto& [address, topic] : topics_) addresses.push_back(address);
    return addresses;
}

std::optional<TopicSettings> PubSub::topic_settings(const Address& address) const {
    auto topic = find_topic(address);
    if (!topic) return std::nullopt;
    return topic->settings();
}

std::vector<TopicStats> PubSub::topic_stats() const {
    std::vector<std::shared_ptr<Topic>> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [address, topic] : topics_) all.push_back(topic);
        for (const auto& [address, topic] : inboxes_) all.push_back(topic);
    }
    std::vector<TopicStats> stats;
    for (const auto& topic : all) stats.push_back(topic->stats());
    return stats;
}

std::shared_ptr<Topic> PubSub::find_or_auto_create(const Identity& identity, const Address& address,
                                                   std::string* reason) {
    if (auto topic = find_topic(address); topic) return topic;
    if (!matches_prefix(config_.auto_create_prefixes, address.name)) {
        if (reason != nullptr) *reason = address.to_string() + " does not exist";
        return nullptr;
    }
    std::string create_reason;
    if (!create_topic(identity, address, config_.auto_create_settings, &create_reason)) {
        if (auto topic = find_topic(address); topic) return topic;  // lost a race: fine
        if (reason != nullptr) *reason = create_reason;
        return nullptr;
    }
    log(LogLevel::Info, "auto-created " + address.to_string() + " for " + identity.principal.to_string() +
                            " (lifecycle " + std::string(lifecycle_name(config_.auto_create_settings.lifecycle)) + ")");
    audit("topic_auto_created", identity.principal, address, "auto-create prefix matched");
    return find_topic(address);
}

std::shared_ptr<Topic> PubSub::ensure_system_topic(const Address& address, TopicSettings settings) {
    std::shared_ptr<Topic> topic;
    bool created = false;
    {
        std::lock_guard lock(mutex_);
        auto& slot = topics_[address];
        if (!slot) {
            slot = std::make_shared<Topic>(this, address, std::move(settings));
            created = true;
        }
        topic = slot;
    }
    if (created) {
        persist_definition(*topic);
        entities_changed();
    }
    return topic;
}

std::shared_ptr<Topic> PubSub::inbox_topic_for(const Address& owner, bool create) {
    std::shared_ptr<Topic> topic;
    bool created = false;
    {
        std::lock_guard lock(mutex_);
        auto found = inboxes_.find(owner);
        if (found != inboxes_.end()) return found->second;
        if (!create) return nullptr;
        TopicSettings settings;
        settings.delivery.guarantee = Guarantee::AtLeastOnce;
        topic = std::make_shared<Topic>(this, Address{"inbox", owner.kind + "." + owner.name}, settings, owner);
        inboxes_[owner] = topic;
        created = true;
    }
    if (created) {
        persist_definition(*topic);
        entities_changed();
    }
    return topic;
}

// --- Subscribing -------------------------------------------------------------

std::unique_ptr<Subscription> PubSub::subscribe(const Identity& identity, const Address& subscriber,
                                                const Address& address, SubscribeOptions options,
                                                std::string* reason) {
    if (!accepting_.load()) {
        if (reason != nullptr) *reason = "pub/sub is shutting down";
        return nullptr;
    }
    if (!authorizer_->authorize(identity.principal, PubSubOperation::Subscribe, address)) {
        if (reason != nullptr) *reason = identity.principal.to_string() + " may not subscribe to " + address.to_string();
        audit("subscribe_refused", identity.principal, address, "no pubsub:subscribe");
        return nullptr;
    }
    auto topic = find_or_auto_create(identity, address, reason);
    if (!topic) return nullptr;
    const auto id = next_subscription_id.fetch_add(1);
    auto state = topic->add_subscription(subscriber, std::move(options), id);
    entities_changed();
    return std::unique_ptr<Subscription>(new Subscription(topic, state, subscriber, address, id));
}

std::unique_ptr<Subscription> PubSub::open_inbox(const Identity& identity, const Address& owner,
                                                 SubscribeOptions options, std::string* reason) {
    if (!accepting_.load()) {
        if (reason != nullptr) *reason = "pub/sub is shutting down";
        return nullptr;
    }
    if (owner.is_wildcard()) {
        if (reason != nullptr) *reason = "an inbox belongs to one address, not a wildcard";
        return nullptr;
    }
    if (!authorizer_->authorize(identity.principal, PubSubOperation::Subscribe, owner)) {
        if (reason != nullptr) *reason = identity.principal.to_string() + " may not read the inbox of " + owner.to_string();
        audit("subscribe_refused", identity.principal, owner, "no pubsub:subscribe on the inbox");
        return nullptr;
    }
    auto topic = inbox_topic_for(owner, true);
    // The owner reads everything waiting in its inbox.
    options.start = StartPosition::Oldest;
    const auto id = next_subscription_id.fetch_add(1);
    auto state = topic->add_subscription(owner, std::move(options), id);
    return std::unique_ptr<Subscription>(new Subscription(topic, state, owner, topic->address(), id));
}

// --- Publishing --------------------------------------------------------------

void PubSub::entities_changed() {
    std::lock_guard lock(mutex_);
    wildcard_cache_.clear();
    wildcard_cache_order_.clear();
}

std::vector<Address> PubSub::resolve_wildcard(const Address& pattern) {
    const auto key = pattern.to_string();
    std::lock_guard lock(mutex_);
    if (auto found = wildcard_cache_.find(key); found != wildcard_cache_.end()) return found->second;
    std::vector<Address> matches;
    for (const auto& [address, topic] : topics_) {
        if (address_matches(pattern, address)) matches.push_back(address);
    }
    for (const auto& [owner, topic] : inboxes_) {
        if (address_matches(pattern, owner)) matches.push_back(owner);
    }
    wildcard_cache_[key] = matches;
    wildcard_cache_order_.push_back(key);
    while (wildcard_cache_order_.size() > config_.wildcard_cache_limit) {
        wildcard_cache_.erase(wildcard_cache_order_.front());
        wildcard_cache_order_.pop_front();
    }
    return matches;
}

void PubSub::audit(std::string action, const Address& principal, const Address& target, std::string detail) {
    std::function<void(const AuditRecord&)> sink;
    {
        std::lock_guard lock(mutex_);
        sink = audit_sink_;
    }
    if (sink) sink(AuditRecord{std::move(action), principal, target, std::move(detail)});
}

void PubSub::set_audit_sink(std::function<void(const AuditRecord&)> sink) {
    std::lock_guard lock(mutex_);
    audit_sink_ = std::move(sink);
}

void PubSub::register_drain(const std::string& kind, std::shared_ptr<InboxDrain> drain) {
    std::lock_guard lock(mutex_);
    drains_[kind] = std::move(drain);
}

std::uint64_t PubSub::total_memory_bytes() const {
    std::vector<std::shared_ptr<Topic>> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [address, topic] : topics_) all.push_back(topic);
        for (const auto& [address, topic] : inboxes_) all.push_back(topic);
    }
    std::uint64_t total = 0;
    for (const auto& topic : all) total += topic->memory_bytes();
    return total;
}

std::uint64_t PubSub::total_disk_bytes() const {
    std::vector<std::shared_ptr<Topic>> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [address, topic] : topics_) all.push_back(topic);
        for (const auto& [address, topic] : inboxes_) all.push_back(topic);
    }
    std::uint64_t total = 0;
    for (const auto& topic : all) total += topic->disk_bytes();
    return total;
}

PublishResult PubSub::publish(const Identity& identity, PublishRequest request) {
    PublishResult result;
    if (!accepting_.load()) {
        result.error = "pub/sub is shutting down";
        return result;
    }
    // The envelope's from/sender are stamped from the identity (D33, D34).
    Event event;
    event.id = request.id.empty() ? new_event_id() : request.id;
    event.envelope.from = identity.principal;
    event.envelope.sender = identity.acting_chain;
    event.envelope.to = std::move(request.to);
    event.envelope.cc = std::move(request.cc);
    event.envelope.bcc = std::move(request.bcc);
    event.envelope.reply_to = std::move(request.reply_to);
    event.envelope.correlation_id = std::move(request.correlation_id);
    event.envelope.topics_chain = std::move(request.topics_chain);
    event.encoding = request.encoding;
    event.mime = mime_types_.intern(request.mime_type);
    event.delivery = request.delivery;
    event.published_at = SystemClock::now();
    if (request.ttl) event.expires_at = event.published_at + *request.ttl;
    event.payload = Payload::make(std::move(request.payload), kDefaultInlineThreshold);
    result.event_id = event.id;

    if (check_sender_chain(event.envelope, config_.max_sender_chain) != EnvelopeProblem::None) {
        result.error = "sender chain of " + std::to_string(event.envelope.sender.size()) +
                       " exceeds the limit of " +
                       std::to_string(std::min(config_.max_sender_chain, kMaxSenderChainDepth));
        log(LogLevel::Error, "dropped event " + event.id + " from " + identity.principal.to_string() + ": " +
                                 result.error);
        return result;
    }
    // Every link of the delegation stack must be allowed to act for the one before it.
    const Address* acting_for = &identity.principal;
    for (const auto& actor : identity.acting_chain) {
        if (!authorizer_->may_act_for(actor, *acting_for)) {
            result.error = actor.to_string() + " may not act for " + acting_for->to_string();
            audit("publish_refused", identity.principal, actor, result.error);
            log(LogLevel::Warn, "refused event " + event.id + ": " + result.error);
            return result;
        }
        acting_for = &actor;
    }

    // Deliver to every recipient: to, cc, bcc.
    auto deliver_list = [&](const std::vector<Address>& list) {
        for (const auto& address : list) {
            if (!address.is_wildcard()) {
                deliver_to(identity, copy_for_recipient(event, address), address, result);
                continue;
            }
            if (!authorizer_->authorize(identity.principal, PubSubOperation::SendWildcard, address)) {
                result.refused.emplace_back(address, "no right to send to a wildcard");
                audit("publish_refused", identity.principal, address, "no pubsub:send_wildcard");
                continue;
            }
            const auto matches = resolve_wildcard(address);
            if (matches.empty()) {
                dead_letter(event, "no address matches " + address.to_string());
                result.refused.emplace_back(address, "no match");
                continue;
            }
            if (matches.size() > config_.max_wildcard_fanout) {
                result.refused.emplace_back(address, "fan-out " + std::to_string(matches.size()) +
                                                         " exceeds the limit of " +
                                                         std::to_string(config_.max_wildcard_fanout));
                log(LogLevel::Warn, "refused wildcard " + address.to_string() + " for event " + event.id + ": " +
                                        std::to_string(matches.size()) + " matches");
                continue;
            }
            audit("wildcard_send", identity.principal, address, std::to_string(matches.size()) + " recipients");
            for (const auto& target : matches) deliver_to(identity, copy_for_recipient(event, address), target, result);
        }
    };
    deliver_list(event.envelope.to);
    deliver_list(event.envelope.cc);
    deliver_list(event.envelope.bcc);
    result.accepted = !result.delivered_to.empty();
    if (!result.accepted && result.error.empty()) {
        result.error = result.refused.empty() ? "no recipients" : result.refused.front().second;
    }
    return result;
}

void PubSub::deliver_to(const Identity& identity, const Event& event, const Address& target,
                        PublishResult& result) {
    if (!authorizer_->authorize(identity.principal, PubSubOperation::Publish, target)) {
        result.refused.emplace_back(target, "no pubsub:publish on " + target.to_string());
        audit("publish_refused", identity.principal, target, "no pubsub:publish");
        return;
    }
    // Topic: bounded by governance, then appended (or queued on the dispatcher).
    if (target.kind == "topic") {
        std::string reason;
        auto topic = find_or_auto_create(identity, target, &reason);
        if (!topic) {
            dead_letter(event, reason);
            result.refused.emplace_back(target, reason);
            return;
        }
        const auto settings = topic->settings();
        if (const auto problem = check_topic_hop(event.envelope, target, settings.topics_chain_max);
            problem != EnvelopeProblem::None) {
            dead_letter(event, std::string(to_string(problem)) + " at " + target.to_string());
            result.refused.emplace_back(target, std::string(to_string(problem)));
            return;
        }
        Event copy = event;
        copy.envelope.topics_chain.push_back(target);
        const auto requested = resolve_delivery(copy.delivery, {}, settings.delivery);
        const auto bounded = apply_bounds(requested, authorizer_->bounds(identity.principal, target),
                                          settings.over_bound);
        if (bounded.rejected) {
            result.refused.emplace_back(target, bounded.reason);
            audit("publish_rejected", identity.principal, target, bounded.reason);
            return;
        }
        if (!bounded.clamped.empty()) {
            for (const auto& field : bounded.clamped) {
                result.clamped.push_back(target.to_string() + "." + field);
                if (field == "priority") copy.delivery.priority = bounded.effective.priority;
                if (field == "recipients") copy.delivery.recipients = bounded.effective.recipients;
                if (field == "guarantee") copy.delivery.guarantee = bounded.effective.guarantee;
            }
            audit("clamped", identity.principal, target, "lowered to the governance bounds");
        }
        if (settings.producer_queue) {
            dispatcher_->submit(target.to_string(), [this, topic, copy = std::move(copy)]() mutable {
                auto outcome = topic->append(copy);
                if (outcome.status != Topic::AppendStatus::Ok) dead_letter(copy, outcome.reason);
            });
            result.delivered_to.push_back(target);
            return;
        }
        auto outcome = topic->append(std::move(copy));
        if (outcome.status != Topic::AppendStatus::Ok) {
            result.refused.emplace_back(target, outcome.reason);
            return;
        }
        result.delivered_to.push_back(target);
        return;
    }
    // A kind with a drain: table:, function:, model:, service:, ...
    std::shared_ptr<InboxDrain> drain;
    {
        std::lock_guard lock(mutex_);
        if (auto found = drains_.find(target.kind); found != drains_.end()) drain = found->second;
    }
    if (drain) {
        dispatcher_->submit(target.to_string(), [this, drain, target, event]() { run_drain(drain, target, event); });
        result.delivered_to.push_back(target);
        return;
    }
    // Anything else addressable waits in its inbox; agents get one on first delivery.
    auto inbox = inbox_topic_for(target, target.kind == "agent");
    if (!inbox) {
        dead_letter(event, "no such address " + target.to_string());
        result.refused.emplace_back(target, "no such address");
        return;
    }
    auto outcome = inbox->append(event);
    if (outcome.status != Topic::AppendStatus::Ok) {
        result.refused.emplace_back(target, outcome.reason);
        return;
    }
    result.delivered_to.push_back(target);
}

void PubSub::run_drain(std::shared_ptr<InboxDrain> drain, Address target, Event event) {
    DrainResult outcome;
    try {
        outcome = drain->drain(target, event);
    } catch (const std::exception& error) {  // a drain must not take the dispatcher down
        outcome.ok = false;
        outcome.error = std::string("drain threw: ") + error.what();
    }
    if (!outcome.ok) {
        report_error(target, event, outcome.error);
        return;
    }
    if (!outcome.result) return;
    const auto policy = event.delivery.reply.value_or(drain->default_reply());
    if (policy == ReplyPolicy::None) return;
    send_reply(target, event, policy, std::move(*outcome.result), outcome.result_encoding);
}

void PubSub::send_reply(const Address& from, const Event& request, ReplyPolicy policy, std::string payload,
                        Encoding encoding) {
    std::vector<Address> recipients = request.envelope.reply_to;
    if (policy == ReplyPolicy::ReplyAll) {
        if (recipients.empty() && request.envelope.from) recipients.push_back(*request.envelope.from);
        for (const auto& list : {request.envelope.to, request.envelope.cc}) {
            for (const auto& address : list) {
                if (address != from && std::ranges::find(recipients, address) == recipients.end()) {
                    recipients.push_back(address);
                }
            }
        }
    }
    if (recipients.empty()) return;
    PublishRequest reply;
    reply.to = std::move(recipients);
    reply.correlation_id = request.envelope.correlation_id;
    reply.encoding = encoding;
    reply.payload = std::move(payload);
    const auto result = publish(Identity{from, {}}, std::move(reply));
    if (!result.accepted) log(LogLevel::Warn, "reply from " + from.to_string() + " not delivered: " + result.error);
}

void PubSub::dead_letter(const Event& event, std::string reason) {
    if (std::ranges::find(event.envelope.topics_chain, dead_letter_address()) != event.envelope.topics_chain.end()) {
        log(LogLevel::Warn, "dropped event " + event.id + " (" + reason + "); it was already a dead letter");
        return;
    }
    TopicSettings settings;
    settings.overflow = OverflowPolicy::SkipAhead;
    auto topic = ensure_system_topic(dead_letter_address(), settings);
    Event letter = event;
    letter.headers["reason"] = reason;
    letter.expires_at.reset();  // a dead letter is kept for inspection, not expired again
    letter.envelope.topics_chain.push_back(dead_letter_address());
    const auto outcome = topic->append(std::move(letter));
    if (outcome.status != Topic::AppendStatus::Ok) {
        log(LogLevel::Warn, "dropped event " + event.id + " (" + reason + "); dead-letter topic refused it: " +
                                outcome.reason);
    }
}

void PubSub::report_error(const Address& recipient, const Event& event, std::string error) {
    const auto address = error_topic_for(recipient);
    TopicSettings settings;
    settings.overflow = OverflowPolicy::SkipAhead;
    auto topic = ensure_system_topic(address, settings);  // created on the first error (D37)
    Event copy = event;
    copy.headers["error"] = error;
    copy.expires_at.reset();
    copy.headers["recipient"] = recipient.to_string();
    copy.envelope.topics_chain.push_back(address);
    const auto outcome = topic->append(std::move(copy));
    log(LogLevel::Warn, recipient.to_string() + " failed event " + event.id + ": " + error +
                            (outcome.status == Topic::AppendStatus::Ok ? " (recorded in " + address.to_string() + ")"
                                                                       : " (error topic refused it)"));
}

// --- Maintenance -------------------------------------------------------------

void PubSub::maintain() {
    const auto now = SteadyClock::now();
    std::vector<std::shared_ptr<Topic>> topics;
    std::vector<std::pair<Address, std::shared_ptr<Topic>>> inboxes;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [address, topic] : topics_) topics.push_back(topic);
        for (const auto& [owner, topic] : inboxes_) inboxes.emplace_back(owner, topic);
    }
    for (auto& topic : topics) topic->maintain(now, config_.spill_log_interval);
    for (auto& [owner, topic] : inboxes) {
        topic->maintain(now, config_.spill_log_interval);
        // An inbox nobody has read for its retention is closed; what is still in
        // it goes to the dead letters (D31, D32).
        const auto idle = topic->idle_since();
        const auto retention = owner.kind == "agent" ? std::chrono::milliseconds(kAgentInboxRetention)
                                                     : std::chrono::milliseconds(0);
        if (!idle || topic->subscription_count() > 0 || now - *idle < retention) continue;
        for (auto& level : topic->snapshot()) {
            for (auto& event : level) dead_letter(event, "inbox retention expired for " + owner.to_string());
        }
        {
            std::lock_guard lock(mutex_);
            inboxes_.erase(owner);
        }
        topic->close_all("inbox retention expired");
        topic->detach();
        std::error_code error;
        std::filesystem::remove_all(topics_directory() / encode_name(topic->address().to_string()), error);
        std::filesystem::remove_all(spill_directory_for(topic->address()), error);
        log(LogLevel::Info, "closed the inbox of " + owner.to_string() + " after its retention");
        entities_changed();
    }
}

void PubSub::drain_dispatch() { dispatcher_->drain(); }

}  // namespace zetta::pubsub
