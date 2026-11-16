// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::data {

// The per-table / per-database replication strategy (ENT-35) — *how* a given
// table or database's replicated state is laid out across the cluster. It is the
// per-entity counterpart to ReplicationManager's node-wide ReplicationPolicy
// (include/zetta/cluster/replication.hpp): the manager's policy is one knob for the whole
// document; this records a choice carried by each database/table.
//
// Three strategies:
//   - ReplicateToAll          every node holds a full copy (the default; today's
//                             behavior, identical to ReplicationPolicy's
//                             ReplicateToAllNodes).
//   - Partitioned             sharded across nodes, a single copy per shard (no
//                             replicas).
//   - PartitionedWithReplicas sharded with `replication_factor` total copies of
//                             each shard (the default factor is 3).
//
// SCOPE (this slice): zetta *records, persists, and replicates* the choice — a
// table/db can be created with a strategy, and the default ReplicateToAll
// preserves current behavior exactly (it is written nowhere, so the on-disk and
// CRDT representations are byte-identical to before). The partitioned-placement
// engine that actually *acts* on a non-default strategy (storing only a subset of
// shards/replicas on each node, instead of a full copy everywhere) is a later
// slice — the same "record the choice now, drive it later" shape as
// DeploymentTarget's dial (R1 Local only) and ScalingPolicy (the reward model
// before the autoscaler that consumes it).

// The strategy family. Declaration order is stable because the snake_case name is
// BOTH the display label AND the stored JSON/disk representation — a durable
// identifier; append new kinds, never reorder or rename.
enum class ReplicationStrategyKind {
    ReplicateToAll,           // full copy on every node (the default)
    Partitioned,              // sharded, one copy per shard (no replicas)
    PartitionedWithReplicas,  // sharded, `replication_factor` copies per shard
};

// The default total copies per shard for PartitionedWithReplicas when a factor is
// not given (a common, safe quorum-friendly default).
inline constexpr int kDefaultReplicationFactor = 3;

// Canonical snake_case name of a strategy kind ("replicate_to_all" /
// "partitioned" / "partitioned_with_replicas"). Both the display label and the
// stored representation.
std::string_view to_string(ReplicationStrategyKind kind);

// Parse a stored/canonical strategy-kind name back to the enum; false (out
// untouched) on an unknown spelling — the boundary check for the enum.
bool replication_strategy_kind_from_string(std::string_view text,
                                           ReplicationStrategyKind& out);

// A table's / database's replication strategy: its kind plus, for
// PartitionedWithReplicas, the total number of copies of each shard
// (`replication_factor`). The default-constructed value is ReplicateToAll — the
// behavior every database/table has today.
class ReplicationStrategy {
public:
    ReplicationStrategy() = default;

    // Factories — the readable way to build each strategy.
    static ReplicationStrategy replicate_to_all() { return {}; }
    static ReplicationStrategy partitioned() {
        return ReplicationStrategy(ReplicationStrategyKind::Partitioned, 1);
    }
    // `replication_factor` is the total copies per shard (>= 1); defaults to
    // kDefaultReplicationFactor. A factor < 1 is clamped to 1.
    static ReplicationStrategy partitioned_with_replicas(
        int replication_factor = kDefaultReplicationFactor) {
        return ReplicationStrategy(
            ReplicationStrategyKind::PartitionedWithReplicas,
            replication_factor < 1 ? 1 : replication_factor);
    }

    ReplicationStrategyKind kind() const { return kind_; }

    // Total copies of each shard. For PartitionedWithReplicas this is the chosen
    // factor; for Partitioned it is 1 (a single copy); for ReplicateToAll it is
    // not a fixed number (every node holds a copy) and reads back as 1.
    int replication_factor() const { return replication_factor_; }

    // Is this the default (ReplicateToAll)? When true the strategy is omitted from
    // the on-disk / CRDT projection, so the default representation is unchanged
    // from before this entity carried a strategy.
    bool is_default() const {
        return kind_ == ReplicationStrategyKind::ReplicateToAll;
    }

    bool operator==(const ReplicationStrategy&) const = default;

    // The JSON value shape (mirrors schemas/replication_strategy.schema.json):
    //   { "kind": "replicate_to_all" }
    //   { "kind": "partitioned" }
    //   { "kind": "partitioned_with_replicas", "replication_factor": <int>=1> }
    // `replication_factor` is present only for partitioned_with_replicas.
    nlohmann::json to_json() const;

    // Parse from JSON, validating the shape at the boundary so a malformed
    // strategy fails fast instead of silently degrading the layout choice. Returns
    // std::nullopt on anything malformed: not an object, a missing/unknown kind,
    // or (for partitioned_with_replicas) a replication_factor that is not a
    // positive integer. A partitioned_with_replicas value with no replication_factor
    // defaults to kDefaultReplicationFactor.
    static std::optional<ReplicationStrategy> from_json(
        const nlohmann::json& value);

private:
    ReplicationStrategy(ReplicationStrategyKind kind, int replication_factor)
        : kind_(kind), replication_factor_(replication_factor) {}

    ReplicationStrategyKind kind_ = ReplicationStrategyKind::ReplicateToAll;
    int replication_factor_ = 1;  // total copies per shard (see above)
};

// The ReplicationStrategy JSON Schema (draft 2020-12) — the code-owned source of
// truth; schemas/replication_strategy.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& replication_strategy_schema();

// Read the optional "replication" strategy field out of a metadata document (a
// database.json / table.json file, or a db:/tbl: CRDT value) at the boundary.
// Absent → the default (ReplicateToAll). Present-but-malformed → also the default:
// the strategy is advisory layout metadata, so a clearly invalid value falls back
// to safe full replication rather than stranding the table/database on load. The
// strict failing parse is ReplicationStrategy::from_json (used wherever a strategy
// enters from outside, e.g. a future API/DDL boundary).
ReplicationStrategy replication_strategy_from_meta(const nlohmann::json& meta);

}  // namespace zetta::data