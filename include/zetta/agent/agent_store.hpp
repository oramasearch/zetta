// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zetta/agent/agent_record.hpp"

namespace zetta::cluster {
class ReplicationManager;
}

namespace zetta::agent {

// CRUD over the replicated `agent:<uuid>` entities. The CRDT document is the source of
// truth — put/remove capture into it, list/get read its snapshot — so agents replicate +
// persist like every other entity, with no parallel store. Pure: it stores records as
// given; id assignment + timestamps are the caller's (the MCP/REST layer). Non-throwing.
class AgentStore {
public:
    explicit AgentStore(zetta::cluster::ReplicationManager& replication);

    // Create or replace `agent` (captures `agent:<id>`). std::nullopt (+ *error) when the
    // record is malformed (empty id/name/model, or a bad tools list).
    std::optional<AgentRecord> put(const AgentRecord& agent, std::string* error);

    std::optional<AgentRecord> get(const std::string& id) const;  // nullopt if absent
    std::vector<AgentRecord> list() const;                        // all, id-ordered
    bool remove(const std::string& id);                           // false if absent

private:
    zetta::cluster::ReplicationManager& replication_;
};

}  // namespace zetta::agent
