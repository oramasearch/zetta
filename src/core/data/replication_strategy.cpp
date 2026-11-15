// SPDX-License-Identifier: MIT

#include "zetta/data/replication_strategy.hpp"

#include <array>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace zetta::data {

std::string_view to_string(ReplicationStrategyKind kind) {
    switch (kind) {
        case ReplicationStrategyKind::ReplicateToAll:
            return "replicate_to_all";
        case ReplicationStrategyKind::Partitioned:
            return "partitioned";
        case ReplicationStrategyKind::PartitionedWithReplicas:
            return "partitioned_with_replicas";
    }
    return "unknown";
}

bool replication_strategy_kind_from_string(std::string_view text,
                                           ReplicationStrategyKind& out) {
    constexpr std::array<ReplicationStrategyKind, 3> kinds{
        ReplicationStrategyKind::ReplicateToAll,
        ReplicationStrategyKind::Partitioned,
        ReplicationStrategyKind::PartitionedWithReplicas};
    for (const ReplicationStrategyKind kind : kinds) {
        if (to_string(kind) == text) {
            out = kind;
            return true;
        }
    }
    return false;
}

nlohmann::json ReplicationStrategy::to_json() const {
    nlohmann::json document{{"kind", to_string(kind_)}};
    if (kind_ == ReplicationStrategyKind::PartitionedWithReplicas) {
        document["replication_factor"] = replication_factor_;
    }
    return document;
}

std::optional<ReplicationStrategy> ReplicationStrategy::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("kind") || !value["kind"].is_string()) {
        return std::nullopt;
    }
    ReplicationStrategyKind kind{};
    if (!replication_strategy_kind_from_string(value["kind"].get<std::string>(),
                                               kind)) {
        return std::nullopt;
    }
    if (kind != ReplicationStrategyKind::PartitionedWithReplicas) {
        // Only partitioned_with_replicas carries a factor; the field is ignored
        // (and need not be present) for the other kinds.
        return kind == ReplicationStrategyKind::Partitioned
                   ? ReplicationStrategy::partitioned()
                   : ReplicationStrategy::replicate_to_all();
    }
    // partitioned_with_replicas: replication_factor is optional (defaults to
    // kDefaultReplicationFactor), but when present must be a positive integer.
    if (!value.contains("replication_factor")) {
        return ReplicationStrategy::partitioned_with_replicas();
    }
    const nlohmann::json& factor = value["replication_factor"];
    if (!factor.is_number_integer()) return std::nullopt;
    const std::int64_t parsed = factor.get<std::int64_t>();
    if (parsed < 1) return std::nullopt;
    return ReplicationStrategy::partitioned_with_replicas(
        static_cast<int>(parsed));
}

// The ReplicationStrategy JSON Schema (draft 2020-12) — the code-owned source of truth;
// schemas/replication_strategy.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& replication_strategy_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/replication_strategy.schema.json",
  "title": "ReplicationStrategy",
  "description": "The per-table / per-database replication strategy (ENT-35) — how a given table or database's replicated state is laid out across the cluster. The per-entity counterpart to ReplicationManager's node-wide ReplicationPolicy. ReplicateToAll keeps a full copy on every node (the default, written nowhere so the projection is byte-identical to before); Partitioned shards a single copy per shard; PartitionedWithReplicas shards with replication_factor total copies per shard (default 3). Mirrored by zetta::ReplicationStrategy in include/zetta/replication_strategy.hpp and enforced by ReplicationStrategy::from_json() in src/core/replication_strategy.cpp — keep them in sync. This slice records/persists/replicates the choice; the partitioned-placement engine that acts on a non-default strategy is a later slice. A malformed strategy is rejected at the boundary. (The conditional 'replication_factor is required-with-default for partitioned_with_replicas, ignored otherwise' is enforced in from_json, beyond what this schema expresses.)",
  "type": "object",
  "required": ["kind"],
  "additionalProperties": false,
  "properties": {
    "kind": {
      "description": "The strategy family. Append-only enum (the snake_case name is the stored identifier). 'replicate_to_all' = a full copy on every node (the default); 'partitioned' = sharded, one copy per shard; 'partitioned_with_replicas' = sharded, replication_factor copies per shard.",
      "type": "string",
      "enum": ["replicate_to_all", "partitioned", "partitioned_with_replicas"]
    },
    "replication_factor": {
      "description": "Total copies of each shard. Present only for kind 'partitioned_with_replicas' (a positive integer; defaults to 3 when omitted). Ignored for the other kinds.",
      "type": "integer",
      "minimum": 1
    }
  }
}
)JSONSCHEMA");
    return schema;
}

ReplicationStrategy replication_strategy_from_meta(const nlohmann::json& meta) {
    if (!meta.is_object() || !meta.contains("replication")) return {};
    if (auto parsed = ReplicationStrategy::from_json(meta["replication"])) {
        return *parsed;
    }
    return {};  // present-but-malformed → safe default (full replication)
}

}  // namespace zetta::data