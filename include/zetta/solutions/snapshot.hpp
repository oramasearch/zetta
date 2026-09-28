#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Point-in-time snapshot tables and row-level as-of joins (#1103).
//
// Snapshot tables use the logical key `(entity_id, as_of)`. Each materialized
// row also records `_source_as_of` and `_source_table`, so downstream lineage
// can prove which observation was visible at the requested cutoff. Tables are
// ordinary Node-owned tables: their metadata and rows follow the existing CRDT
// capture/reconcile path rather than introducing another state channel.
//
// See docs/solutions/point-in-time-snapshots.md.
namespace zetta {
class Node;
}

namespace zetta::data {
class Table;
}

namespace zetta::solutions {

enum class TimePointKind {
    Numeric,
    Iso8601,
};

struct TimePoint {
    TimePointKind kind = TimePointKind::Numeric;
    double value = 0.0;

    bool operator==(const TimePoint&) const = default;
};

// Parse a finite numeric instant or an ISO-8601 UTC date/date-time. A date-only
// value means midnight UTC. `error` receives a path-ready reason on failure.
std::optional<TimePoint> parse_time_point(const nlohmann::json& value,
                                          std::string* error = nullptr);

struct SnapshotSpec {
    std::string database;
    std::string source_table;
    std::string target_table;
    std::string entity_column;
    std::string time_column;
    nlohmann::json as_of;

    nlohmann::json to_json() const;
    static std::optional<SnapshotSpec> from_json(const nlohmann::json& value,
                                                 std::string* error = nullptr);

    bool operator==(const SnapshotSpec&) const = default;
};

const nlohmann::json& snapshot_spec_schema();

struct SnapshotResult {
    std::shared_ptr<zetta::data::Table> table;
    std::size_t source_rows = 0;
    std::size_t materialized_rows = 0;
    std::size_t existing_rows = 0;
    std::size_t future_rows_excluded = 0;
    std::string error;

    bool ok() const noexcept {
        return error.empty() && static_cast<bool>(table);
    }
};

// Materialize the latest source observation per entity at or before `as_of`.
// Repeating the same request is idempotent; a conflicting row at an existing
// `(entity_id, as_of)` key is refused. Partition-only sources are read through
// the existing one-owner-per-partition scatter/gather path.
SnapshotResult materialize_snapshot(Node& node, const SnapshotSpec& spec);

struct AsOfJoinSpec {
    std::string left_entity_column;
    std::string left_time_column;
    std::string right_entity_column = "entity_id";
    std::string right_time_column = "as_of";
    std::string right_prefix = "snapshot_";

    nlohmann::json to_json() const;
    static std::optional<AsOfJoinSpec> from_json(const nlohmann::json& value,
                                                 std::string* error = nullptr);

    bool operator==(const AsOfJoinSpec&) const = default;
};

const nlohmann::json& as_of_join_spec_schema();

struct AsOfJoinResult {
    std::vector<nlohmann::json> rows;
    std::size_t matched = 0;
    std::size_t unmatched = 0;
    std::size_t future_rows_excluded = 0;
    std::string error;

    bool ok() const noexcept {
        return error.empty();
    }
};

// For each left row, attach the latest right row for the same entity whose
// right time is <= the left row's decision time. Right payload fields are
// prefixed; key fields are reported as `<prefix>entity_id` and `<prefix>as_of`.
AsOfJoinResult as_of_join(const std::vector<nlohmann::json>& left_rows,
                          const std::vector<nlohmann::json>& right_rows,
                          const AsOfJoinSpec& spec);

}  // namespace zetta::solutions
