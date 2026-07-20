#include <gtest/gtest.h>

#include <chrono>
#include <string>

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
