#include "zetta/cluster/node_telemetry.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::cluster {
namespace {

// The draft-07 schema for a `node.metrics` payload, embedded + compiled once at
// runtime — the same pattern as the signal envelope / CollectiveMessage payload
// (see src/core/collectives.cpp). A closed object: `node_id` + `sampled_at` are
// required identity; every gauge field is optional, so an unavailable counter is
// carried by **omission** (graceful degradation round-trips as field presence).
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.NodeMetrics",
      "type": "object",
      "properties": {
        "node_id":              { "type": "string", "minLength": 1 },
        "sampled_at":           { "type": "string" },
        "cpu_cores":            { "type": "integer", "minimum": 0 },
        "cpu_percent":          { "type": "number" },
        "memory_used_bytes":    { "type": "integer", "minimum": 0 },
        "memory_total_bytes":   { "type": "integer", "minimum": 0 },
        "memory_percent":       { "type": "number" },
        "process_rss_bytes":    { "type": "integer", "minimum": 0 },
        "process_peak_rss_bytes": { "type": "integer", "minimum": 0 },
        "allocator_in_use_bytes": { "type": "integer", "minimum": 0 },
        "allocator_free_held_bytes": { "type": "integer", "minimum": 0 },
        "attributed_memory_bytes": { "type": "integer", "minimum": 0 },
        "unattributed_memory_bytes": { "type": "integer", "minimum": 0 },
        "memory_explained_percent": { "type": "number" },
        "gpu_percent":          { "type": "number" },
        "vram_used_bytes":      { "type": "integer", "minimum": 0 },
        "vram_total_bytes":     { "type": "integer", "minimum": 0 },
        "storage_used_bytes":   { "type": "integer", "minimum": 0 },
        "storage_total_bytes":  { "type": "integer", "minimum": 0 },
        "storage_percent":      { "type": "number" },
        "net_rx_bytes_per_sec": { "type": "integer", "minimum": 0 },
        "net_tx_bytes_per_sec": { "type": "integer", "minimum": 0 },
        "net_percent":          { "type": "number" },
        "storage_census": {
          "type": "object",
          "properties": {
            "measured_at":     { "type": "string", "minLength": 1 },
            "age_seconds":     { "type": "number", "minimum": 0 },
            "total_bytes":     { "type": "integer", "minimum": 0 },
            "kinds": {
              "type": "object",
              "additionalProperties": { "type": "integer", "minimum": 0 }
            },
            "buckets": {
              "type": "array",
              "maxItems": 256,
              "items": {
                "type": "object",
                "properties": {
                  "bucket": { "type": "string" },
                  "bytes":  { "type": "integer", "minimum": 0 }
                },
                "required": ["bucket", "bytes"],
                "additionalProperties": false
              }
            },
            "buckets_omitted": { "type": "integer", "minimum": 0 }
          },
          "required": ["measured_at", "age_seconds", "total_bytes", "kinds", "buckets"],
          "additionalProperties": false
        }
      },
      "required": ["node_id", "sampled_at"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure without letting it cross the validator
// boundary as an exception (matching src/core/collectives.cpp).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

void set_error(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

}  // namespace

const nlohmann::json& node_metrics_wire_schema() { return schema_document(); }

std::string encode_node_metrics(const NodeMetrics& metrics,
                                const std::string& node_id) {
    nlohmann::json document;
    document["node_id"] = node_id;
    document["sampled_at"] = metrics.sampled_at;
    if (metrics.cpu_cores > 0) document["cpu_cores"] = metrics.cpu_cores;
    // Each gauge group is emitted only when available — an unreportable counter is
    // omitted, not zeroed, so the receiver reconstructs the same n/a state.
    if (metrics.cpu_percent_available) {
        document["cpu_percent"] = metrics.cpu_percent;
    }
    if (metrics.memory_available) {
        document["memory_used_bytes"] = metrics.memory_used_bytes;
        document["memory_total_bytes"] = metrics.memory_total_bytes;
        document["memory_percent"] = metrics.memory_percent;
    }
    if (metrics.process_memory_available) {
        document["process_rss_bytes"] = metrics.process_rss_bytes;
        document["process_peak_rss_bytes"] = metrics.process_peak_rss_bytes;
    }
    if (metrics.allocator_memory_available) {
        document["allocator_in_use_bytes"] = metrics.allocator_in_use_bytes;
        document["allocator_free_held_bytes"] = metrics.allocator_free_held_bytes;
        document["attributed_memory_bytes"] = metrics.attributed_memory_bytes;
        document["unattributed_memory_bytes"] = metrics.unattributed_memory_bytes;
        document["memory_explained_percent"] = metrics.memory_explained_percent;
    }
    if (metrics.gpu_available) {
        document["gpu_percent"] = metrics.gpu_percent;
        document["vram_used_bytes"] = metrics.vram_used_bytes;
        document["vram_total_bytes"] = metrics.vram_total_bytes;
    }
    if (metrics.storage_available) {
        document["storage_used_bytes"] = metrics.storage_used_bytes;
        document["storage_total_bytes"] = metrics.storage_total_bytes;
        document["storage_percent"] = metrics.storage_percent;
    }
    if (metrics.network_available) {
        document["net_rx_bytes_per_sec"] = metrics.net_rx_bytes_per_sec;
        document["net_tx_bytes_per_sec"] = metrics.net_tx_bytes_per_sec;
    }
    if (metrics.net_utilization_available) {
        document["net_percent"] = metrics.net_percent;
    }
    if (metrics.storage_census_available) {
        // The largest buckets by name, at most kMaxWireCensusBuckets; the rest are counted, and
        // their bytes stay in kinds.buckets and total_bytes.
        std::vector<std::pair<std::string, std::uint64_t>> buckets(
            metrics.storage_census.buckets.begin(), metrics.storage_census.buckets.end());
        std::ranges::stable_sort(buckets, std::ranges::greater{},
                                 &std::pair<std::string, std::uint64_t>::second);
        const std::size_t listed = std::min(buckets.size(), kMaxWireCensusBuckets);
        nlohmann::json listed_buckets = nlohmann::json::array();
        for (std::size_t index = 0; index < listed; ++index) {
            listed_buckets.push_back(
                {{"bucket", buckets[index].first}, {"bytes", buckets[index].second}});
        }
        nlohmann::json census{
            {"measured_at", metrics.storage_census_measured_at},
            {"age_seconds", std::max(0.0, metrics.storage_census_age_seconds)},
            {"total_bytes", metrics.storage_census.total_bytes},
            {"kinds", metrics.storage_census.kinds},
            {"buckets", std::move(listed_buckets)}};
        const std::uint64_t omitted =
            metrics.storage_census_buckets_omitted + (buckets.size() - listed);
        if (omitted > 0) census["buckets_omitted"] = omitted;
        document["storage_census"] = std::move(census);
    }
    return document.dump();
}

std::optional<NodeTelemetrySample> decode_node_metrics(std::string_view payload,
                                                       std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "node metrics is not valid JSON");
        return std::nullopt;
    }

