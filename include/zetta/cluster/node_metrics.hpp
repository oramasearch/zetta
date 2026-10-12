#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "zetta/data/storage_census.hpp"

namespace zetta::cluster {

// A live snapshot of one node's host resource utilization — CPU, memory, GPU +
// VRAM, storage, network. This is the engine telemetry source behind the
// read-only, **ephemeral** `system.node_metrics` table (the animated per-node
// gauges of #575 area 2). It is **observed state, never replicated**: it is
// sampled straight from the host's OS counters at query time, never stored or
// gossiped (like `system.cloud_status` / `system.checkpoints`).
//
// A node measures only **its own** host, so a query on one node reports that
// node's row. Counters a host can't report **degrade gracefully**: each group
// carries an `*_available` flag, and the `system.node_metrics` projection emits
// `null` for an unavailable group's values rather than a misleading zero.
//
// Notes per group:
//  - CPU: `cpu_cores` (logical cores) is always knowable; `cpu_percent`
//    (0..100 across all logical cores) is a **delta** measurement — it needs a
//    previous sample, so the very first sample after baseline may report it
//    unavailable until an interval has elapsed.
//  - Memory: physical RAM used / total (used = total − available).
//  - GPU + VRAM: **absent in zetta-core** (CPU-only; no CUDA/NVML is allowed to
//    leak into the core), so `gpu_available` is false here — a future
//    accelerator-telemetry binding fills it behind this same struct.
//  - Storage: the filesystem backing the node's `data_dir`.
//  - Network: rx/tx **bytes per second** since the previous sample. Utilization
//    (`net_percent`) needs a link-capacity reference that isn't reliably
//    available cross-platform, so it degrades to unavailable by default.
struct NodeMetrics {
    // CPU
    unsigned cpu_cores = 0;            // logical cores (0 ⇒ unknown)
    bool cpu_percent_available = false;
    double cpu_percent = 0.0;         // 0..100 across all logical cores

    // Memory (physical RAM)
    bool memory_available = false;
    std::uint64_t memory_used_bytes = 0;
    std::uint64_t memory_total_bytes = 0;
    double memory_percent = 0.0;      // 0..100

    // This zetta process and its allocator/probe breakdown.
    bool process_memory_available = false;
    std::uint64_t process_rss_bytes = 0;
    std::uint64_t process_peak_rss_bytes = 0;
    bool allocator_memory_available = false;
    std::uint64_t allocator_in_use_bytes = 0;
    std::uint64_t allocator_free_held_bytes = 0;
    std::uint64_t attributed_memory_bytes = 0;
    std::uint64_t unattributed_memory_bytes = 0;
    double memory_explained_percent = 0.0;

    // GPU + VRAM (degrades gracefully — absent in a CPU-only core build/host)
    bool gpu_available = false;
    double gpu_percent = 0.0;         // 0..100
    std::uint64_t vram_used_bytes = 0;
    std::uint64_t vram_total_bytes = 0;

    // Storage (the node's data_dir filesystem)
    bool storage_available = false;
    std::uint64_t storage_used_bytes = 0;
    std::uint64_t storage_total_bytes = 0;
    double storage_percent = 0.0;     // 0..100

    // Network (rates since the previous sample)
    bool network_available = false;
    std::uint64_t net_rx_bytes_per_sec = 0;
    std::uint64_t net_tx_bytes_per_sec = 0;
    bool net_utilization_available = false;
    double net_percent = 0.0;         // 0..100 link utilization

    std::string sampled_at;           // ISO-8601 UTC stamp of this sample

    // Storage census (zetta#1259, docs/data/storage-census.md): this node's disk by kind. Walking
    // the data directory is blocking work, so the node measures it on the blocking pool at a
    // bounded interval (Node::kStorageCensusInterval) and every sample carries the latest one;
    // absent until the first measurement. Not part of `system.node_metrics`.
    bool storage_census_available = false;
    zetta::data::StorageCensus storage_census;
    std::string storage_census_measured_at;  // ISO-8601 UTC, by the measuring node's clock
    double storage_census_age_seconds = 0.0;  // how old the census was when this sample was taken
    // Buckets beyond the wire's cap (kMaxWireCensusBuckets): counted in `kinds.buckets` and
    // `total_bytes`, not listed in `storage_census.buckets`.
    std::uint64_t storage_census_buckets_omitted = 0;
};

// A per-node, stateful sampler of `NodeMetrics`. The CPU and network gauges are
// **rates** computed from the delta between consecutive samples, so the sampler
// holds the previous reading; absolute gauges (cores, memory, storage) need no
// history. It seeds a baseline on construction so the first `sample()` after any
// real elapsed time already has an interval to measure over.
//
// Thread-safe: `sample()` is guarded by an internal mutex, so it may be called
// from the query thread (a SELECT samples on read) and the render loop alike.
// It holds **no platform types** — the OS-specific reads live in the .cpp — so
// this header stays free of <mach/*> / <sys/*> and safe to include widely (e.g.
// from `Node`).
class NodeMetricsSampler {
public:
    NodeMetricsSampler();

    // Sample the host's live counters now. `storage_path` selects the filesystem
    // for the storage gauge (the node's data_dir). Never throws; an unreadable
    // counter group leaves its `*_available` flag false.
    NodeMetrics sample(const std::string& storage_path);

private:
    std::mutex mutex_;

    bool have_cpu_baseline_ = false;
    std::uint64_t prev_cpu_total_ = 0;  // total jiffies/ticks across all states
    std::uint64_t prev_cpu_busy_ = 0;   // non-idle jiffies/ticks

    bool have_net_baseline_ = false;
    std::uint64_t prev_net_rx_ = 0;     // cumulative received bytes
    std::uint64_t prev_net_tx_ = 0;     // cumulative transmitted bytes
    std::chrono::steady_clock::time_point prev_net_time_{};
};

}  // namespace zetta::cluster
