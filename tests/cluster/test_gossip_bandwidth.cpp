// What a replication gossip round costs on the wire, and how fast a mesh
// converges under sustained writes (#794).
//
// `GossipBandwidth.Measure` is the measurement behind the #794 numbers. It is
// skipped unless ZETTA_GOSSIP_BENCH=1, because it runs for tens of seconds:
//
//   ZETTA_GOSSIP_BENCH=1 ZETTA_GOSSIP_BENCH_NODES=3 ./build/tests/zetta-tests \
//       --gtest_filter=GossipBandwidth.Measure
//
// ZETTA_GOSSIP_BENCH_NODES (default 3) sets the mesh size and
// ZETTA_GOSSIP_BENCH_TLS=0 runs it plaintext (mutual TLS is the default, as on
// every deployed node).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ClusterSecurity;
using zetta::cluster::GossipStats;
using zetta::cluster::ReplicationManager;

namespace {

using Clock = std::chrono::steady_clock;

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

ClusterSecurity must_derive(const std::string& token, const std::string& node_id) {
    std::string reason;
    auto security = zetta::cluster::derive_cluster_security(token, node_id, &reason);
    EXPECT_TRUE(security) << reason;
    return security ? *security : ClusterSecurity{};
}

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::atoi(value) : fallback;
}

// A loopback mesh of `count` nodes, star-seeded on the first so it meshes fully.
struct Mesh {
    std::vector<std::unique_ptr<ReplicationManager>> replicas;
    std::vector<std::unique_ptr<Cluster>> clusters;

    Mesh(int count, std::uint16_t base_port, bool tls) {
        const std::string token = zetta::cluster::generate_join_token();
        for (int index = 0; index < count; ++index) {
            const std::string id = zetta::util::new_uuid();
            const auto port = static_cast<std::uint16_t>(base_port + index);
            auto replica = std::make_unique<ReplicationManager>(id);
            replica->set("node:" + id,
                         {{"address", "127.0.0.1:" + std::to_string(port)}});
            ClusterConfig config;
            config.listen_host = "127.0.0.1";
            config.listen_port = port;
            config.port_range = 1;
            config.reconnect_interval_seconds = 1;
            if (index > 0) config.seeds = {"127.0.0.1:" + std::to_string(base_port)};
            if (tls) config.security = must_derive(token, id);
            clusters.push_back(std::make_unique<Cluster>(id, config, replica.get()));
            replicas.push_back(std::move(replica));
        }
        for (auto& cluster : clusters) cluster->start();
    }

    ~Mesh() {
        for (auto it = clusters.rbegin(); it != clusters.rend(); ++it) (*it)->stop();
    }

    bool meshed() const {
        const auto peers = clusters.size() - 1;
        return std::ranges::all_of(clusters, [&](const auto& cluster) {
            return cluster->peer_count() == peers;
        });
    }

    bool wait_meshed(int timeout_ms = 30000) const {
        for (int waited = 0; waited < timeout_ms; waited += 50) {
            if (meshed()) return true;
            sleep_ms(50);
        }
        return meshed();
    }

    bool converged() const {
        const std::string version = replicas.front()->version();
        const std::size_t entries = replicas.front()->entry_count();
        return std::ranges::all_of(replicas, [&](const auto& replica) {
            return replica->version() == version && replica->entry_count() == entries;
        });
    }

    // Milliseconds until every node holds the same document, or -1 at the deadline.
    long wait_converged(int timeout_ms) const {
        const auto start = Clock::now();
        while (Clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
            if (converged()) {
                return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             Clock::now() - start)
                                             .count());
            }
            sleep_ms(20);
        }
        return converged() ? timeout_ms : -1;
    }

    GossipStats stats() const {
        GossipStats total;
        for (const auto& cluster : clusters) {
            const GossipStats one = cluster->gossip_stats();
            total.rounds += one.rounds;
            total.sync_frames += one.sync_frames;
            total.full_syncs += one.full_syncs;
            total.incremental_syncs += one.incremental_syncs;
            total.sync_bytes += one.sync_bytes;
        }
        return total;
    }
};

GossipStats minus(const GossipStats& after, const GossipStats& before) {
    return GossipStats{
        .rounds = after.rounds - before.rounds,
        .sync_frames = after.sync_frames - before.sync_frames,
        .full_syncs = after.full_syncs - before.full_syncs,
        .incremental_syncs = after.incremental_syncs - before.incremental_syncs,
        .sync_bytes = after.sync_bytes - before.sync_bytes,
    };
}

