// SPDX-License-Identifier: MIT

// Chaos experiments (#842): torment a live in-process mutual-TLS cluster on a
// seeded schedule and assert it HEALS — not merely that it works undisturbed. The
// harness is tests/cluster/chaos_harness.hpp; the experiment format, the venues and
// how to run the long schedules are in docs/testing/chaos-experiments.md.
//
//   ChaosFaults.*  the injector and the schedule, no network (fast)
//   ChaosSmoke.*   one short pass over every fault class (ZETTA_CHAOS_SMOKE=1, until #835)
//   Chaos.*        the long runs, skipped unless their environment variable is set:
//                  ZETTA_CHAOS_SCHEDULES=<n> (seeded schedules) and
//                  ZETTA_CHAOS_PROBE_ROUNDS=<n> (the #835 stall probe)
//
// Every test here carries the ctest label `chaos` (`ctest -L chaos`).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "chaos_harness.hpp"

using namespace zetta::chaos;

namespace {

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::strtoull(value, nullptr, 10);
}

// Run `schedule` on a fresh cluster. Every step must prove its fault landed and
// then heal by the deadline; the first step that does not fails the test with the
// seed, the whole schedule, every node's state and the line that reruns it.
bool run_schedule(const ChaosOptions& options, const std::vector<ChaosStep>& schedule,
                  const std::string& rerun) {
    std::fprintf(stderr, "[chaos] seed=%llu nodes=%d steps=%zu\n%s",
                 static_cast<unsigned long long>(options.seed), options.nodes,
                 schedule.size(), describe(schedule).c_str());
    ChaosCluster cluster(options);
    cluster.start();
    if (!cluster.wait_meshed()) {
        ADD_FAILURE() << "chaos seed " << options.seed
                      << ": the mesh did not form before any fault\n"
                      << cluster.report() << "rerun: " << rerun;
        return false;
    }
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const ChaosStep& step = schedule[i];
        const StepResult result = run_step(cluster, step);
        if (!result.fault_landed) {
            ADD_FAILURE() << "chaos seed " << options.seed << " step " << i << " ("
                          << step.describe()
                          << "): the fault did not land, so the step proves nothing — "
                          << result.fault_evidence
                          << "\n(few frames received means the link itself had stalled — "
                             "the #835 shape — not that the injector failed)\nschedule:\n"
                          << describe(schedule) << cluster.report() << "rerun: " << rerun;
            return false;
        }
        if (!result.healed) {
            std::string shape;
            if (result.frozen_node) {
                shape = std::format(
                    "\nSHAPE: #835 — every link up, the other nodes agree, and node {}'s "
                    "document did not move for at least half the deadline",
                    result.frozen_index);
            }
            if (options.inject) {
                shape += "\nframes received after the heal:";
                for (std::size_t j = 0; j < result.received_after_heal.size(); ++j) {
                    shape += std::format(" node {}={}", j, result.received_after_heal[j]);
                }
            }
            ADD_FAILURE() << "chaos seed " << options.seed << " step " << i << " ("
                          << step.describe() << "): did not heal within "
                          << options.converge_deadline.count()
                          << "ms of the heal — " << result.failure << shape
                          << "\nfault evidence: " << result.fault_evidence
                          << "\nschedule:\n"
                          << describe(schedule) << cluster.report() << "rerun: " << rerun;
            return false;
        }
        std::fprintf(stderr, "[chaos] seed=%llu step %zu %s: landed (%s), healed in %ldms\n",
                     static_cast<unsigned long long>(options.seed), i,
                     step.describe().c_str(), result.fault_evidence.c_str(),
                     result.heal_ms);
    }
    return true;
}

}  // namespace

// --- The injector and the schedule (no network) --------------------------------

TEST(ChaosFaults, SameSeedSameScheduleAndDifferentSeedsDiffer) {
    const auto a = make_schedule(842, 5, 12);
    const auto b = make_schedule(842, 5, 12);
    const auto c = make_schedule(843, 5, 12);
    EXPECT_EQ(describe(a), describe(b));
    EXPECT_NE(describe(a), describe(c));
}

