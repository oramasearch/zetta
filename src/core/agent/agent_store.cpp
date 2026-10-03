#include "zetta/agent/agent_store.hpp"

#include <algorithm>
#include <memory>
#include <string>
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

// Reads take the replication manager's shared immutable view — one pointer copy
// under its mutex — and never the deep copy `snapshot()` makes (#1227). `get`
// is one map lookup; `list` walks only the `agent:` key range.
std::optional<AgentRecord> AgentStore::get(const std::string& id) const {
    if (id.empty()) return std::nullopt;
    const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
    const auto found = document->find(catalog::agent_key(id));
    if (found == document->end()) return std::nullopt;
    return agent_from_json(*found, nullptr);
}

std::vector<AgentRecord> AgentStore::list() const {
    std::vector<AgentRecord> agents;
    const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
    catalog::for_each_with_prefix(
        *document, catalog::kAgentPrefix,
        [&agents](const std::string&, const nlohmann::json& value) {
            if (std::optional<AgentRecord> agent = agent_from_json(value, nullptr)) {
                agents.push_back(*std::move(agent));
            }
        });
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
