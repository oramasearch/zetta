#include "zetta/data/metric_store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

#include "zetta/cluster/replication.hpp"
#include "zetta/data/record_store.hpp"

#include "../catalog.hpp"

// Labelled time series (#1231). See include/zetta/data/metric_store.hpp.
namespace zetta::data {
namespace {

using zetta::cluster::ReplicationWrite;

constexpr std::size_t kMaxLabels = 16;
constexpr std::size_t kMaxLabelValueBytes = 256;
constexpr std::size_t kMaxPointsPerQuery = 10'000;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool valid_metric_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    const auto allowed = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' ||
               character == '.' || character == '-';
    };
    return ((name.front() >= 'a' && name.front() <= 'z') ||
            (name.front() >= '0' && name.front() <= '9')) &&
           std::ranges::all_of(name, allowed);
}

bool valid_label_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (!((name.front() >= 'a' && name.front() <= 'z') || name.front() == '_')) return false;
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

std::string definition_key(std::string_view name) {
    return std::string(catalog::kMetricPrefix) + std::string(name);
}

std::string bucket_collection(std::string_view name) {
    return "_metric." + std::string(name);
}

// 16 zero-padded decimal digits: lexicographic order is time order.
std::string padded_bucket(std::int64_t bucket) {
    std::string digits = std::to_string(bucket);
    return std::string(16 - std::min<std::size_t>(16, digits.size()), '0') + digits;
}

std::string series_id(const std::map<std::string, std::string>& labels) {
    const std::string canonical = nlohmann::json(labels).dump();
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a 64
    for (const char character : canonical) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int position = 15; position >= 0; --position) {
        out[static_cast<std::size_t>(position)] = kDigits[hash & 0x0f];
        hash >>= 4;
    }
    return out;
}

struct PointKey {
    std::int64_t bucket = 0;
    std::string series;
    std::string node;
};

std::optional<PointKey> parse_point_key(std::string_view key) {
    const std::size_t first = key.find(':');
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t second = key.find(':', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    PointKey parsed;
    const std::string_view digits = key.substr(0, first);
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed.bucket);
    if (result.ec != std::errc{}) return std::nullopt;
    parsed.series = std::string(key.substr(first + 1, second - first - 1));
    parsed.node = std::string(key.substr(second + 1));
    return parsed;
}

struct Bucket {
    std::map<std::string, std::string> labels;
    double sum = 0.0;
    std::uint64_t count = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double last = 0.0;
    std::int64_t last_at = 0;
};

nlohmann::json bucket_value(const Bucket& bucket) {
    return {{"labels", bucket.labels}, {"sum", bucket.sum},     {"count", bucket.count},
            {"min", bucket.minimum},   {"max", bucket.maximum}, {"last", bucket.last},
            {"last_at", bucket.last_at}};
}

// Non-throwing parse mirroring metric_bucket_schema().
std::optional<Bucket> parse_bucket(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    Bucket bucket;
    const auto number = [&](const char* field, double& out) {
        const auto found = value.find(field);
        if (found == value.end() || !found->is_number()) return false;
        out = found->get<double>();
        return true;
    };
    if (!number("sum", bucket.sum) || !number("min", bucket.minimum) ||
        !number("max", bucket.maximum) || !number("last", bucket.last)) {
        return std::nullopt;
    }
    const auto count = value.find("count");
    const auto last_at = value.find("last_at");
    const auto labels = value.find("labels");
    if (count == value.end() || !count->is_number_unsigned() || last_at == value.end() ||
        !last_at->is_number_integer() || labels == value.end() || !labels->is_object()) {
        return std::nullopt;
    }
    bucket.count = count->get<std::uint64_t>();
    bucket.last_at = last_at->get<std::int64_t>();
    for (const auto& [name, label] : labels->items()) {
        if (!label.is_string()) return std::nullopt;
        bucket.labels[name] = label.get<std::string>();
    }
    return bucket;
}

MetricOutcome failure(MetricStatus status, std::string message, std::string path = {}) {
    MetricOutcome outcome;
    outcome.status = status;
    outcome.message = std::move(message);
    outcome.path = std::move(path);
    return outcome;
}

}  // namespace

std::string_view to_string(MetricKind kind) {
    return kind == MetricKind::Counter ? "counter" : "gauge";
}

std::optional<MetricKind> metric_kind_from_string(std::string_view text) {
    if (text == "counter") return MetricKind::Counter;
    if (text == "gauge") return MetricKind::Gauge;
    return std::nullopt;
}

