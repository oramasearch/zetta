#include "zetta/replication_strategy.hpp"

#include <array>
#include <cstdint>

namespace zetta {

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

ReplicationStrategy replication_strategy_from_meta(const nlohmann::json& meta) {
    if (!meta.is_object() || !meta.contains("replication")) return {};
    if (auto parsed = ReplicationStrategy::from_json(meta["replication"])) {
        return *parsed;
    }
    return {};  // present-but-malformed → safe default (full replication)
}

}  // namespace zetta
