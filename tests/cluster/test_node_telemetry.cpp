#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "zetta/cluster/node_metrics.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/cluster/signal.hpp"

// The cluster-wide live-telemetry exchange over the signal plane (#584): the
// `node.metrics` wire boundary (encode/decode + schema validation) and the
// NodeTelemetryExchange cache (record this node, receive peers, expire stale).

namespace {

zetta::cluster::NodeMetrics make_full_sample() {
    zetta::cluster::NodeMetrics metrics;
    metrics.sampled_at = "2026-06-30T12:00:00Z";
    metrics.cpu_cores = 8;
    metrics.cpu_percent_available = true;
    metrics.cpu_percent = 41.5;
    metrics.memory_available = true;
    metrics.memory_used_bytes = 12ull * 1024 * 1024 * 1024;
    metrics.memory_total_bytes = 32ull * 1024 * 1024 * 1024;
    metrics.memory_percent = 37.5;
    metrics.process_memory_available = true;
    metrics.process_rss_bytes = 812ull * 1024 * 1024;
    metrics.process_peak_rss_bytes = 900ull * 1024 * 1024;
    metrics.allocator_memory_available = true;
    metrics.allocator_in_use_bytes = 790ull * 1024 * 1024;
    metrics.allocator_free_held_bytes = 22ull * 1024 * 1024;
    metrics.attributed_memory_bytes = 750ull * 1024 * 1024;
    metrics.unattributed_memory_bytes = 40ull * 1024 * 1024;
    metrics.memory_explained_percent = 94.9367;
    metrics.storage_available = true;
    metrics.storage_used_bytes = 210ull * 1024 * 1024 * 1024;
    metrics.storage_total_bytes = 500ull * 1024 * 1024 * 1024;
    metrics.storage_percent = 42.0;
    metrics.network_available = true;
    metrics.net_rx_bytes_per_sec = 1500000;
    metrics.net_tx_bytes_per_sec = 320000;
    // GPU + net utilization deliberately left unavailable (CPU-only host).
    return metrics;
}

}  // namespace

TEST(NodeTelemetryWire, RoundTripsAvailableGroups) {
    const zetta::cluster::NodeMetrics original = make_full_sample();
    const std::string payload = zetta::cluster::encode_node_metrics(original, "node-a");

    std::string error;
    const auto decoded = zetta::cluster::decode_node_metrics(payload, &error);
    ASSERT_TRUE(decoded.has_value()) << error;
    EXPECT_EQ(decoded->node_id, "node-a");

    const zetta::cluster::NodeMetrics& m = decoded->metrics;
    EXPECT_EQ(m.sampled_at, original.sampled_at);
    EXPECT_EQ(m.cpu_cores, 8u);
    EXPECT_TRUE(m.cpu_percent_available);
    EXPECT_DOUBLE_EQ(m.cpu_percent, 41.5);
    EXPECT_TRUE(m.memory_available);
    EXPECT_EQ(m.memory_used_bytes, original.memory_used_bytes);
    EXPECT_EQ(m.memory_total_bytes, original.memory_total_bytes);
    EXPECT_DOUBLE_EQ(m.memory_percent, 37.5);
    EXPECT_TRUE(m.process_memory_available);
    EXPECT_EQ(m.process_rss_bytes, original.process_rss_bytes);
    EXPECT_TRUE(m.allocator_memory_available);
    EXPECT_EQ(m.allocator_in_use_bytes, original.allocator_in_use_bytes);
    EXPECT_EQ(m.unattributed_memory_bytes,
              original.unattributed_memory_bytes);
    EXPECT_DOUBLE_EQ(m.memory_explained_percent,
                     original.memory_explained_percent);
    EXPECT_TRUE(m.storage_available);
    EXPECT_EQ(m.storage_used_bytes, original.storage_used_bytes);
    EXPECT_DOUBLE_EQ(m.storage_percent, 42.0);
    EXPECT_TRUE(m.network_available);
    EXPECT_EQ(m.net_rx_bytes_per_sec, 1500000u);
    EXPECT_EQ(m.net_tx_bytes_per_sec, 320000u);
}