    collecting_error_handler handler;
    schema_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    NodeTelemetrySample sample;
    sample.node_id = document.value("node_id", std::string{});
    NodeMetrics& metrics = sample.metrics;
    metrics.sampled_at = document.value("sampled_at", std::string{});
    metrics.cpu_cores = document.value("cpu_cores", 0u);
    // A gauge group is available exactly when its fields are present (the encode
    // boundary's omission rule, in reverse).
    if (document.contains("cpu_percent")) {
        metrics.cpu_percent_available = true;
        metrics.cpu_percent = document.at("cpu_percent").get<double>();
    }
    if (document.contains("memory_used_bytes")) {
        metrics.memory_available = true;
        metrics.memory_used_bytes =
            document.at("memory_used_bytes").get<std::uint64_t>();
        metrics.memory_total_bytes =
            document.value("memory_total_bytes", std::uint64_t{0});
        metrics.memory_percent = document.value("memory_percent", 0.0);
    }
    if (document.contains("process_rss_bytes")) {
        metrics.process_memory_available = true;
        metrics.process_rss_bytes =
            document.at("process_rss_bytes").get<std::uint64_t>();
        metrics.process_peak_rss_bytes =
            document.value("process_peak_rss_bytes", std::uint64_t{0});
    }
    if (document.contains("allocator_in_use_bytes")) {
        metrics.allocator_memory_available = true;
        metrics.allocator_in_use_bytes =
            document.at("allocator_in_use_bytes").get<std::uint64_t>();
        metrics.allocator_free_held_bytes =
            document.value("allocator_free_held_bytes", std::uint64_t{0});
        metrics.attributed_memory_bytes =
            document.value("attributed_memory_bytes", std::uint64_t{0});
        metrics.unattributed_memory_bytes =
            document.value("unattributed_memory_bytes", std::uint64_t{0});
        metrics.memory_explained_percent =
            document.value("memory_explained_percent", 0.0);
    }
    if (document.contains("gpu_percent")) {
        metrics.gpu_available = true;
        metrics.gpu_percent = document.at("gpu_percent").get<double>();
        metrics.vram_used_bytes =
            document.value("vram_used_bytes", std::uint64_t{0});
        metrics.vram_total_bytes =
            document.value("vram_total_bytes", std::uint64_t{0});
    }
    if (document.contains("storage_used_bytes")) {
        metrics.storage_available = true;
        metrics.storage_used_bytes =
            document.at("storage_used_bytes").get<std::uint64_t>();
        metrics.storage_total_bytes =
            document.value("storage_total_bytes", std::uint64_t{0});
        metrics.storage_percent = document.value("storage_percent", 0.0);
    }
    if (document.contains("net_rx_bytes_per_sec")) {
        metrics.network_available = true;
        metrics.net_rx_bytes_per_sec =
            document.at("net_rx_bytes_per_sec").get<std::uint64_t>();
        metrics.net_tx_bytes_per_sec =
            document.value("net_tx_bytes_per_sec", std::uint64_t{0});
    }
    if (document.contains("net_percent")) {
        metrics.net_utilization_available = true;
        metrics.net_percent = document.at("net_percent").get<double>();
    }
    if (document.contains("storage_census")) {
        const nlohmann::json& census = document.at("storage_census");
        metrics.storage_census_available = true;
        metrics.storage_census_measured_at = census.at("measured_at").get<std::string>();
        metrics.storage_census_age_seconds = census.at("age_seconds").get<double>();
        metrics.storage_census.total_bytes = census.at("total_bytes").get<std::uint64_t>();
        for (const auto& [kind, bytes] : census.at("kinds").items()) {
            metrics.storage_census.kinds[kind] = bytes.get<std::uint64_t>();
        }
        for (const nlohmann::json& bucket : census.at("buckets")) {
            metrics.storage_census.buckets[bucket.at("bucket").get<std::string>()] +=
                bucket.at("bytes").get<std::uint64_t>();
        }
        metrics.storage_census_buckets_omitted =
            census.value("buckets_omitted", std::uint64_t{0});
    }
    return sample;
}

