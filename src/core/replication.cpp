#include "zetta/replication.hpp"

#include <mutex>
#include <optional>
#include <utility>

#include <automerge-cpp/automerge.hpp>

#include "zetta/uuid.hpp"

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
    : state_(std::make_unique<State>()), policy_(policy) {
    // The node's UUID is its Automerge actor id (distinct per node, so
    // concurrent changes merge conflict-free).
    state_->document.set_actor_id(am::ActorId{uuid_to_bytes(node_id)});
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