TEST(ChaosFaults, ScheduleStaysWithinItsBounds) {
    const ScheduleBounds bounds;
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        for (const ChaosStep& step : make_schedule(seed, 4, 10, bounds)) {
            EXPECT_NE(step.kind, FaultKind::LoadOnly) << "a schedule step is a fault";
            EXPECT_GE(step.target, 0);
            EXPECT_LT(step.target, 4);
            EXPECT_GE(step.window_ms, bounds.min_window_ms);
            EXPECT_LE(step.window_ms, bounds.max_window_ms);
            EXPECT_LE(step.frames.delay_max_ms, bounds.max_delay_ms);
            // A lag never reaches the 2s idle timeout, or skew would be a partition.
            EXPECT_LE(step.frames.lag_ms, bounds.max_lag_ms);
        }
    }
}

TEST(ChaosFaults, InjectorDropsDuplicatesReordersDelaysAndHeals) {
    SeededFrameFaults faults(1);
    const std::string peer = "peer";
    using Frames = std::vector<std::string>;

    EXPECT_EQ(faults.on_inbound(peer, "f0"), Frames{"f0"});  // no fault: as sent

    faults.set({.drop = 1.0});
    EXPECT_EQ(faults.on_inbound(peer, "f1"), Frames{});

    faults.set({.duplicate = 1.0});
    EXPECT_EQ(faults.on_inbound(peer, "f2"), (Frames{"f2", "f2"}));

    faults.set({.reorder = 1.0});
    EXPECT_EQ(faults.on_inbound(peer, "f3"), Frames{});              // held back…
    EXPECT_EQ(faults.on_inbound(peer, "f4"), (Frames{"f4", "f3"}));  // …lands after f4

    faults.set({.reorder = 1.0});
    EXPECT_EQ(faults.on_inbound(peer, "f5"), Frames{});
    sleep_for_ms(SeededFrameFaults::kReorderFallbackMs + 50);  // no next frame comes
    EXPECT_EQ(faults.release_due(peer), Frames{"f5"});

    faults.set({.lag_ms = 40});
    EXPECT_EQ(faults.on_inbound(peer, "f6"), Frames{});
    EXPECT_EQ(faults.release_due(peer), Frames{});  // not due yet
    sleep_for_ms(80);
    EXPECT_EQ(faults.release_due(peer), Frames{"f6"});

    faults.heal();
    faults.block(peer);
    EXPECT_EQ(faults.on_inbound(peer, "f7"), Frames{});
    EXPECT_EQ(faults.on_inbound("other", "g0"), Frames{"g0"});  // only that peer
    faults.heal();
    EXPECT_EQ(faults.on_inbound(peer, "f8"), Frames{"f8"});

    const FrameFaultCounters counters = faults.counters();
    EXPECT_EQ(counters.received, 10u);  // f0..f8 and g0
    EXPECT_EQ(counters.dropped, 2u);
    EXPECT_EQ(counters.duplicated, 1u);
    EXPECT_EQ(counters.reordered, 2u);
    EXPECT_EQ(counters.delayed, 1u);
}

TEST(ChaosFaults, SameSeedSameFrameDecisions) {
    const auto decisions = [](std::uint64_t seed) {
        SeededFrameFaults faults(seed);
        faults.set({.drop = 0.5, .duplicate = 0.5});
        std::string pattern;
        for (int i = 0; i < 64; ++i) {
            pattern += std::to_string(faults.on_inbound("p", "x").size());
        }
        return pattern;
    };
    EXPECT_EQ(decisions(7), decisions(7));
    EXPECT_NE(decisions(7), decisions(8));
}

// --- The smoke: every fault class once, on a 3-node mTLS mesh, under load -------

