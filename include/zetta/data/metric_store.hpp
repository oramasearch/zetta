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
namespace zetta::data {
class RecordStore;
}

// Labelled time series (#1231, docs/data/time-series.md): counters and gauges,
// appended with labels and read back summed over a time range. Built for lake's
// metering (lake#348) and request metrics (lake#384).
//
// A metric is declared once in the replicated catalogue (`metric:<name>`). Its
// buckets live in the record store's node-local log, in the engine-internal
// collection `_metric.<name>`: each node folds the points it receives into keys
// only IT writes — `<bucket start>:<series>:<node>` — so recording never
// conflicts, and anti-entropy brings every node's buckets to every other node. A
// key orders by time first, so a range query walks exactly its buckets. Retention
// is the bucket record's expiry.
//
// A bucket is not fsynced per point (the log flushes every tick), and another
// node's points become visible within one anti-entropy round. Records that must
// be exact belong in the record store.
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
const nlohmann::json& metric_bucket_schema();      // one bucket in `_metric.<name>`

nlohmann::json to_json(const MetricDefinition& definition);
std::optional<MetricDefinition> metric_definition_from_json(const nlohmann::json& value,
                                                            std::string* reason = nullptr);

// Thread-safe: definitions are in the replicated catalogue, buckets in the
// record store's log.
class MetricStore {
public:
    MetricStore(zetta::cluster::ReplicationManager& replication, RecordStore& records,
                std::string node_id);

    MetricOutcome create(const MetricDefinition& definition);
    std::optional<MetricDefinition> find(const std::string& name) const;
    MetricOutcome record(const std::string& metric, const std::vector<MetricPoint>& points);
    MetricOutcome query(const MetricQuery& query) const;

private:
    zetta::cluster::ReplicationManager& replication_;
    RecordStore& records_;
    std::string node_id_;
};

}  // namespace zetta::data
