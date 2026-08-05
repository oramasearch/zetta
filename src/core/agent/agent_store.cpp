#include "zetta/agent/agent_store.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "../catalog.hpp"
#include "zetta/cluster/replication.hpp"

namespace zetta::agent {

AgentStore::AgentStore(zetta::cluster::ReplicationManager& replication)
    : replication_(replication) {}

std::optional<AgentRecord> AgentStore::put(const AgentRecord& agent, std::string* error) {
    // Validate by round-tripping through the boundary parser, then capture.
    std::string reason;
    std::optional<AgentRecord> validated = agent_from_json(to_json(agent), &reason);
    if (!validated) {
        if (error != nullptr) *error = reason;
        return std::nullopt;
    }
    replication_.set(catalog::agent_key(validated->id), to_json(*validated));
    return validated;
}

std::optional<AgentRecord> AgentStore::get(const std::string& id) const {
    if (id.empty()) return std::nullopt;
    const nlohmann::json document = replication_.snapshot();
    const auto found = document.find(catalog::agent_key(id));
    if (found == document.end()) return std::nullopt;
    return agent_from_json(*found, nullptr);
}

std::vector<AgentRecord> AgentStore::list() const {
    std::vector<AgentRecord> agents;
    const nlohmann::json document = replication_.snapshot();
    for (const auto& [key, value] : document.items()) {
        if (!std::string_view(key).starts_with(catalog::kAgentPrefix)) continue;
        if (std::optional<AgentRecord> agent = agent_from_json(value, nullptr)) {
            agents.push_back(*std::move(agent));
        }
    }
    std::sort(agents.begin(), agents.end(),
              [](const AgentRecord& left, const AgentRecord& right) {
                  return left.id < right.id;
              });
    return agents;
}

bool AgentStore::remove(const std::string& id) {
    if (id.empty() || !get(id).has_value()) return false;  // snapshot-present (tombstone-aware)
    replication_.remove(catalog::agent_key(id));
    return true;
}

}  // namespace zetta::agent
