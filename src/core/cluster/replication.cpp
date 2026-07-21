#include "zetta/cluster/replication.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <utility>

#include <automerge-cpp/automerge.hpp>

#include "zetta/util/uuid.hpp"

// src/core/replication.cpp is the CRDT adapter: the one .cpp that uses
// automerge-cpp. ReplicationManager's public header exposes no Automerge types
// (only JSON + bytes), so the rest of zetta stays decoupled from the backend.
namespace zetta::cluster {
namespace am = automerge_cpp;

struct ReplicationManager::State {
    am::Document document;
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

void ReplicationManager::set(const std::string& key,
                             const nlohmann::json& value) {
    // Store each value as its JSON text — supports any value (incl. nested) and
    // keeps the public API JSON-only. The CRDT still merges per key.
    const std::string encoded = value.dump();
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->document.transact([&](am::Transaction& transaction) {
        transaction.put(am::root, key, encoded);
    });
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
    nlohmann::json result = nlohmann::json::object();
    for (const std::string& key : state_->document.keys(am::root)) {
        const std::optional<std::string> encoded =
            state_->document.get<std::string>(am::root, key);
        if (!encoded) continue;
        if (is_tombstone(*encoded) || removed_locked(key)) continue;
        nlohmann::json value =
            nlohmann::json::parse(*encoded, nullptr, /*allow_exceptions=*/false);
        result[key] = value.is_discarded() ? nlohmann::json(*encoded)
                                            : std::move(value);
    }
    return result;
}

bool ReplicationManager::knows_key(const std::string& key) const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->document.get<std::string>(am::root, key).has_value();
}

std::size_t ReplicationManager::entry_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::size_t live = 0;
    for (const std::string& key : state_->document.keys(am::root)) {
        const std::optional<std::string> encoded =
            state_->document.get<std::string>(am::root, key);
        if (!encoded) continue;
        if (is_tombstone(*encoded) || removed_locked(key)) continue;
        ++live;
    }
    return live;
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
}

}  // namespace zetta::cluster