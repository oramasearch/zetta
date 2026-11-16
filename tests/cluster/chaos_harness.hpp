// SPDX-License-Identifier: MIT

#pragma once

// The chaos harness (#842): an in-process 3–5 node mutual-TLS cluster that is
// tormented on a seeded schedule — a node killed and restarted, partitioned and
// healed, its inbound frames dropped / delayed / duplicated / reordered, its view
// of its peers lagged — while every node writes, and then asserted to HEAL: every
// node converges to the identical document version AND the identical projection
// (snapshot) within a deadline, no written key is lost and no removed key comes
// back. Each step proves its fault landed before its result counts. Every failure
// prints the seed and the schedule, and how to rerun exactly that schedule.
//
// What is reproducible: the schedule (which fault, which node, for how long, with
// which parameters), every node id, every frame-level fault decision sequence and
// every writer's key/op sequence all derive from the seed. What is not: the thread
// interleaving of real TCP + TLS in-process — a seed replays the same torment, not
// the same nanoseconds. See docs/testing/chaos-experiments.md.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/frame_faults.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"

namespace zetta::chaos {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

inline void sleep_for_ms(std::int64_t ms) {
    std::this_thread::sleep_for(milliseconds(ms));
}

// splitmix64 — derives independent, reproducible sub-seeds from one seed.
inline std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t stream) {
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (stream + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// A UUIDv4-shaped node id drawn from `rng`, so a seed fixes every actor id (and so
// the min-node-id link tiebreak) instead of leaving it to new_uuid().
inline std::string seeded_uuid(std::mt19937_64& rng) {
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 10
    const std::string h = std::format("{:016x}{:016x}", hi, lo);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" +
           h.substr(16, 4) + "-" + h.substr(20, 12);
}

// --- Frame faults -------------------------------------------------------------

// What a node's injector does to frames from the peers it applies to. Every
// probability is per frame, decided in this order: drop, delay, reorder, duplicate.
struct FrameFaultSettings {
    double drop = 0.0;        // discard the frame
    double delay = 0.0;       // hold it for a uniform [delay_min_ms, delay_max_ms]
    int delay_min_ms = 0;
    int delay_max_ms = 0;
    double reorder = 0.0;     // hold it until the NEXT frame has been applied
    double duplicate = 0.0;   // apply it twice
    int lag_ms = 0;           // timing skew: every frame arrives this much late
};

// Counters an experiment reads to prove its fault landed (all monotonic).
struct FrameFaultCounters {
    std::uint64_t received = 0;
    std::uint64_t applied = 0;
    std::uint64_t dropped = 0;    // by a drop probability or a partition
    std::uint64_t delayed = 0;    // by a delay probability or a lag
    std::uint64_t reordered = 0;
    std::uint64_t duplicated = 0;
};

// The harness's FrameFaultInjector: seeded, thread-safe (the test thread changes
// settings while the node's network thread filters frames), self-healing on
// heal(). A held frame (delay, lag, reorder) is never lost by heal(): it comes out
// through release_due() when due, as a delayed packet would.
class SeededFrameFaults final : public zetta::cluster::FrameFaultInjector {
public:
    explicit SeededFrameFaults(std::uint64_t seed) : rng_(seed) {}

    // Apply `settings` to frames from every peer.
    void set(const FrameFaultSettings& settings) {
        const std::lock_guard lock(mutex_);
        settings_ = settings;
    }
    // Discard everything from `peer_node_id` (one direction of a partition).
    void block(const std::string& peer_node_id) {
        const std::lock_guard lock(mutex_);
        blocked_.insert(peer_node_id);
    }
    // Clear every fault. Held frames still release when due.
    void heal() {
        const std::lock_guard lock(mutex_);
        settings_ = {};
        blocked_.clear();
    }
    FrameFaultCounters counters() const {
        const std::lock_guard lock(mutex_);
        return counters_;
    }

    std::vector<std::string> on_inbound(const std::string& peer,
                                        std::string frame) override {
        const std::lock_guard lock(mutex_);
        ++counters_.received;
        std::vector<std::string> out;
        if (blocked_.contains(peer) || chance(settings_.drop)) {
            ++counters_.dropped;
            return out;
        }
        const auto now = Clock::now();
        int hold_ms = settings_.lag_ms;
        if (chance(settings_.delay)) {
            std::uniform_int_distribution<int> span(
                settings_.delay_min_ms,
                std::max(settings_.delay_min_ms, settings_.delay_max_ms));
            hold_ms += span(rng_);
        }
        if (hold_ms > 0) {
            ++counters_.delayed;
            held_[peer].push_back({now + milliseconds(hold_ms), sequence_++,
                                   std::move(frame)});
            return out;
        }
        auto& stash = stash_[peer];
        if (!stash && chance(settings_.reorder)) {
            ++counters_.reordered;
            // Released after the next frame, or by release_due() if none comes.
            stash = Held{now + milliseconds(kReorderFallbackMs), sequence_++,
                         std::move(frame)};
            return out;
        }
        out.push_back(frame);
        if (chance(settings_.duplicate)) {
            ++counters_.duplicated;
            out.push_back(frame);
        }
        if (stash) {
            out.push_back(std::move(stash->frame));
            stash.reset();
        }
        counters_.applied += out.size();
        return out;
    }

    std::vector<std::string> release_due(const std::string& peer) override {
        const std::lock_guard lock(mutex_);
        std::vector<std::string> out;
        const auto now = Clock::now();
        if (auto it = held_.find(peer); it != held_.end()) {
            auto& queue = it->second;
            std::vector<Held> due;
            std::erase_if(queue, [&](Held& held) {
                if (held.due > now) return false;
                due.push_back(std::move(held));
                return true;
            });
            std::ranges::sort(due, {}, [](const Held& h) {
                return std::pair{h.due, h.order};
            });
            for (auto& held : due) out.push_back(std::move(held.frame));
        }
        if (auto it = stash_.find(peer);
            it != stash_.end() && it->second && it->second->due <= now) {
            out.push_back(std::move(it->second->frame));
            it->second.reset();
        }
        counters_.applied += out.size();
        return out;
    }

    static constexpr int kReorderFallbackMs = 150;

private:
    struct Held {
        Clock::time_point due;
        std::uint64_t order = 0;
        std::string frame;
    };

    bool chance(double p) {
        if (p <= 0.0) return false;
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
    }

    mutable std::mutex mutex_;
    std::mt19937_64 rng_;
    FrameFaultSettings settings_;
    std::set<std::string> blocked_;
    std::map<std::string, std::vector<Held>> held_;
    std::map<std::string, std::optional<Held>> stash_;
    std::uint64_t sequence_ = 0;
    FrameFaultCounters counters_;
};

// --- The schedule -------------------------------------------------------------

enum class FaultKind {
    LoadOnly,            // no fault: the baseline, and the #835 probe
    KillRestart,         // stop a node's networking abruptly, restart it, data intact
    PartitionOneWay,     // the node discards every frame from its peers, still sends
    PartitionSymmetric,  // the node and its peers discard each other's frames
    FrameDrop,
    FrameDelay,
    FrameDuplicate,
    FrameReorder,
    TimingSkew,          // the node sees every peer frame a constant lag late
};

inline const char* to_string(FaultKind kind) {
    switch (kind) {
        case FaultKind::LoadOnly: return "load-only";
        case FaultKind::KillRestart: return "kill-restart";
        case FaultKind::PartitionOneWay: return "partition-one-way";
        case FaultKind::PartitionSymmetric: return "partition-symmetric";
        case FaultKind::FrameDrop: return "frame-drop";
        case FaultKind::FrameDelay: return "frame-delay";
        case FaultKind::FrameDuplicate: return "frame-duplicate";
        case FaultKind::FrameReorder: return "frame-reorder";
        case FaultKind::TimingSkew: return "timing-skew";
    }
    return "?";
}

// One fault, on one node, for one window, under load. Exactly one fault per step.
struct ChaosStep {
    FaultKind kind = FaultKind::LoadOnly;
    int target = 0;            // the node the fault is injected on
    int window_ms = 1000;      // how long the fault (and the load) lasts
    FrameFaultSettings frames; // the frame faults for the Frame*/TimingSkew kinds

    std::string describe() const {
        std::string text = std::format("{} node={} window={}ms", to_string(kind),
                                       target, window_ms);
        switch (kind) {
            case FaultKind::FrameDrop:
                text += std::format(" drop={:.2f}", frames.drop);
                break;
            case FaultKind::FrameDelay:
                text += std::format(" delay={:.2f} [{}..{}]ms", frames.delay,
                                    frames.delay_min_ms, frames.delay_max_ms);
                break;
            case FaultKind::FrameDuplicate:
                text += std::format(" duplicate={:.2f}", frames.duplicate);
                break;
            case FaultKind::FrameReorder:
                text += std::format(" reorder={:.2f}", frames.reorder);
                break;
            case FaultKind::TimingSkew:
                text += std::format(" lag={}ms", frames.lag_ms);
                break;
            default:
                break;
        }
        return text;
    }
};

// Bounds every schedule stays within, so a seed never asks for more than the
// cluster's own timeouts can heal from (a lag stays under the idle timeout).
struct ScheduleBounds {
    int min_window_ms = 1500;
    int max_window_ms = 6000;
    int max_delay_ms = 800;
    int max_lag_ms = 1500;
};

// `steps` faults drawn from `seed` over a `nodes`-node cluster. Same seed, same
// schedule — the property the failure report's rerun line depends on.
inline std::vector<ChaosStep> make_schedule(std::uint64_t seed, int nodes, int steps,
                                            const ScheduleBounds& bounds = {}) {
    std::mt19937_64 rng(mix_seed(seed, 0xC4A05));
    const auto pick = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };
    const auto prob = [&](double lo, double hi) {
        return std::uniform_real_distribution<double>(lo, hi)(rng);
    };
    std::vector<ChaosStep> schedule;
    for (int i = 0; i < steps; ++i) {
        ChaosStep step;
        step.kind = static_cast<FaultKind>(
            pick(static_cast<int>(FaultKind::KillRestart),
                 static_cast<int>(FaultKind::TimingSkew)));
        step.target = pick(0, nodes - 1);
        step.window_ms = pick(bounds.min_window_ms, bounds.max_window_ms);
        switch (step.kind) {
            case FaultKind::FrameDrop: step.frames.drop = prob(0.2, 0.6); break;
            case FaultKind::FrameDelay:
                step.frames.delay = prob(0.3, 0.9);
                step.frames.delay_min_ms = pick(20, 100);
                step.frames.delay_max_ms = pick(200, bounds.max_delay_ms);
                break;
            case FaultKind::FrameDuplicate: step.frames.duplicate = prob(0.2, 0.6); break;
            case FaultKind::FrameReorder: step.frames.reorder = prob(0.2, 0.6); break;
            case FaultKind::TimingSkew:
                step.frames.lag_ms = pick(100, bounds.max_lag_ms);
                break;
            default: break;
        }
        schedule.push_back(step);
    }
    return schedule;
}

inline std::string describe(const std::vector<ChaosStep>& schedule) {
    std::string text;
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        text += std::format("  step {}: {}\n", i, schedule[i].describe());
    }
    return text;
}