TEST(NodeTelemetryWire, UnavailableGroupsDegradeGracefully) {
    // A host with only CPU cores known (no CPU% delta yet, no mem/gpu/storage/net):
    // the unavailable groups are omitted on the wire and decode back as n/a, never
    // a misleading zero.
    zetta::cluster::NodeMetrics sparse;
    sparse.sampled_at = "2026-06-30T12:00:01Z";
    sparse.cpu_cores = 4;

    const auto decoded =
        zetta::cluster::decode_node_metrics(zetta::cluster::encode_node_metrics(sparse, "node-b"));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->metrics.cpu_cores, 4u);
    EXPECT_FALSE(decoded->metrics.cpu_percent_available);
    EXPECT_FALSE(decoded->metrics.memory_available);
    EXPECT_FALSE(decoded->metrics.process_memory_available);
    EXPECT_FALSE(decoded->metrics.allocator_memory_available);
    EXPECT_FALSE(decoded->metrics.gpu_available);
    EXPECT_FALSE(decoded->metrics.storage_available);
    EXPECT_FALSE(decoded->metrics.network_available);
    EXPECT_FALSE(decoded->metrics.net_utilization_available);
}

TEST(NodeTelemetryWire, RejectsMalformedPayloadAtBoundary) {
    std::string error;
    EXPECT_FALSE(zetta::cluster::decode_node_metrics("{not json", &error).has_value());
    EXPECT_FALSE(error.empty());

    // Missing required node_id.
    EXPECT_FALSE(
        zetta::cluster::decode_node_metrics(R"({"sampled_at":"t"})").has_value());
    // Wrong type for a gauge (string where a number is required).
    EXPECT_FALSE(
        zetta::cluster::decode_node_metrics(
            R"({"node_id":"n","sampled_at":"t","cpu_percent":"hot"})")
            .has_value());
    // Unknown field (closed object).
    EXPECT_FALSE(
        zetta::cluster::decode_node_metrics(
            R"({"node_id":"n","sampled_at":"t","mystery":1})")
            .has_value());
}

TEST(NodeTelemetryExchange, RecordsLocalAndReceivesPeers) {
    zetta::cluster::NodeTelemetryExchange exchange;

    // The local node records its own sample.
    exchange.record_local("node-a", make_full_sample());

    // A peer's broadcast arrives as a node.metrics event (keyed by Signal::from).
    zetta::cluster::NodeMetrics peer = make_full_sample();
    peer.cpu_percent = 88.0;
    zetta::cluster::Signal event;
    event.kind = zetta::cluster::SignalKind::Event;
    event.method = std::string(zetta::cluster::NodeTelemetryExchange::kMethod);
    event.from = "node-b";
    event.payload = zetta::cluster::encode_node_metrics(peer, "node-b");
    exchange.on_event(event);

    const auto snapshot = exchange.snapshot();
    ASSERT_EQ(snapshot.size(), 2u);
    // std::map ordering → node-a then node-b.
    EXPECT_EQ(snapshot[0].node_id, "node-a");
    EXPECT_TRUE(snapshot[0].is_local);
    EXPECT_EQ(snapshot[1].node_id, "node-b");
    EXPECT_FALSE(snapshot[1].is_local);
    EXPECT_DOUBLE_EQ(snapshot[1].metrics.cpu_percent, 88.0);
    EXPECT_GE(snapshot[0].age_seconds, 0.0);
}

TEST(NodeTelemetryExchange, IgnoresOtherEventsAndMalformedPayloads) {
    zetta::cluster::NodeTelemetryExchange exchange;

    zetta::cluster::Signal other;
    other.kind = zetta::cluster::SignalKind::Event;
    other.method = "some.other.event";
    other.from = "node-x";
    other.payload = "whatever";
    exchange.on_event(other);  // wrong method → ignored

    zetta::cluster::Signal bad;
    bad.kind = zetta::cluster::SignalKind::Event;
    bad.method = std::string(zetta::cluster::NodeTelemetryExchange::kMethod);
    bad.from = "node-y";
    bad.payload = "{not valid";
    exchange.on_event(bad);  // malformed payload → dropped at the boundary

    EXPECT_TRUE(exchange.snapshot().empty());
}

