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
namespace zetta {
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
    state_->document.set_actor_id(am::ActorId{uuid_to_bytes(node_id_)});
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

void ReplicationManager::remove(const std::string& key) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->document.transact([&](am::Transaction& transaction) {
        transaction.delete_key(am::root, key);
    });
}

nlohmann::json ReplicationManager::snapshot() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    nlohmann::json result = nlohmann::json::object();
    for (const std::string& key : state_->document.keys(am::root)) {
        const std::optional<std::string> encoded =
            state_->document.get<std::string>(am::root, key);
        if (!encoded) continue;
        nlohmann::json value =
            nlohmann::json::parse(*encoded, nullptr, /*allow_exceptions=*/false);
        result[key] = value.is_discarded() ? nlohmann::json(*encoded)
                                            : std::move(value);
    }
    return result;
}

std::size_t ReplicationManager::entry_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->document.keys(am::root).size();
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

}  // namespace zetta