// --- The cluster --------------------------------------------------------------

struct ChaosOptions {
    std::uint64_t seed = 842;
    int nodes = 3;
    std::uint16_t base_port = 43900;
    // Load: each node overwrites/removes its own keys `chaos:<node>:<k>` — one
    // writer per key, so the expected final value of every key is exact.
    int keys_per_node = 8;
    int write_interval_ms = 25;
    std::size_t blob_bytes = 256;
    double remove_ratio = 0.15;
    // Install the frame-fault injector on every node. False runs the network
    // thread unperturbed (#835 vanished when a printf was added to that thread).
    bool inject = true;
    unsigned idle_timeout_seconds = 2;
    milliseconds converge_deadline{30000};
};

// One node: its document, its live Cluster (absent while killed), its injector,
// and the ledger of what its writer has done.
struct ChaosNode {
    int index = 0;
    std::string id;
    std::uint16_t port = 0;
    zetta::cluster::ClusterSecurity security;
    std::shared_ptr<SeededFrameFaults> faults;
    std::mutex mutex;  // guards repl/cluster/ledger/alive against the writer
    std::unique_ptr<zetta::cluster::ReplicationManager> repl;
    std::unique_ptr<zetta::cluster::Cluster> cluster;
    // key -> the seq last written, or nullopt when the last op removed it.
    std::map<std::string, std::optional<std::uint64_t>> ledger;
    bool alive = false;
    std::mt19937_64 writer_rng;
    std::atomic<std::uint64_t> writes{0};
};

