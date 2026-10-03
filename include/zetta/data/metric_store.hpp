#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cluster {
class ReplicationManager;
}

// Labelled time series (#1231, docs/data/time-series.md): counters and gauges,
// appended with labels and read back summed over a time range. Built for lake's
// metering (lake#348) and request metrics (lake#384).
//
// A metric is declared once (`metric:<name>`). Each node folds the points it
// receives into fixed-width buckets under keys only IT writes —
// `metric_point:<name>:<bucket start>:<series>:<node>` — so concurrent recording
// on several nodes never conflicts and a query sums every node's buckets from the
// replicated catalogue. A bucket key orders by time first, so a range query walks
// exactly the buckets in its range (#1227: one key range of the shared snapshot).
//
// Points are not journaled: a crash may lose the buckets written in the last
// second before the catalogue was saved, and another node's points become visible
// within one gossip interval. Records that must be exact belong in the record store.
namespace zetta::data {

enum class MetricKind { Counter, Gauge };

std::string_view to_string(MetricKind kind);
std::optional<MetricKind> metric_kind_from_string(std::string_view text);

struct MetricDefinition {
    std::string name;
    MetricKind kind = MetricKind::Counter;
    std::int64_t resolution_ms = 60'000;        // bucket width
    std::optional<std::int64_t> retention_ms;   // buckets older than this are removed
    std::int64_t created_ms = 0;
};

struct MetricPoint {
    std::map<std::string, std::string> labels;
    double value = 0.0;
    std::optional<std::int64_t> at_ms;  // default: now
};

struct MetricQuery {
    std::string metric;
    std::int64_t from_ms = 0;  // inclusive, rounded down to a bucket
    std::int64_t to_ms = 0;    // exclusive
    std::map<std::string, std::string> labels;  // only series carrying these labels
    std::vector<std::string> group_by;          // labels a result series keeps
    std::optional<std::int64_t> step_ms;        // a multiple of the resolution; default one step
    std::string aggregate;  // counter: "sum"; gauge: "last" (default), "sum", "avg", "min", "max"
};

struct MetricSeriesResult {
    std::map<std::string, std::string> labels;           // the group_by labels
    double total = 0.0;                                   // the aggregate over the range
    std::vector<std::pair<std::int64_t, double>> points;  // (step start, aggregate)
};

enum class MetricStatus { Ok, InvalidInput, NotFound, Conflict };

struct MetricOutcome {
    MetricStatus status = MetricStatus::Ok;
    std::string message;
    std::string path;
    std::optional<MetricDefinition> definition;
    bool created = false;
    std::size_t recorded = 0;
    std::vector<MetricSeriesResult> series;

    bool ok() const { return status == MetricStatus::Ok; }
};

const nlohmann::json& metric_definition_schema();  // metric:<name>
const nlohmann::json& metric_bucket_schema();      // metric_point:...

nlohmann::json to_json(const MetricDefinition& definition);
std::optional<MetricDefinition> metric_definition_from_json(const nlohmann::json& value,
                                                            std::string* reason = nullptr);

// Thread-safe: every method works on the replicated catalogue only.
class MetricStore {
public:
    MetricStore(zetta::cluster::ReplicationManager& replication, std::string node_id);

    MetricOutcome create(const MetricDefinition& definition);
    std::optional<MetricDefinition> find(const std::string& name) const;
    MetricOutcome record(const std::string& metric, const std::vector<MetricPoint>& points);
    MetricOutcome query(const MetricQuery& query) const;

    // Remove this node's buckets older than each metric's retention.
    void sweep(std::int64_t now_ms);

private:
    zetta::cluster::ReplicationManager& replication_;
    std::string node_id_;
};

}  // namespace zetta::data
