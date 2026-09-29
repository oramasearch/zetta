#include "zetta/cluster/replication.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <automerge-cpp/automerge.hpp>

#include "zetta/util/uuid.hpp"

// src/core/replication.cpp is the CRDT adapter: the one .cpp that uses
// automerge-cpp. ReplicationManager's public header exposes no Automerge types
// (only JSON + bytes), so the rest of zetta stays decoupled from the backend.
namespace zetta::cluster {
namespace am = automerge_cpp;

struct ReplicationManager::State {
    struct ChangeRecord {
        ReplicationChangeToken token;
        std::string key;
    };

    am::Document document;
    std::shared_ptr<nlohmann::json> snapshot =
        std::make_shared<nlohmann::json>(nlohmann::json::object());
    std::unordered_map<std::string, std::string> encoded_values;
    std::deque<ChangeRecord> changes;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t audit_encoded_bytes = 0;
    std::uint64_t audit_items = 0;
    std::uint64_t change_key_bytes = 0;
    ReplicationChangeToken current_token = 1;
    mutable std::mutex mutex;
};

ReplicationManager::ReplicationManager(std::string node_id,
                                       ReplicationPolicy policy)
    : state_(std::make_unique<State>()),
      node_id_(std::move(node_id)),
      policy_(policy) {
    // The node's UUID is its Automerge actor id (distinct per node, so
    // concurrent changes merge conflict-free).
    state_->document.set_actor_id(am::ActorId{zetta::util::uuid_to_bytes(node_id_)});
}

ReplicationManager::~ReplicationManager() = default;

namespace {

constexpr std::size_t kChangeJournalCapacity = 65536;

template <typename StateType>
void make_snapshot_mutable(StateType& state) {
    if (state.snapshot.use_count() == 1) return;
    state.snapshot = std::make_shared<nlohmann::json>(*state.snapshot);
}

template <typename StateType>
void record_change(StateType& state, const std::string& key) {
    ++state.current_token;
    state.changes.push_back({state.current_token, key});
    state.change_key_bytes += key.size();
    if (state.changes.size() > kChangeJournalCapacity) {
        state.change_key_bytes -= state.changes.front().key.size();
        state.changes.pop_front();
    }
}

bool is_audit_key(const std::string& key) {
    return key.starts_with("audit:");
}

template <typename StateType>
void account_encoded_value(StateType& state, const std::string& key,
                           const std::string* encoded) {
    const auto existing = state.encoded_values.find(key);
    if (existing != state.encoded_values.end()) {
        const auto old_bytes = existing->first.size() +
                               existing->second.size();
        state.encoded_bytes -= old_bytes;
        if (is_audit_key(existing->first)) {
            state.audit_encoded_bytes -= old_bytes;
            --state.audit_items;
        }
    }
    if (encoded) {
        const auto new_bytes = key.size() + encoded->size();
        state.encoded_bytes += new_bytes;
        if (is_audit_key(key)) {
            state.audit_encoded_bytes += new_bytes;
            ++state.audit_items;
        }
    }
}

template <typename StateType>
void cache_live_value(StateType& state, const std::string& key,
                      const std::string& encoded) {
    account_encoded_value(state, key, &encoded);
    state.encoded_values[key] = encoded;
    nlohmann::json value =
        nlohmann::json::parse(encoded, nullptr, /*allow_exceptions=*/false);
    (*state.snapshot)[key] =
        value.is_discarded() ? nlohmann::json(encoded) : std::move(value);
}

}  // namespace

void ReplicationManager::set(const std::string& key,
                             const nlohmann::json& value) {
    // Store each value as its JSON text — supports any value (incl. nested) and
    // keeps the public API JSON-only. The CRDT still merges per key.
    const std::string encoded = value.dump();
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->document.transact([&](am::Transaction& transaction) {
        transaction.put(am::root, key, encoded);
    });
    make_snapshot_mutable(*state_);
    cache_live_value(*state_, key, encoded);
    record_change(*state_, key);
}

namespace {

// The tombstone a removal writes. `_removed` is deliberately distinct from the
// `deleted` flag a `row:` tombstone carries: a row tombstone is a real entry that
// reconcile must SEE (it routes by its `tbl` field to call materialize_row_delete),
// whereas this marker means "treat this key as absent" and is filtered out of
// every snapshot. The leading underscore marks it engine-internal, like `_zid`.
constexpr std::string_view kRemovedMarker = "_removed";

// Is this stored value a removal tombstone? Values are stored as JSON text, so
// the cheap substring check gates the parse — the overwhelmingly common case is a
// live entry, and every key is examined on every snapshot.
bool is_tombstone(const std::string& encoded) {
    if (encoded.find(kRemovedMarker) == std::string::npos) return false;
    const nlohmann::json value =
        nlohmann::json::parse(encoded, nullptr, /*allow_exceptions=*/false);
    return value.is_object() && value.value(std::string(kRemovedMarker), false);
}

}  // namespace

void ReplicationManager::remove(const std::string& key) {
    // A tombstone VALUE, not a map delete — see the header for why (#838).
    const std::string encoded =
        nlohmann::json{{std::string(kRemovedMarker), true}}.dump();
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->document.transact([&](am::Transaction& transaction) {
        transaction.put(am::root, key, encoded);
    });
    make_snapshot_mutable(*state_);
    account_encoded_value(*state_, key, &encoded);
    state_->encoded_values[key] = encoded;
    state_->snapshot->erase(key);
    record_change(*state_, key);
}

// Is the key tombstoned in ANY of its concurrent values? Two nodes that
// concurrently remove and rewrite a key both survive the merge as conflicting
// values at that key; asking `get` would return whichever one automerge picks as
// the winner, which is deterministic but arbitrary. Asking all of them makes
// removal win — the property a cleanup path needs, since a half-removed entity is
// exactly what #838 produced.
bool ReplicationManager::removed_locked(const std::string& key) const {
    for (const am::Value& value : state_->document.get_all(am::root, key)) {
        const auto* scalar = std::get_if<am::ScalarValue>(&value);
        if (!scalar) continue;
        if (const auto* text = std::get_if<std::string>(scalar);
            text && is_tombstone(*text)) {
            return true;
        }
    }
    return false;
}

nlohmann::json ReplicationManager::snapshot() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return *state_->snapshot;
}