// What one node looks like, for the invariant and the failure report.
struct NodeView {
    bool alive = false;
    std::string version;
    std::size_t entries = 0;
    std::size_t bytes = 0;  // filled by report() only (a save() is not free)
    std::size_t peers = 0;
    std::size_t connections = 0;
    std::size_t members = 0;
    nlohmann::json snapshot;
    FrameFaultCounters counters;
};

class ChaosCluster {
public:
    explicit ChaosCluster(ChaosOptions options) : options_(std::move(options)) {
        std::mt19937_64 ids(mix_seed(options_.seed, 0x1D5));
        const std::string token = zetta::cluster::generate_join_token();
        for (int i = 0; i < options_.nodes; ++i) {
            auto node = std::make_unique<ChaosNode>();
            node->index = i;
            node->id = seeded_uuid(ids);
            node->port = static_cast<std::uint16_t>(options_.base_port + i);
            std::string reason;
            auto security =
                zetta::cluster::derive_cluster_security(token, node->id, &reason);
            if (security) node->security = *security;
            if (options_.inject) {
                node->faults = std::make_shared<SeededFrameFaults>(
                    mix_seed(options_.seed, 0xFA00 + i));
            }
            node->writer_rng.seed(mix_seed(options_.seed, 0x3700 + i));
            node->repl = std::make_unique<zetta::cluster::ReplicationManager>(node->id);
            // Each node publishes its membership entry so the hub-seeded star
            // fills into a full mesh (#789), and a restarted node is re-dialed.
            node->repl->set("node:" + node->id,
                            {{"address", std::format("127.0.0.1:{}", node->port)}});
            nodes_.push_back(std::move(node));
        }
    }