// Every node writes `keys_per_tick` keys of `value_bytes` each, every 50 ms, for
// `write_ms`. With `fresh_keys` every write is a new key (the document grows, as
// with ingested rows); otherwise a bounded set of keys is overwritten (history
// grows, the live document does not).
void hammer(Mesh& mesh, int write_ms, int keys_per_tick, int value_bytes, bool fresh_keys) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (std::size_t node = 0; node < mesh.replicas.size(); ++node) {
        writers.emplace_back([&, node] {
            const std::string blob(static_cast<std::size_t>(value_bytes), 'x');
            ReplicationManager& replica = *mesh.replicas[node];
            for (std::uint64_t tick = 0; !stop.load(); ++tick) {
                for (int key = 0; key < keys_per_tick; ++key) {
                    const std::string name =
                        "load:" + std::to_string(node) + ":" +
                        (fresh_keys ? std::to_string(tick) + ":" : std::string{}) +
                        std::to_string(key);
                    replica.set(name, {{"n", tick}, {"blob", blob}});
                }
                sleep_ms(50);
            }
        });
    }
    sleep_ms(write_ms);
    stop.store(true);
    for (auto& writer : writers) writer.join();
}

struct Scenario {
    const char* name;
    int write_ms;
    int keys_per_tick;
    int value_bytes;
    bool fresh_keys;
};

}  // namespace

TEST(GossipBandwidth, Measure) {
    if (env_int("ZETTA_GOSSIP_BENCH", 0) == 0) {
        GTEST_SKIP() << "set ZETTA_GOSSIP_BENCH=1 to run the #794 gossip measurement";
    }
    const int nodes = std::clamp(env_int("ZETTA_GOSSIP_BENCH_NODES", 3), 2, 9);
    const bool tls = env_int("ZETTA_GOSSIP_BENCH_TLS", 1) != 0;
    const Scenario scenarios[] = {
        // The #786 regression gate's bounded load: 4 keys overwritten per tick.
        {"bounded", 6000, 4, 512, false},
        // The #794 wall: a growing document, 16 new 512 B keys per node per tick.
        {"growing", 10000, 16, 512, true},
    };
    std::uint16_t port = 44700;
    for (const Scenario& scenario : scenarios) {
        Mesh mesh(nodes, port, tls);
        port = static_cast<std::uint16_t>(port + 10);
        ASSERT_TRUE(mesh.wait_meshed()) << "mesh did not form";
        ASSERT_GE(mesh.wait_converged(30000), 0) << "idle mesh did not converge";
        const GossipStats before = mesh.stats();
        const auto started = Clock::now();
        hammer(mesh, scenario.write_ms, scenario.keys_per_tick, scenario.value_bytes,
               scenario.fresh_keys);
        const GossipStats during = minus(mesh.stats(), before);
        const double seconds =
            std::chrono::duration<double>(Clock::now() - started).count();
        const long converge_ms = mesh.wait_converged(60000);
        const std::size_t document = mesh.replicas.front()->save().size();
        std::fprintf(stderr,
                     "[gossip-bench] scenario=%s nodes=%d tls=%d write_s=%.1f "
                     "rounds=%llu sync_frames=%llu full=%llu incremental=%llu "
                     "sync_bytes=%llu bytes_per_round=%.0f MB_per_s=%.2f "
                     "final_document_bytes=%zu converge_after_stop_ms=%ld\n",
                     scenario.name, nodes, tls ? 1 : 0, seconds,
                     static_cast<unsigned long long>(during.rounds),
                     static_cast<unsigned long long>(during.sync_frames),
                     static_cast<unsigned long long>(during.full_syncs),
                     static_cast<unsigned long long>(during.incremental_syncs),
                     static_cast<unsigned long long>(during.sync_bytes),
                     during.rounds ? static_cast<double>(during.sync_bytes) /
                                         static_cast<double>(during.rounds)
                                   : 0.0,
                     static_cast<double>(during.sync_bytes) / seconds / 1e6,
                     document, converge_ms);
        EXPECT_GE(converge_ms, 0) << scenario.name << ": did not converge within 60 s";
    }
}