// HYPOTHESIS   a 3-node mutual-TLS mesh under write load, given each fault class
//              once (frames dropped / delayed / duplicated / reordered, a timing
//              skew, a one-way and a symmetric partition, a node killed and
//              restarted), heals after every one
// FAULT        one per step, on one node, 1.2 s each
// LOAD         3 writers, ~40 ops/s each, sets and removes, throughout each window
// INVARIANT    every node: the same version AND the same snapshot, every written
//              key at its last value, no removed key back, full peers and members
// DEADLINE     30 s from each heal (patient poll)
// BLAST RADIUS the harness's own 3 nodes on 127.0.0.1:43900-43902; nothing persists
TEST(ChaosSmoke, ThreeNodeMtlsMeshHealsFromEveryFaultClass) {
    // Opt-in until #835 is fixed: the smoke hits the mesh load stall often
    // enough that it would leave the default suite red.
    if (env_u64("ZETTA_CHAOS_SMOKE", 0) == 0) {
        GTEST_SKIP() << "the every-fault-class smoke: set ZETTA_CHAOS_SMOKE=1 "
                        "(opt-in until #835 is fixed)";
    }
    ChaosOptions options;
    options.seed = 842;
    options.nodes = 3;
    options.base_port = 43900;
    const int window = 1200;
    std::vector<ChaosStep> schedule = {
        {.kind = FaultKind::LoadOnly, .target = 0, .window_ms = window, .frames = {}},
        {.kind = FaultKind::FrameDrop, .target = 1, .window_ms = window,
         .frames = {.drop = 0.5}},
        {.kind = FaultKind::FrameDelay, .target = 2, .window_ms = window,
         .frames = {.delay = 0.7, .delay_min_ms = 50, .delay_max_ms = 600}},
        {.kind = FaultKind::FrameDuplicate, .target = 0, .window_ms = window,
         .frames = {.duplicate = 0.5}},
        {.kind = FaultKind::FrameReorder, .target = 1, .window_ms = window,
         .frames = {.reorder = 0.5}},
        {.kind = FaultKind::TimingSkew, .target = 2, .window_ms = window,
         .frames = {.lag_ms = 700}},
        {.kind = FaultKind::PartitionOneWay, .target = 0, .window_ms = window, .frames = {}},
        {.kind = FaultKind::PartitionSymmetric, .target = 1, .window_ms = window, .frames = {}},
        {.kind = FaultKind::KillRestart, .target = 2, .window_ms = window, .frames = {}},
        {.kind = FaultKind::KillRestart, .target = 0, .window_ms = window, .frames = {}},  // the hub
    };
    EXPECT_TRUE(run_schedule(options, schedule,
                             "build/tests/zetta-chaos-tests "
                             "--gtest_filter=ChaosSmoke.ThreeNodeMtlsMeshHealsFromEveryFaultClass"));
}

// --- The long runs (label `chaos`, opt-in by environment) -----------------------

// HYPOTHESIS   any seeded schedule of single faults heals after every step
// FAULT        ZETTA_CHAOS_STEPS (default 6) drawn from the seed, 1.5–6 s each
// LOAD         one writer per node throughout each window
// INVARIANT    as the smoke
// DEADLINE     30 s from each heal
// BLAST RADIUS the harness's own nodes on 127.0.0.1:43910-43914
TEST(Chaos, SeededSchedulesHeal) {
    const std::uint64_t schedules = env_u64("ZETTA_CHAOS_SCHEDULES", 0);
    if (schedules == 0) {
        GTEST_SKIP() << "long chaos run: set ZETTA_CHAOS_SCHEDULES=<n> (and optionally "
                        "ZETTA_CHAOS_SEED, ZETTA_CHAOS_NODES=3..5, ZETTA_CHAOS_STEPS)";
    }
    const auto now_seed = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const std::uint64_t first_seed = env_u64("ZETTA_CHAOS_SEED", now_seed % 1000000007ULL);
    const int nodes = static_cast<int>(
        std::clamp<std::uint64_t>(env_u64("ZETTA_CHAOS_NODES", 3), 3, 5));
    const int steps = static_cast<int>(env_u64("ZETTA_CHAOS_STEPS", 6));
    for (std::uint64_t i = 0; i < schedules; ++i) {
        ChaosOptions options;
        options.seed = first_seed + i;
        options.nodes = nodes;
        options.base_port = 43910;
        const std::string rerun = std::format(
            "ZETTA_CHAOS_SCHEDULES=1 ZETTA_CHAOS_SEED={} ZETTA_CHAOS_NODES={} "
            "ZETTA_CHAOS_STEPS={} build/tests/zetta-chaos-tests "
            "--gtest_filter=Chaos.SeededSchedulesHeal",
            options.seed, nodes, steps);
        if (!run_schedule(options, make_schedule(options.seed, nodes, steps), rerun)) {
            return;
        }
    }
}