    ~ChaosCluster() {
        stop_writers_ = true;
        for (auto& writer : writers_) writer.join();
        for (auto& node : nodes_) {
            const std::lock_guard lock(node->mutex);
            if (node->cluster) node->cluster->stop();
            node->cluster.reset();
        }
    }

    ChaosCluster(const ChaosCluster&) = delete;
    ChaosCluster& operator=(const ChaosCluster&) = delete;

    const ChaosOptions& options() const { return options_; }
    int size() const { return static_cast<int>(nodes_.size()); }
    ChaosNode& node(int i) { return *nodes_.at(i); }

    void start() {
        for (auto& node : nodes_) {
            const std::lock_guard lock(node->mutex);
            start_locked(*node);
        }
        for (auto& node : nodes_) {
            writers_.emplace_back([this, raw = node.get()] { writer_loop(*raw); });
        }
    }

    bool wait_meshed(milliseconds deadline = milliseconds(20000)) {
        return poll_until(deadline, [&] { return meshed(); });
    }

    bool meshed() {
        const auto want = static_cast<std::size_t>(size() - 1);
        for (auto& node : nodes_) {
            const std::lock_guard lock(node->mutex);
            if (!node->alive || !node->cluster ||
                node->cluster->peer_count() != want) {
                return false;
            }
        }
        return true;
    }

    void set_load(bool on) { load_ = on; }

    // --- Faults ---------------------------------------------------------------

    // Stop the node's networking abruptly (the io_context stops mid-flight, every
    // socket closes) and keep its document as a kill -9 with an intact data dir
    // would: every write it acknowledged survives, nothing more.
    void kill(int i) {
        ChaosNode& n = node(i);
        const std::lock_guard lock(n.mutex);
        if (!n.alive) return;
        n.alive = false;
        n.cluster->stop();
        n.cluster.reset();
        killed_blob_[i] = n.repl->save();
        n.repl.reset();
    }

    // Restart a killed node on the same port from its persisted document.
    bool restart(int i) {
        ChaosNode& n = node(i);
        const std::lock_guard lock(n.mutex);
        if (n.alive) return true;
        n.repl = std::make_unique<zetta::cluster::ReplicationManager>(n.id);
        if (!n.repl->restore(killed_blob_[i])) return false;
        start_locked(n);
        return n.cluster->is_listening();
    }

    void partition(int target, bool symmetric) {
        for (int other = 0; other < size(); ++other) {
            if (other == target) continue;
            node(target).faults->block(node(other).id);
            if (symmetric) node(other).faults->block(node(target).id);
        }
    }

    void set_frames(int target, const FrameFaultSettings& settings) {
        node(target).faults->set(settings);
    }

    void heal_all() {
        for (auto& node : nodes_) {
            if (node->faults) node->faults->heal();
        }
    }

    // --- Observation ----------------------------------------------------------