std::shared_ptr<const nlohmann::json> ReplicationManager::shared_snapshot() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->snapshot;
}

ReplicationChanges ReplicationManager::changes_since(
    ReplicationChangeToken token) const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    ReplicationChanges result;
    result.next_token = state_->current_token;
    result.snapshot = state_->snapshot;
    if (token == 0 || token > state_->current_token) {
        result.full_resync = true;
        return result;
    }
    if (token == state_->current_token) return result;
    if (state_->changes.empty() ||
        token + 1 < state_->changes.front().token) {
        result.full_resync = true;
        return result;
    }

    std::unordered_set<std::string> seen;
    for (const State::ChangeRecord& change : state_->changes) {
        if (change.token <= token || !seen.insert(change.key).second) continue;
        result.keys.push_back(change.key);
    }
    return result;
}

bool ReplicationManager::knows_key(const std::string& key) const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->document.get<std::string>(am::root, key).has_value();
}

std::size_t ReplicationManager::entry_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->snapshot->size();
}

ReplicationMemoryUsage ReplicationManager::memory_usage() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    ReplicationMemoryUsage usage;
    constexpr std::uint64_t kCopiesPerLiveValue = 3;
    constexpr std::uint64_t kEntryOverhead = 160;
    const auto all_value_bytes = state_->encoded_bytes * kCopiesPerLiveValue +
                                 state_->encoded_values.size() * kEntryOverhead;
    usage.audit_bytes = state_->audit_encoded_bytes * kCopiesPerLiveValue +
                        state_->audit_items * kEntryOverhead;
    usage.bytes = all_value_bytes - usage.audit_bytes;
    usage.items = state_->encoded_values.size() - state_->audit_items;
    usage.pending_change_bytes = state_->change_key_bytes +
                                 state_->changes.size() * sizeof(State::ChangeRecord);
    usage.pending_changes = state_->changes.size();
    usage.bytes += usage.pending_change_bytes;
    usage.audit_items = state_->audit_items;
    return usage;
}

std::string ReplicationManager::version() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<am::ChangeHash> heads = state_->document.get_heads();
    if (heads.empty()) return "empty";
    // Sort so the fingerprint is order-independent: two documents with the same
    // set of heads (i.e. converged) hash identically regardless of merge order.
    std::sort(heads.begin(), heads.end());
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a (64-bit) offset basis
    for (const am::ChangeHash& head : heads) {
        for (const std::byte raw : head.bytes) {
            hash ^= static_cast<std::uint8_t>(raw);
            hash *= 1099511628211ull;  // FNV-1a prime
        }
    }
    // Fold to 32 bits and render 8 hex chars — short but collision-resistant
    // enough to tell converged from diverged documents at a glance.
    const std::uint32_t folded =
        static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return std::format("{:08x}", folded);
}

std::vector<std::byte> ReplicationManager::save() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->document.save();
}

void ReplicationManager::merge(std::span<const std::byte> blob) {
    std::optional<am::Document> other = am::Document::load(blob);
    if (!other) return;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->document.merge(*other);

    std::unordered_set<std::string> document_keys;
    std::vector<std::string> changed_keys;
    for (const std::string& key : state_->document.keys(am::root)) {
        const std::optional<std::string> encoded =
            state_->document.get<std::string>(am::root, key);
        if (!encoded) continue;
        document_keys.insert(key);
        const bool removed = is_tombstone(*encoded) || removed_locked(key);
        const std::string effective =
            removed
                ? nlohmann::json{{std::string(kRemovedMarker), true}}.dump()
                : *encoded;
        const auto cached = state_->encoded_values.find(key);
        if (cached != state_->encoded_values.end() &&
            cached->second == effective) {
            continue;
        }
        changed_keys.push_back(key);
    }
    for (const auto& [key, encoded] : state_->encoded_values) {
        if (!document_keys.contains(key)) changed_keys.push_back(key);
    }
    if (changed_keys.empty()) return;

    make_snapshot_mutable(*state_);
    for (const std::string& key : changed_keys) {
        const std::optional<std::string> encoded =
            state_->document.get<std::string>(am::root, key);
        if (!encoded) {
            account_encoded_value(*state_, key, nullptr);
            state_->encoded_values.erase(key);
            state_->snapshot->erase(key);
            record_change(*state_, key);
            continue;
        }
        const bool removed = is_tombstone(*encoded) || removed_locked(key);
        if (removed) {
            const std::string tombstone =
                nlohmann::json{{std::string(kRemovedMarker), true}}.dump();
            account_encoded_value(*state_, key, &tombstone);
            state_->encoded_values[key] = tombstone;
            state_->snapshot->erase(key);
        } else {
            cache_live_value(*state_, key, *encoded);
        }
        record_change(*state_, key);
    }
}

}  // namespace zetta::cluster