// The #835 probe: the exact load of ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad
// (3 nodes, 4 keys × 512 B per node every 50 ms for 5 s), with NO injector installed
// so the network thread runs unperturbed, repeated for ZETTA_CHAOS_PROBE_ROUNDS
// fresh clusters. Each round must converge within 45 s; a round that does not is
// reported with whether it has the #835 shape (one node's document frozen while
// every link is up and the others agree).
//
// HYPOTHESIS   a 3-node mTLS mesh converges within 45 s of sustained load stopping
// FAULT        none injected — sustained concurrent load is the provocation
// LOAD         3 writers, 4 keys × 512 B every 50 ms, 5 s
// INVARIANT    as the smoke
// DEADLINE     45 s from the load stopping
// BLAST RADIUS the harness's own nodes on 127.0.0.1:43920-43922
TEST(Chaos, MeshLoadStallProbe) {
    const std::uint64_t rounds = env_u64("ZETTA_CHAOS_PROBE_ROUNDS", 0);
    if (rounds == 0) {
        GTEST_SKIP() << "the #835 stall probe: set ZETTA_CHAOS_PROBE_ROUNDS=<n> "
                        "(optionally ZETTA_CHAOS_SEED)";
    }
    const std::uint64_t first_seed = env_u64("ZETTA_CHAOS_SEED", 835);
    std::uint64_t stalls = 0;
    std::uint64_t frozen = 0;
    std::string first_stall;
    for (std::uint64_t round = 0; round < rounds; ++round) {
        ChaosOptions options;
        options.seed = first_seed + round;
        options.nodes = 3;
        options.base_port = 43920;
        options.keys_per_node = 4;
        options.write_interval_ms = 12;  // 4 keys per 50 ms, one at a time
        options.blob_bytes = 512;
        options.remove_ratio = 0.0;
        options.inject = false;
        options.idle_timeout_seconds = 6;  // the production default
        options.converge_deadline = std::chrono::milliseconds(45000);
        ChaosCluster cluster(options);
        cluster.start();
        ASSERT_TRUE(cluster.wait_meshed()) << "seed " << options.seed << "\n"
                                           << cluster.report();
        const StepResult result =
            run_step(cluster, {.kind = FaultKind::LoadOnly, .target = 0, .window_ms = 5000, .frames = {}});
        ASSERT_TRUE(result.fault_landed) << result.fault_evidence;
        std::fprintf(stderr, "[chaos-probe] round %llu seed=%llu: %s%s\n",
                     static_cast<unsigned long long>(round),
                     static_cast<unsigned long long>(options.seed),
                     result.healed ? std::format("converged in {}ms", result.heal_ms).c_str()
                                   : ("STALL: " + result.failure).c_str(),
                     result.frozen_node ? " (#835 shape)" : "");
        if (!result.healed) {
            ++stalls;
            if (result.frozen_node) ++frozen;
            if (first_stall.empty()) {
                first_stall = std::format(
                    "seed {}: {}{}\n{}rerun: ZETTA_CHAOS_PROBE_ROUNDS=1 ZETTA_CHAOS_SEED={} "
                    "build/tests/zetta-chaos-tests --gtest_filter=Chaos.MeshLoadStallProbe",
                    options.seed, result.failure,
                    result.frozen_node
                        ? std::format(" — #835 shape, node {} frozen", result.frozen_index)
                        : "",
                    cluster.report(), options.seed);
            }
        }
    }
    std::fprintf(stderr, "[chaos-probe] %llu of %llu rounds stalled (%llu with the #835 shape)\n",
                 static_cast<unsigned long long>(stalls),
                 static_cast<unsigned long long>(rounds),
                 static_cast<unsigned long long>(frozen));
    EXPECT_EQ(stalls, 0u) << stalls << " of " << rounds << " rounds did not converge ("
                          << frozen << " with the #835 shape); first: " << first_stall;
}