TEST(NodeTelemetryExchange, ExpiresStaleEntries) {
    zetta::cluster::NodeTelemetryExchange exchange;
    exchange.record_local("node-a", make_full_sample());

    // The freshness window is half-open — kept while `age < ttl`, dropped at
    // `age >= ttl` — so a zero-length window keeps nothing and a generous one keeps
    // the entry. Both hold for *any* age the steady clock can report, including the
    // 0 you get when the record and the snapshot land in the same tick. Under `>`
    // the first assertion was a coin flip on exactly that (#834).
    EXPECT_TRUE(exchange.snapshot(std::chrono::duration<double>(0.0)).empty());
    EXPECT_EQ(exchange.snapshot(std::chrono::hours(1)).size(), 1u);

    // The window is evaluated per entry against its own age, not against the cache
    // as a whole: a second entry recorded later is no fresher than zero either.
    exchange.record_local("node-b", make_full_sample());
    EXPECT_TRUE(exchange.snapshot(std::chrono::duration<double>(0.0)).empty());
    EXPECT_EQ(exchange.snapshot(std::chrono::hours(1)).size(), 2u);

    // A negative window is degenerate but must not resurrect anything.
    EXPECT_TRUE(exchange.snapshot(std::chrono::duration<double>(-1.0)).empty());
}

// --- The storage census in the exchange (zetta#1259) -------------------------

namespace {

zetta::cluster::NodeMetrics with_census(std::uint64_t datasets, std::uint64_t bucket_bytes,
                                        double age_seconds) {
    zetta::cluster::NodeMetrics metrics;
    metrics.sampled_at = "2026-10-11T12:00:00Z";
    metrics.storage_census_available = true;
    for (const std::string_view kind : zetta::data::storage_kinds()) {
        metrics.storage_census.kinds[std::string(kind)] = 0;
    }
    metrics.storage_census.kinds["datasets"] = datasets;
    metrics.storage_census.kinds["buckets"] = bucket_bytes;
    metrics.storage_census.buckets["photos"] = bucket_bytes;
    metrics.storage_census.total_bytes = datasets + bucket_bytes;
    metrics.storage_census_measured_at = "2026-10-11T11:59:30Z";
    metrics.storage_census_age_seconds = age_seconds;
    return metrics;
}

zetta::cluster::NodeTelemetrySample sample_of(std::string node_id,
                                              zetta::cluster::NodeMetrics metrics,
                                              double age_seconds = 0.5) {
    zetta::cluster::NodeTelemetrySample sample;
    sample.node_id = std::move(node_id);
    sample.metrics = std::move(metrics);
    sample.age_seconds = age_seconds;
    return sample;
}

}  // namespace

TEST(NodeTelemetryWire, CarriesTheStorageCensus) {
    const auto original = with_census(1000, 4096, 12.5);
    std::string error;
    const auto decoded =
        zetta::cluster::decode_node_metrics(encode_node_metrics(original, "node-a"), &error);
    ASSERT_TRUE(decoded.has_value()) << error;
    const auto& metrics = decoded->metrics;
    ASSERT_TRUE(metrics.storage_census_available);
    EXPECT_EQ(metrics.storage_census.total_bytes, 5096u);
    EXPECT_EQ(metrics.storage_census.kinds.at("datasets"), 1000u);
    EXPECT_EQ(metrics.storage_census.buckets.at("photos"), 4096u);
    EXPECT_EQ(metrics.storage_census_measured_at, "2026-10-11T11:59:30Z");
    EXPECT_DOUBLE_EQ(metrics.storage_census_age_seconds, 12.5);
    EXPECT_EQ(metrics.storage_census_buckets_omitted, 0u);

    // Without a census the payload carries none, and the receiver says so.
    const auto bare = zetta::cluster::decode_node_metrics(
        encode_node_metrics(zetta::cluster::NodeMetrics{}, "node-b"));
    ASSERT_TRUE(bare.has_value());
    EXPECT_FALSE(bare->metrics.storage_census_available);
}