    NodeView view(int i) {
        ChaosNode& n = node(i);
        NodeView view;
        const std::lock_guard lock(n.mutex);
        view.alive = n.alive;
        if (n.faults) view.counters = n.faults->counters();
        if (!n.alive) return view;
        view.version = n.repl->version();
        view.entries = n.repl->entry_count();
        view.snapshot = n.repl->snapshot();
        view.peers = n.cluster->peer_count();
        view.connections = n.cluster->connection_count();
        view.members = n.cluster->membership().size();
        return view;
    }

    std::uint64_t total_writes() const {
        std::uint64_t total = 0;
        for (const auto& node : nodes_) total += node->writes.load();
        return total;
    }

    // The heal invariant, read on EVERY node (never just the initiator). Empty
    // when it holds; otherwise the first reason it does not.
    std::string check_healed() {
        std::vector<NodeView> views;
        for (int i = 0; i < size(); ++i) views.push_back(view(i));
        const auto want_peers = static_cast<std::size_t>(size() - 1);
        for (int i = 0; i < size(); ++i) {
            const NodeView& v = views[i];
            if (!v.alive) return std::format("node {} is not running", i);
            if (v.peers != want_peers) {
                return std::format("node {} has {} peers, want {}", i, v.peers,
                                   want_peers);
            }
            if (v.members != static_cast<std::size_t>(size())) {
                return std::format("node {} lists {} members, want {}", i,
                                   v.members, size());
            }
        }
        for (int i = 1; i < size(); ++i) {
            if (views[i].version != views[0].version) {
                return std::format("version differs: node 0 {} vs node {} {}",
                                   views[0].version, i, views[i].version);
            }
            if (views[i].snapshot != views[0].snapshot) {
                return std::format("projection differs between node 0 and node {} "
                                   "at the same version",
                                   i);
            }
        }
        // No lost key, no resurrected key: every ledger entry, on every node.
        for (auto& writer : nodes_) {
            std::map<std::string, std::optional<std::uint64_t>> ledger;
            {
                const std::lock_guard lock(writer->mutex);
                ledger = writer->ledger;
            }
            for (int i = 0; i < size(); ++i) {
                const nlohmann::json& snap = views[i].snapshot;
                for (const auto& [key, seq] : ledger) {
                    const bool present = snap.contains(key);
                    if (seq && !present) {
                        return std::format("LOST: node {} is missing {} (seq {})", i,
                                           key, *seq);
                    }
                    if (!seq && present) {
                        return std::format("RESURRECTED: node {} has removed key {}",
                                           i, key);
                    }
                    if (seq && snap[key].value("seq", std::uint64_t{0}) != *seq) {
                        return std::format("STALE: node {} has {} at seq {}, want {}",
                                           i, key,
                                           snap[key].value("seq", std::uint64_t{0}),
                                           *seq);
                    }
                }
            }
        }
        return {};
    }

    std::string report() {
        std::string text;
        for (int i = 0; i < size(); ++i) {
            NodeView v = view(i);
            {
                const std::lock_guard lock(node(i).mutex);
                if (node(i).repl) v.bytes = node(i).repl->save().size();
            }
            text += std::format(
                "  node {} {} alive={} ver={} entries={} bytes={} peers={} conns={} "
                "members={} writes={} frames recv={} applied={} dropped={} delayed={} "
                "reordered={} duplicated={}\n",
                i, node(i).id.substr(0, 8), v.alive, v.version, v.entries, v.bytes,
                v.peers, v.connections, v.members, node(i).writes.load(),
                v.counters.received, v.counters.applied, v.counters.dropped,
                v.counters.delayed, v.counters.reordered, v.counters.duplicated);
        }
        return text;
    }

    template <typename Predicate>
    static bool poll_until(milliseconds deadline, Predicate&& predicate) {
        const auto until = Clock::now() + deadline;
        while (Clock::now() < until) {
            if (predicate()) return true;
            sleep_for_ms(50);
        }
        return predicate();
    }

private:
    void start_locked(ChaosNode& n) {
        zetta::cluster::ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = n.port;
        config.port_range = 1;
        config.reconnect_interval_seconds = 1;
        config.peer_idle_timeout_seconds = options_.idle_timeout_seconds;
        config.membership_grace_seconds = 0;
        if (n.index != 0) {
            config.seeds = {std::format("127.0.0.1:{}", options_.base_port)};
        }
        config.security = n.security;
        config.frame_faults = n.faults;
        n.cluster = std::make_unique<zetta::cluster::Cluster>(n.id, config, n.repl.get());
        n.cluster->start();
        n.alive = true;
    }