const nlohmann::json& metric_definition_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.MetricDefinition",
  "description": "A labelled time series (#1231): the replicated `metric:<name>` value. A counter's points are non-negative increments summed over a range; a gauge's points are readings. `resolution_ms` is the bucket width; `retention_ms`, when present, removes buckets older than it; `created` is milliseconds since the Unix epoch.",
  "type": "object",
  "required": ["name", "kind", "resolution_ms", "created"],
  "additionalProperties": false,
  "properties": {
    "name": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_.-]{0,127}$"},
    "kind": {"enum": ["counter", "gauge"]},
    "resolution_ms": {"type": "integer", "minimum": 1000, "maximum": 86400000},
    "retention_ms": {"type": "integer", "minimum": 1},
    "created": {"type": "integer"}
  }
}
)JSONSCHEMA");
    return schema;
}

const nlohmann::json& metric_bucket_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.MetricBucket",
  "description": "One node's fold of one series over one bucket (#1231): the value of key `<bucket start>:<series>:<node>` in the record store's `_metric.<metric>` collection, written only by that node. `sum`, `count`, `min` and `max` cover every point in the bucket; `last` is the latest reading and `last_at` its time in milliseconds since the Unix epoch.",
  "type": "object",
  "required": ["labels", "sum", "count", "min", "max", "last", "last_at"],
  "additionalProperties": false,
  "properties": {
    "labels": {"type": "object", "additionalProperties": {"type": "string"}},
    "sum": {"type": "number"},
    "count": {"type": "integer", "minimum": 0},
    "min": {"type": "number"},
    "max": {"type": "number"},
    "last": {"type": "number"},
    "last_at": {"type": "integer"}
  }
}
)JSONSCHEMA");
    return schema;
}

nlohmann::json to_json(const MetricDefinition& definition) {
    nlohmann::json value{{"name", definition.name},
                         {"kind", to_string(definition.kind)},
                         {"resolution_ms", definition.resolution_ms},
                         {"created", definition.created_ms}};
    if (definition.retention_ms) value["retention_ms"] = *definition.retention_ms;
    return value;
}

