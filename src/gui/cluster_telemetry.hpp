// SPDX-License-Identifier: MIT

#pragma once

// GUI-internal animation + sparkline state for the cluster node-box gauges
// (#584). It is a thin renderer's working copy of the engine's
// `Node::telemetry_snapshot()` (system.node_metrics gathered cluster-wide over the
// signal plane — see docs/gui/cluster-node-gauges.md): the engine owns the data; this
// only holds what's needed to ANIMATE it (an eased value per gauge so the bars
// glide between the ~1 Hz samples, plus a short history ring for the sparklines).
//
// Pure data — no ImGui/SDL here, so app.hpp may include it without pulling the
// GUI toolkit into every translation unit. The drawing lives in
// src/gui/app_operations.cpp.

#include <array>
#include <string>

namespace zetta::gui {

// The per-node utilization gauges, in display order: CPU, memory, GPU + VRAM,
// storage, network throughput. Mirrors the system.node_metrics groups (#575
// area 2) the cluster node boxes animate.
enum class Gauge { Cpu, Memory, Gpu, Storage, Network };
inline constexpr int kGaugeCount = 5;

// A short ring buffer of recent gauge readings (0..100), drawn as a sparkline
// behind each gauge so the trend reads at a glance. One point is pushed per
// underlying telemetry sample (~1 Hz); the oldest rolls off once full.
struct Sparkline {
    static constexpr int kCapacity = 64;
    std::array<float, kCapacity> values{};
    int count = 0;  // valid samples (<= kCapacity)
    int head = 0;   // next write slot

    void push(float value) {
        values[head] = value;
        head = (head + 1) % kCapacity;
        if (count < kCapacity) ++count;
    }
    // The i-th sample in time order (0 = oldest valid … count-1 = newest).
    float at(int index) const {
        const int start = (head - count + 2 * kCapacity) % kCapacity;
        return values[(start + index) % kCapacity];
    }
};

// Live, animated gauge state for ONE node box. Per gauge it holds the live
// sampled target (0..100), an eased "shown" value (so the bar GLIDES between the
// throttled samples — the animation), an availability flag (an unreportable
// counter renders n/a, never a misleading 0), the sparkline history, and two
// read-out strings (compact for the box, fuller for the detail pane). `net_peak`
// is the rolling peak throughput the relative network "flow" bar normalizes
// against; `last_sampled_at` is the sample stamp the history was last advanced
// for, so a fresh sample pushes exactly one sparkline point.
struct NodeGaugeState {
    std::array<float, kGaugeCount> target{};
    std::array<float, kGaugeCount> shown{};
    std::array<bool, kGaugeCount> available{};
    std::array<Sparkline, kGaugeCount> history;
    std::array<std::string, kGaugeCount> box_text;     // compact box read-out
    std::array<std::string, kGaugeCount> detail_text;  // fuller detail read-out
    float net_peak = 1.0f;
    std::string last_sampled_at;
    bool have_data = false;
};

}  // namespace zetta::gui