    void writer_loop(ChaosNode& n) {
        const std::string blob(options_.blob_bytes, 'x');
        std::uint64_t seq = 0;
        while (!stop_writers_) {
            if (load_) {
                const std::lock_guard lock(n.mutex);
                if (n.alive) {
                    std::uniform_int_distribution<int> key_pick(0, options_.keys_per_node - 1);
                    const std::string key =
                        std::format("chaos:{}:{}", n.index, key_pick(n.writer_rng));
                    const bool remove =
                        std::uniform_real_distribution<double>(0.0, 1.0)(n.writer_rng) <
                        options_.remove_ratio;
                    if (remove) {
                        n.repl->remove(key);
                        n.ledger[key] = std::nullopt;
                    } else {
                        ++seq;
                        n.repl->set(key, {{"w", n.index}, {"seq", seq}, {"blob", blob}});
                        n.ledger[key] = seq;
                    }
                    n.writes.fetch_add(1);
                }
            }
            sleep_for_ms(options_.write_interval_ms);
        }
    }

    ChaosOptions options_;
    std::vector<std::unique_ptr<ChaosNode>> nodes_;
    std::map<int, std::vector<std::byte>> killed_blob_;
    std::vector<std::thread> writers_;
    std::atomic<bool> load_{false};
    std::atomic<bool> stop_writers_{false};
};

// --- Running a schedule -------------------------------------------------------

struct StepResult {
    bool fault_landed = false;
    std::string fault_evidence;  // what proves (or fails to prove) the fault happened
    bool healed = false;
    std::string failure;         // the invariant that did not hold at the deadline
    long heal_ms = -1;           // time from heal to the invariant holding
    // #835 signature: links full everywhere, the other nodes agree, and one node's
    // document never moved during the whole deadline.
    bool frozen_node = false;
    int frozen_index = -1;
    // Frames each node's read pump handed the injector between the heal and the
    // deadline (only with the injector installed). A frozen node whose count kept
    // rising is receiving frames and failing to apply them; one whose count stopped
    // is not being delivered frames at all.
    std::vector<std::uint64_t> received_after_heal;
};