ClusterStorageCensus fold_storage_census(const std::vector<std::string>& members,
                                         const std::vector<NodeTelemetrySample>& samples,
                                         std::chrono::duration<double> stale_after) {
    ClusterStorageCensus folded;
    for (const std::string_view kind : zetta::data::storage_kinds()) {
        folded.total.kinds[std::string(kind)] = 0;
    }
    std::map<std::string, const NodeTelemetrySample*> sampled;
    for (const NodeTelemetrySample& sample : samples) sampled[sample.node_id] = &sample;
    std::set<std::string> everyone(members.begin(), members.end());
    for (const auto& [node_id, sample] : sampled) everyone.insert(node_id);

    for (const std::string& node_id : everyone) {  // std::set → node-id ascending
        if (node_id.empty()) continue;
        const auto found = sampled.find(node_id);
        if (found == sampled.end()) {
            folded.missing.push_back({node_id, "not_reporting"});
            continue;
        }
        const NodeTelemetrySample& sample = *found->second;
        if (!sample.metrics.storage_census_available) {
            folded.missing.push_back({node_id, "not_measured"});
            continue;
        }
        const double age = sample.metrics.storage_census_age_seconds + sample.age_seconds;
        if (age >= stale_after.count()) {
            folded.missing.push_back({node_id, "stale"});
            continue;
        }
        const zetta::data::StorageCensus& census = sample.metrics.storage_census;
        folded.total.total_bytes += census.total_bytes;
        for (const auto& [kind, bytes] : census.kinds) folded.total.kinds[kind] += bytes;
        for (const auto& [bucket, bytes] : census.buckets) folded.total.buckets[bucket] += bytes;
        folded.nodes.push_back({node_id, sample.is_local, census,
                                sample.metrics.storage_census_measured_at, age,
                                sample.metrics.storage_census_buckets_omitted});
    }
    return folded;
}

void NodeTelemetryExchange::record_local(const std::string& node_id,
                                         const NodeMetrics& metrics) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[node_id];
    entry.metrics = metrics;
    entry.received = std::chrono::steady_clock::now();
    entry.is_local = true;
}

void NodeTelemetryExchange::on_event(const Signal& event) {
    if (event.method != kMethod) return;
    std::optional<NodeTelemetrySample> sample = decode_node_metrics(event.payload);
    if (!sample) return;  // malformed payload — drop it (boundary already logged)
    // Key by the transport-verified sender id (Signal::from), not the payload's
    // self-reported node_id, so a peer can't masquerade as another node.
    const std::string& key = event.from.empty() ? sample->node_id : event.from;
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[key];
    entry.metrics = std::move(sample->metrics);
    entry.received = std::chrono::steady_clock::now();
    entry.is_local = false;
}

std::vector<NodeTelemetrySample> NodeTelemetryExchange::snapshot(
    std::chrono::duration<double> ttl) const {
    const auto now = std::chrono::steady_clock::now();
    std::vector<NodeTelemetrySample> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [node_id, entry] : entries_) {  // std::map → node-id ascending
        const std::chrono::duration<double> age = now - entry.received;
        // Stale peer — drop it so its gauges don't dangle. The comparison is `>=`,
        // not `>`, so the window is half-open: an entry is kept only while its age is
        // strictly *inside* it. That makes `ttl == 0` mean "keep nothing", which is the
        // only reading of a zero-length freshness window that says anything; with `>`
        // it instead meant "keep whatever was recorded in this same clock tick", so the
        // result depended on whether the steady clock happened to advance between the
        // record and the snapshot (#834). No caller passes a ttl an entry can land on
        // exactly other than 0, so nothing else changes.
        if (age >= ttl) continue;
        NodeTelemetrySample sample;
        sample.node_id = node_id;
        sample.metrics = entry.metrics;
        sample.age_seconds = age.count();
        sample.is_local = entry.is_local;
        result.push_back(std::move(sample));
    }
    return result;
}

}  // namespace zetta::cluster