TEST(NodeTelemetryWire, CapsTheListedBucketsAndKeepsTheirBytes) {
    auto metrics = with_census(0, 0, 0);
    metrics.storage_census.buckets.clear();
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < zetta::cluster::kMaxWireCensusBuckets + 10; ++index) {
        const std::uint64_t bytes = index + 1;
        metrics.storage_census.buckets["b" + std::to_string(index)] = bytes;
        total += bytes;
    }
    metrics.storage_census.kinds["buckets"] = total;
    metrics.storage_census.total_bytes = total;
    std::string error;
    const auto decoded =
        zetta::cluster::decode_node_metrics(encode_node_metrics(metrics, "node-a"), &error);
    ASSERT_TRUE(decoded.has_value()) << error;
    EXPECT_EQ(decoded->metrics.storage_census.buckets.size(), zetta::cluster::kMaxWireCensusBuckets);
    EXPECT_EQ(decoded->metrics.storage_census_buckets_omitted, 10u);
    EXPECT_EQ(decoded->metrics.storage_census.kinds.at("buckets"), total);
    // The smallest ten were left out, so the largest is still listed.
    EXPECT_TRUE(decoded->metrics.storage_census.buckets.contains(
        "b" + std::to_string(zetta::cluster::kMaxWireCensusBuckets + 9)));
    EXPECT_FALSE(decoded->metrics.storage_census.buckets.contains("b0"));
}

TEST(NodeTelemetryWire, RejectsAMalformedCensus) {
    std::string error;
    EXPECT_FALSE(zetta::cluster::decode_node_metrics(
                     R"({"node_id":"n","sampled_at":"t","storage_census":{"measured_at":"t",)"
                     R"("age_seconds":1,"total_bytes":-5,"kinds":{},"buckets":[]}})",
                     &error)
                     .has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(zetta::cluster::decode_node_metrics(
                     R"({"node_id":"n","sampled_at":"t","storage_census":{"measured_at":"t",)"
                     R"("age_seconds":1,"total_bytes":5,"kinds":{"datasets":"many"},"buckets":[]}})")
                     .has_value());
}

// A three-node cluster: two members with a fresh census are summed; a member whose census is
// stale, one that has not measured yet and one that does not report are named, never zero.
TEST(StorageCensusFold, SumsFreshMembersAndNamesTheRest) {
    using namespace std::chrono_literals;
    auto local = sample_of("node-a", with_census(1000, 100, 0));
    local.is_local = true;
    const std::vector<zetta::cluster::NodeTelemetrySample> samples{
        local,
        sample_of("node-b", with_census(2000, 50, 30)),
        sample_of("node-c", with_census(4000, 0, 179.8), 0.5),  // 180.3 s old: stale
        sample_of("node-d", zetta::cluster::NodeMetrics{}),      // reports, no census yet
    };
    const auto folded = zetta::cluster::fold_storage_census(
        {"node-a", "node-b", "node-c", "node-d", "node-e"}, samples, 180s);

    ASSERT_EQ(folded.nodes.size(), 2u);
    EXPECT_EQ(folded.nodes[0].node_id, "node-a");
    EXPECT_TRUE(folded.nodes[0].is_local);
    EXPECT_EQ(folded.nodes[1].node_id, "node-b");
    EXPECT_DOUBLE_EQ(folded.nodes[1].age_seconds, 30.5);
    EXPECT_EQ(folded.total.kinds.at("datasets"), 3000u);
    EXPECT_EQ(folded.total.kinds.at("buckets"), 150u);
    EXPECT_EQ(folded.total.buckets.at("photos"), 150u);  // physical: each copy counts
    EXPECT_EQ(folded.total.total_bytes, 3150u);
    for (const std::string_view kind : zetta::data::storage_kinds()) {
        EXPECT_TRUE(folded.total.kinds.contains(std::string(kind))) << kind;
    }

    ASSERT_EQ(folded.missing.size(), 3u);
    EXPECT_EQ(folded.missing[0].node_id, "node-c");
    EXPECT_EQ(folded.missing[0].reason, "stale");
    EXPECT_EQ(folded.missing[1].node_id, "node-d");
    EXPECT_EQ(folded.missing[1].reason, "not_measured");
    EXPECT_EQ(folded.missing[2].node_id, "node-e");
    EXPECT_EQ(folded.missing[2].reason, "not_reporting");
}