// Run one step: load on, inject, hold the window, prove the fault landed, heal,
// load off, then poll the heal invariant patiently until the deadline — the clock
// starts at the heal, not at the injection.
inline StepResult run_step(ChaosCluster& cluster, const ChaosStep& step) {
    StepResult result;
    const int n = cluster.size();
    const auto before = cluster.view(step.target).counters;
    const std::uint64_t writes_before = cluster.total_writes();
    cluster.set_load(true);
    switch (step.kind) {
        case FaultKind::LoadOnly:
            sleep_for_ms(step.window_ms);
            break;
        case FaultKind::KillRestart: {
            cluster.kill(step.target);
            const bool noticed = ChaosCluster::poll_until(milliseconds(10000), [&] {
                for (int i = 0; i < n; ++i) {
                    if (i == step.target) continue;
                    if (cluster.view(i).peers >= static_cast<std::size_t>(n - 1)) {
                        return false;
                    }
                }
                return true;
            });
            sleep_for_ms(step.window_ms);
            const bool restarted = cluster.restart(step.target);
            result.fault_landed = noticed && restarted;
            result.fault_evidence =
                std::format("survivors saw the node leave: {}; restarted on its port: {}",
                            noticed, restarted);
            break;
        }
        case FaultKind::PartitionOneWay:
        case FaultKind::PartitionSymmetric:
            cluster.partition(step.target,
                              step.kind == FaultKind::PartitionSymmetric);
            sleep_for_ms(step.window_ms);
            break;
        case FaultKind::FrameDrop:
        case FaultKind::FrameDelay:
        case FaultKind::FrameDuplicate:
        case FaultKind::FrameReorder:
        case FaultKind::TimingSkew:
            cluster.set_frames(step.target, step.frames);
            sleep_for_ms(step.window_ms);
            break;
    }
    const auto after = cluster.view(step.target).counters;
    const std::uint64_t writes = cluster.total_writes() - writes_before;
    cluster.heal_all();
    cluster.set_load(false);
    const auto healed_at = Clock::now();

    const auto moved = [&](std::uint64_t FrameFaultCounters::*field) {
        return after.*field - before.*field;
    };
    switch (step.kind) {
        case FaultKind::LoadOnly:
            result.fault_landed = writes > 0;
            result.fault_evidence = std::format("{} writes during the window", writes);
            break;
        case FaultKind::KillRestart:
            break;  // recorded above
        case FaultKind::PartitionOneWay:
        case FaultKind::PartitionSymmetric:
            result.fault_landed = moved(&FrameFaultCounters::dropped) > 0;
            result.fault_evidence = std::format(
                "{} frames from its peers discarded on node {}",
                moved(&FrameFaultCounters::dropped), step.target);
            break;
        case FaultKind::FrameDrop:
            result.fault_landed = moved(&FrameFaultCounters::dropped) > 0;
            result.fault_evidence =
                std::format("{} frames dropped", moved(&FrameFaultCounters::dropped));
            break;
        case FaultKind::FrameDelay:
        case FaultKind::TimingSkew:
            result.fault_landed = moved(&FrameFaultCounters::delayed) > 0;
            result.fault_evidence =
                std::format("{} frames held", moved(&FrameFaultCounters::delayed));
            break;
        case FaultKind::FrameDuplicate:
            result.fault_landed = moved(&FrameFaultCounters::duplicated) > 0;
            result.fault_evidence = std::format(
                "{} frames duplicated", moved(&FrameFaultCounters::duplicated));
            break;
        case FaultKind::FrameReorder:
            result.fault_landed = moved(&FrameFaultCounters::reordered) > 0;
            result.fault_evidence = std::format(
                "{} frames reordered", moved(&FrameFaultCounters::reordered));
            break;
    }
    if (step.kind != FaultKind::LoadOnly && step.kind != FaultKind::KillRestart) {
        result.fault_evidence += std::format(
            " of {} frames received on node {}", moved(&FrameFaultCounters::received),
            step.target);
    }
    if (step.kind != FaultKind::LoadOnly && writes == 0) {
        result.fault_landed = false;
        result.fault_evidence += "; but no writes flowed during the window";
    }

    // Patient poll to the deadline, tracking when each node's version last moved
    // so a frozen document (the #835 signature) can be told from a slow one.
    std::vector<std::string> last_version(n);
    std::vector<Clock::time_point> last_change(n, healed_at);
    std::vector<std::uint64_t> received_at_heal(n);
    for (int i = 0; i < n; ++i) received_at_heal[i] = cluster.view(i).counters.received;
    const auto until = healed_at + cluster.options().converge_deadline;
    std::string failure;
    for (;;) {
        failure = cluster.check_healed();
        if (failure.empty()) break;
        for (int i = 0; i < n; ++i) {
            std::string version = cluster.view(i).version;
            if (version != last_version[i]) {
                last_version[i] = std::move(version);
                last_change[i] = Clock::now();
            }
        }
        if (Clock::now() >= until) break;
        sleep_for_ms(100);
    }
    if (failure.empty()) {
        result.healed = true;
        result.heal_ms = static_cast<long>(
            std::chrono::duration_cast<milliseconds>(Clock::now() - healed_at).count());
        return result;
    }
    result.failure = failure;
    // Is it the #835 shape? Every link up, n-1 nodes agree, one node never moved.
    std::vector<NodeView> views;
    for (int i = 0; i < n; ++i) views.push_back(cluster.view(i));
    for (int i = 0; i < n; ++i) {
        result.received_after_heal.push_back(views[i].counters.received -
                                             received_at_heal[i]);
    }
    const bool links_full = std::ranges::all_of(views, [&](const NodeView& v) {
        return v.alive && v.peers == static_cast<std::size_t>(n - 1);
    });
    for (int i = 0; links_full && i < n; ++i) {
        bool others_agree = true;
        std::string other_version;
        for (int j = 0; j < n; ++j) {
            if (j == i) continue;
            if (other_version.empty()) other_version = views[j].version;
            if (views[j].version != other_version) others_agree = false;
        }
        if (others_agree && views[i].version != other_version &&
            Clock::now() - last_change[i] >= cluster.options().converge_deadline / 2) {
            result.frozen_node = true;
            result.frozen_index = i;
        }
    }
    return result;
}

}  // namespace zetta::chaos