std::optional<MetricDefinition> metric_definition_from_json(const nlohmann::json& value,
                                                            std::string* reason) {
    const auto fail = [&](std::string why) -> std::optional<MetricDefinition> {
        if (reason) *reason = std::move(why);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("/: not an object");
    for (const auto& [field, _] : value.items()) {
        if (field != "name" && field != "kind" && field != "resolution_ms" &&
            field != "retention_ms" && field != "created") {
            return fail("/" + field + ": unknown field");
        }
    }
    MetricDefinition definition;
    const auto name = value.find("name");
    if (name == value.end() || !name->is_string() ||
        !valid_metric_name(name->get<std::string>())) {
        return fail("/metric: want a metric name");
    }
    definition.name = name->get<std::string>();
    const auto kind = value.find("kind");
    const auto parsed_kind = kind != value.end() && kind->is_string()
                                 ? metric_kind_from_string(kind->get<std::string>())
                                 : std::nullopt;
    if (!parsed_kind) return fail("/kind: want counter or gauge");
    definition.kind = *parsed_kind;
    const auto resolution = value.find("resolution_ms");
    if (resolution == value.end() || !resolution->is_number_integer() ||
        resolution->get<std::int64_t>() < 1000 ||
        resolution->get<std::int64_t>() > 86'400'000) {
        return fail("/resolution_ms: want 1000 to 86400000");
    }
    definition.resolution_ms = resolution->get<std::int64_t>();
    if (const auto retention = value.find("retention_ms"); retention != value.end()) {
        if (!retention->is_number_integer() || retention->get<std::int64_t>() < 1) {
            return fail("/retention_ms: want a positive integer");
        }
        definition.retention_ms = retention->get<std::int64_t>();
    }
    const auto created = value.find("created");
    if (created == value.end() || !created->is_number_integer()) {
        return fail("/created: want an integer");
    }
    definition.created_ms = created->get<std::int64_t>();
    return definition;
}

MetricStore::MetricStore(zetta::cluster::ReplicationManager& replication,
                         RecordStore& records, std::string node_id)
    : replication_(replication), records_(records), node_id_(std::move(node_id)) {}

std::optional<MetricDefinition> MetricStore::find(const std::string& name) const {
    const auto view = replication_.shared_snapshot();
    const auto found = view->find(definition_key(name));
    if (found == view->end()) return std::nullopt;
    return metric_definition_from_json(*found);
}

MetricOutcome MetricStore::create(const MetricDefinition& requested) {
    MetricDefinition definition = requested;
    if (definition.created_ms == 0) definition.created_ms = now_ms();
    std::string reason;
    if (!metric_definition_from_json(to_json(definition), &reason)) {
        const std::size_t colon = reason.find(':');
        return failure(MetricStatus::InvalidInput, reason,
                       colon == std::string::npos ? "" : reason.substr(0, colon));
    }
    std::optional<MetricDefinition> existing;
    bool conflict = false;
    const bool created = replication_.update(
        definition_key(definition.name),
        [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
            if (current) {
                existing = metric_definition_from_json(*current);
                conflict = !existing || existing->kind != definition.kind ||
                           existing->resolution_ms != definition.resolution_ms ||
                           existing->retention_ms != definition.retention_ms;
                return std::nullopt;
            }
            return std::vector<ReplicationWrite>{
                {definition_key(definition.name), to_json(definition)}};
        });
    if (conflict) {
        MetricOutcome outcome = failure(
            MetricStatus::Conflict,
            "metric '" + definition.name + "' already exists with a different definition",
            "/metric");
        outcome.definition = existing;
        return outcome;
    }
    MetricOutcome outcome;
    outcome.created = created;
    outcome.definition = created ? definition : existing;
    return outcome;
}

MetricOutcome MetricStore::record(const std::string& metric,
                                  const std::vector<MetricPoint>& points) {
    const std::optional<MetricDefinition> definition = find(metric);
    if (!definition) {
        return failure(MetricStatus::NotFound, "no metric '" + metric + "'", "/metric");
    }
    // Validate every point before writing any, so a batch is all or nothing.
    for (std::size_t position = 0; position < points.size(); ++position) {
        const MetricPoint& point = points[position];
        const std::string at = "/points/" + std::to_string(position);
        if (point.labels.size() > kMaxLabels) {
            return failure(MetricStatus::InvalidInput, "at most 16 labels", at + "/labels");
        }
        for (const auto& [name, value] : point.labels) {
            if (!valid_label_name(name) || value.size() > kMaxLabelValueBytes) {
                return failure(MetricStatus::InvalidInput,
                               "want label names like [a-z_][a-z0-9_]* and values of at "
                               "most 256 bytes",
                               at + "/labels/" + name);
            }
        }
        if (!std::isfinite(point.value) ||
            (definition->kind == MetricKind::Counter && point.value < 0)) {
            return failure(MetricStatus::InvalidInput,
                           definition->kind == MetricKind::Counter
                               ? "a counter increment must be a non-negative number"
                               : "a gauge reading must be a finite number",
                           at + "/value");
        }
        if (point.at_ms && *point.at_ms < 0) {
            return failure(MetricStatus::InvalidInput, "want a time after the epoch",
                           at + "/at_ms");
        }
    }
    const std::int64_t now = now_ms();
    for (const MetricPoint& point : points) {
        const std::int64_t at = point.at_ms.value_or(now);
        const std::int64_t bucket = at - at % definition->resolution_ms;
        const std::string key =
            padded_bucket(bucket) + ":" + series_id(point.labels) + ":" + node_id_;
        std::optional<std::int64_t> expires;
        if (definition->retention_ms) {
            expires = bucket + definition->resolution_ms + *definition->retention_ms;
        }
        records_.fold(
            bucket_collection(metric), key,
            [&](const std::string* current) -> std::optional<std::string> {
                Bucket folded;
                std::optional<Bucket> existing;
                if (current) {
                    existing = parse_bucket(nlohmann::json::parse(*current, nullptr, false));
                }
                if (existing) {
                    folded = *existing;
                    folded.minimum = std::min(folded.minimum, point.value);
                    folded.maximum = std::max(folded.maximum, point.value);
                } else {
                    folded.labels = point.labels;
                    folded.minimum = point.value;
                    folded.maximum = point.value;
                    folded.last_at = std::numeric_limits<std::int64_t>::min();
                }
                folded.sum += point.value;
                ++folded.count;
                if (at >= folded.last_at) {
                    folded.last = point.value;
                    folded.last_at = at;
                }
                return bucket_value(folded).dump();
            },
            expires);
    }
    MetricOutcome outcome;
    outcome.definition = definition;
    outcome.recorded = points.size();
    return outcome;
}

MetricOutcome MetricStore::query(const MetricQuery& query) const {
    const std::optional<MetricDefinition> definition = find(query.metric);
    if (!definition) {
        return failure(MetricStatus::NotFound, "no metric '" + query.metric + "'", "/metric");
    }
    if (query.from_ms < 0 || query.to_ms <= query.from_ms) {
        return failure(MetricStatus::InvalidInput, "want 0 <= from_ms < to_ms", "/to_ms");
    }
    const std::int64_t resolution = definition->resolution_ms;
    const std::int64_t from = query.from_ms - query.from_ms % resolution;
    const std::int64_t step = query.step_ms.value_or(query.to_ms - from);
    if (query.step_ms && (*query.step_ms < resolution || *query.step_ms % resolution != 0)) {
        return failure(MetricStatus::InvalidInput,
                       "step_ms must be a multiple of the metric's resolution (" +
                           std::to_string(resolution) + " ms)",
                       "/step_ms");
    }
    if ((query.to_ms - from + step - 1) / step > static_cast<std::int64_t>(kMaxPointsPerQuery)) {
        return failure(MetricStatus::InvalidInput, "at most 10000 steps per query", "/step_ms");
    }
    std::string aggregate = query.aggregate;
    if (aggregate.empty()) aggregate = definition->kind == MetricKind::Counter ? "sum" : "last";
    const bool known = aggregate == "sum" || aggregate == "last" || aggregate == "avg" ||
                       aggregate == "min" || aggregate == "max";
    if (!known || (definition->kind == MetricKind::Counter && aggregate != "sum")) {
        return failure(MetricStatus::InvalidInput,
                       definition->kind == MetricKind::Counter
                           ? "a counter aggregates by sum"
                           : "want last, sum, avg, min or max",
                       "/aggregate");
    }

    // Per group, per step (and once for the whole range, step -1): the fold of
    // every bucket in it; for `last`, the latest reading per series.
    struct Fold {
        double sum = 0.0;
        std::uint64_t count = 0;
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
        std::map<std::string, std::pair<std::int64_t, double>> last;  // series -> (at, value)
    };
    std::map<std::map<std::string, std::string>, std::map<std::int64_t, Fold>> groups;
    const auto fold_into = [](Fold& fold, const std::string& series, const Bucket& bucket) {
        fold.sum += bucket.sum;
        fold.count += bucket.count;
        fold.minimum = std::min(fold.minimum, bucket.minimum);
        fold.maximum = std::max(fold.maximum, bucket.maximum);
        auto [entry, inserted] = fold.last.try_emplace(series, bucket.last_at, bucket.last);
        if (!inserted && bucket.last_at > entry->second.first) {
            entry->second = {bucket.last_at, bucket.last};
        }
    };

    records_.scan(bucket_collection(query.metric), padded_bucket(from),
                  [&](const std::string& entry_key, const std::string& entry_value) {
                      const auto key = parse_point_key(entry_key);
                      if (!key) return true;
                      if (key->bucket >= query.to_ms) return false;
                      const auto bucket =
                          parse_bucket(nlohmann::json::parse(entry_value, nullptr, false));
                      if (!bucket) return true;
                      const bool matches = std::ranges::all_of(query.labels, [&](const auto& wanted) {
                          const auto found = bucket->labels.find(wanted.first);
                          return found != bucket->labels.end() && found->second == wanted.second;
                      });
                      if (!matches) return true;
                      std::map<std::string, std::string> group;
                      for (const std::string& name : query.group_by) {
                          if (const auto found = bucket->labels.find(name);
                              found != bucket->labels.end()) {
                              group[name] = found->second;
                          }
                      }
                      auto& steps = groups[group];
                      const std::int64_t step_start = from + (key->bucket - from) / step * step;
                      fold_into(steps[step_start], key->series, *bucket);
                      fold_into(steps[-1], key->series, *bucket);
                      return true;
                  });

    const auto value_of = [&](const Fold& fold) {
        if (aggregate == "sum") return fold.sum;
        if (aggregate == "avg") return fold.count == 0 ? 0.0 : fold.sum / static_cast<double>(fold.count);
        if (aggregate == "min") return fold.minimum;
        if (aggregate == "max") return fold.maximum;
        double total = 0.0;  // last: the latest reading of each series, summed
        for (const auto& [series, reading] : fold.last) total += reading.second;
        return total;
    };
    MetricOutcome outcome;
    outcome.definition = definition;
    for (const auto& [labels, steps] : groups) {
        MetricSeriesResult result;
        result.labels = labels;
        for (const auto& [step_start, fold] : steps) {
            if (step_start < 0) {
                result.total = value_of(fold);
            } else {
                result.points.emplace_back(step_start, value_of(fold));
            }
        }
        outcome.series.push_back(std::move(result));
    }
    return outcome;
}

}  // namespace zetta::data
