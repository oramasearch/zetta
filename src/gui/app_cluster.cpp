#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"
#include "gui/cluster_pipeline.hpp"
#include "gui/cluster_telemetry.hpp"
#include "gui/tones.hpp"

#include "zetta/cloud/add_nodes.hpp"
#include "zetta/cloud/cloud_provider.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/cloud/provisioner_detect.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/bytes.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zetta::gui {

// Shared state for the in-flight async "Add nodes" op (forward-declared in
// app.hpp). The provision coroutine writes `result` under `mutex` and the
// completion flips `done`; the UI thread polls `done` each frame, applies the
// result on its own thread, and clears the op (poll_add_nodes). Held by shared_ptr
// so the coroutine + its completion keep it alive independent of the App field.
struct AddNodesOp {
    std::string target_id;
    std::string target_name;
    std::size_t requested = 0;
    zetta::cloud::Provisioner* provisioner = nullptr;  // read only by the coroutine (no Node touch)
    std::chrono::steady_clock::time_point started;

    std::mutex mutex;                       // guards `result`
    std::optional<zetta::cloud::AddNodesResult> result;   // set by the coroutine before `done`
    std::atomic<bool> done{false};          // flipped by the spawn completion
};

// Shared state for the async VM-size dropdown fetch (forward-declared in app.hpp).
// The coroutine runs `az vm list-skus` for `location` off the UI thread and writes
// `result` under `mutex`; poll_add_nodes reads `done` and copies the options out.
struct VmSizesOp {
    std::string subscription;
    std::string location;
    std::mutex mutex;
    std::vector<std::string> result;
    std::atomic<bool> done{false};
};

// Defined below (near run_add_nodes); forward-declared so draw_add_nodes_control
// can spawn it.
zetta::runtime::task<> fetch_vm_sizes_task(std::shared_ptr<VmSizesOp> op);

namespace {

// The health-dot colour for a node: the shared node-sync tone (#636), so the
// node boxes, the detail pane, and the graph workspace canvas all agree.
ImVec4 cluster_node_color(const zetta::cluster::NodeStatus& node) {
    return tone_color(tone_of(node));
}

// A short one-word sync state for the box caption.
const char* cluster_node_state(const zetta::cluster::NodeStatus& node) {
    if (node.is_self) return "self";
    if (!node.replication.known) return "no status";
    return node.replication.in_sync ? "in sync" : "syncing";
}

// ── Per-node resource gauges (#584) ───────────────────────────────────────
// Animated CPU / memory / GPU / storage / network gauges + sparklines rendered on
// each cluster node box, driven by the cluster-wide telemetry the signal-plane
// exchange gathers (Node::telemetry_snapshot()). The boxes show the live, eased
// bars; the detail pane shows the same gauges with fuller read-outs.

constexpr ImU32 kGaugeTrack = IM_COL32(236, 236, 234, 255);  // light meter track
constexpr ImU32 kGaugeSpark = IM_COL32(95, 95, 92, 140);     // faint ink trend line

// A load gauge's fill: the shared utilization thresholds (#636) on the
// semantic status palette (never coral). The gauges keep their richer
// treatment (eased bars, sparklines) but the colour breakpoints are the
// system-wide ones. Network throughput is a flow, not a good/bad status, so
// it uses a neutral ink fill instead of this.
ImVec4 gauge_load_color(float percent) {
    return tone_color(tone_of_utilization(percent));
}

// One gauge's derived display: the live value (0..100), whether it is available
// (an unreportable counter degrades to n/a, never a misleading 0), and the compact
// (box) + fuller (detail-pane) read-out strings.
struct GaugeReading {
    float value = 0.0f;
    bool available = false;
    std::string box_text;
    std::string detail_text;
};

// A compact bytes/sec read-out for a box gauge (e.g. "1.8M", "320k", "0") — short
// enough to sit at the right of a small meter. The detail pane shows the full
// per-direction rate instead.
std::string short_rate(std::uint64_t bytes_per_sec) {
    char buffer[16];
    if (bytes_per_sec >= 1000ull * 1000 * 1000) {
        std::snprintf(buffer, sizeof(buffer), "%.1fG",
                      static_cast<double>(bytes_per_sec) / 1e9);
    } else if (bytes_per_sec >= 1000ull * 1000) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM",
                      static_cast<double>(bytes_per_sec) / 1e6);
    } else if (bytes_per_sec >= 1000) {
        std::snprintf(buffer, sizeof(buffer), "%.0fk",
                      static_cast<double>(bytes_per_sec) / 1e3);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu",
                      static_cast<unsigned long long>(bytes_per_sec));
    }
    return buffer;
}

// Derive the five gauge readings from a node's metrics. `net_peak` is the rolling
// peak throughput the network "flow" bar normalizes against — updated here once
// per sample, decayed so a burst scales the window and then relaxes, so an idle
// link doesn't paint a full bar. The load gauges (CPU/MEM/GPU/STORAGE) carry a
// true 0..100 utilization; the network gauge carries the relative flow.
std::array<GaugeReading, kGaugeCount> derive_gauges(const zetta::cluster::NodeMetrics& metrics,
                                                    float& net_peak) {
    std::array<GaugeReading, kGaugeCount> gauges;
    char buffer[96];
    const auto percent_text = [&](double value) {
        std::snprintf(buffer, sizeof(buffer), "%.0f%%", value);
        return std::string(buffer);
    };

    // CPU — utilization across all logical cores; the detail shows the core count.
    GaugeReading& cpu = gauges[static_cast<int>(Gauge::Cpu)];
    cpu.available = metrics.cpu_percent_available;
    cpu.value = static_cast<float>(metrics.cpu_percent);
    cpu.box_text = cpu.available ? percent_text(metrics.cpu_percent) : "n/a";
    {
        std::string detail;
        if (metrics.cpu_cores > 0) {
            detail = std::to_string(metrics.cpu_cores) + " cores · ";
        }
        detail += cpu.available ? percent_text(metrics.cpu_percent) : "n/a";
        cpu.detail_text = detail;
    }

    // Memory — physical RAM used / total.
    GaugeReading& mem = gauges[static_cast<int>(Gauge::Memory)];
    mem.available = metrics.memory_available;
    mem.value = static_cast<float>(metrics.memory_percent);
    mem.box_text = mem.available ? percent_text(metrics.memory_percent) : "n/a";
    mem.detail_text =
        mem.available
            ? zetta::util::format_bytes(metrics.memory_used_bytes) + " / " +
                  zetta::util::format_bytes(metrics.memory_total_bytes) + " · " +
                  percent_text(metrics.memory_percent)
            : "n/a";

    // GPU + VRAM — absent in a CPU-only core build (graceful n/a).
    GaugeReading& gpu = gauges[static_cast<int>(Gauge::Gpu)];
    gpu.available = metrics.gpu_available;
    gpu.value = static_cast<float>(metrics.gpu_percent);
    gpu.box_text = gpu.available ? percent_text(metrics.gpu_percent) : "n/a";
    gpu.detail_text =
        gpu.available
            ? percent_text(metrics.gpu_percent) + " · " +
                  zetta::util::format_bytes(metrics.vram_used_bytes) + " / " +
                  zetta::util::format_bytes(metrics.vram_total_bytes)
            : "no GPU on this host";

    // Storage — the data_dir filesystem used / total.
    GaugeReading& storage = gauges[static_cast<int>(Gauge::Storage)];
    storage.available = metrics.storage_available;
    storage.value = static_cast<float>(metrics.storage_percent);
    storage.box_text =
        storage.available ? percent_text(metrics.storage_percent) : "n/a";
    storage.detail_text =
        storage.available
            ? zetta::util::format_bytes(metrics.storage_used_bytes) + " / " +
                  zetta::util::format_bytes(metrics.storage_total_bytes) + " · " +
                  percent_text(metrics.storage_percent)
            : "n/a";

    // Network — in/out throughput, shown as a relative "flow" against a decaying
    // rolling peak (no reliable link-capacity reference cross-platform).
    GaugeReading& net = gauges[static_cast<int>(Gauge::Network)];
    net.available = metrics.network_available;
    if (net.available) {
        const double total = static_cast<double>(metrics.net_rx_bytes_per_sec) +
                             static_cast<double>(metrics.net_tx_bytes_per_sec);
        net_peak *= 0.92f;  // relax the window so an old burst doesn't pin the bar
        if (static_cast<float>(total) > net_peak) net_peak = static_cast<float>(total);
        if (net_peak < 1.0f) net_peak = 1.0f;
        net.value = std::clamp(static_cast<float>(total) / net_peak * 100.0f, 0.0f,
                               100.0f);
        net.box_text = short_rate(metrics.net_rx_bytes_per_sec +
                                  metrics.net_tx_bytes_per_sec);
        net.detail_text = "in " + zetta::util::format_bytes(metrics.net_rx_bytes_per_sec) +
                          "/s · out " + util::format_bytes(metrics.net_tx_bytes_per_sec) +
                          "/s";
    } else {
        net.box_text = "n/a";
        net.detail_text = "n/a";
    }
    return gauges;
}

// Advance one node's animated gauge state from its latest telemetry sample: on a
// NEW underlying sample re-derive the targets/captions and push one point onto
// each sparkline (the sparkline = the sampled history); every frame ease the
// shown value toward the target by `dt`, so the bars GLIDE between the ~1 Hz
// samples (the animation) — frame-rate-independent via an exponential approach.
void update_node_gauges(NodeGaugeState& state, const zetta::cluster::NodeMetrics& metrics,
                        float dt) {
    if (!state.have_data || metrics.sampled_at != state.last_sampled_at) {
        state.last_sampled_at = metrics.sampled_at;
        const std::array<GaugeReading, kGaugeCount> readings =
            derive_gauges(metrics, state.net_peak);
        for (int gauge = 0; gauge < kGaugeCount; ++gauge) {
            state.target[gauge] =
                readings[gauge].available ? readings[gauge].value : 0.0f;
            state.available[gauge] = readings[gauge].available;
            state.box_text[gauge] = readings[gauge].box_text;
            state.detail_text[gauge] = readings[gauge].detail_text;
            if (readings[gauge].available) {
                state.history[gauge].push(state.target[gauge]);
            }
            if (!state.have_data) state.shown[gauge] = state.target[gauge];
        }
        state.have_data = true;
    }
    const float ease = 1.0f - std::exp(-dt * 9.0f);
    for (int gauge = 0; gauge < kGaugeCount; ++gauge) {
        const float goal = state.available[gauge] ? state.target[gauge] : 0.0f;
        state.shown[gauge] += (goal - state.shown[gauge]) * ease;
    }
}

// Draw one compact animated gauge at `pos` spanning `total_w` × `row_h`: a rounded
// track, the recent history as a faint sparkline, an eased fill bar to
// `shown_percent` (the moving part), a left mono label, and a right read-out (the
// `right_w` column). Unavailable gauges render as a faint n/a track. Pure draw —
// no ImGui layout, so it works both inside the node-box canvas (absolute coords)
// and in the detail pane (cursor-derived coords).
void draw_node_gauge(ImDrawList* draw, ImFont* mono, float font, ImVec2 pos,
                     float total_w, float row_h, float right_w, const char* label,
                     float shown_percent, const Sparkline& history, bool available,
                     ImVec4 fill, const std::string& right_text) {
    const float label_w = font * 2.4f;
    const float gap = font * 0.35f;
    const float meter_x0 = pos.x + label_w;
    const float meter_x1 = pos.x + total_w - right_w - gap;
    const float meter_w = std::max(font * 1.0f, meter_x1 - meter_x0);
    const float meter_h = std::min(row_h - font * 0.18f, font * 0.9f);
    const float meter_y0 = pos.y + (row_h - meter_h) * 0.5f;
    const float meter_y1 = meter_y0 + meter_h;
    const float rounding = meter_h * 0.32f;
    const float text_size = font * 0.85f;

    // Label (mono, muted), vertically centered.
    const ImVec2 label_pos(pos.x, pos.y + (row_h - text_size) * 0.5f);
    if (mono) {
        draw->AddText(mono, text_size, label_pos, kClusterBoxMuted, label);
    } else {
        draw->AddText(label_pos, kClusterBoxMuted, label);
    }

    const ImVec2 meter_min(meter_x0, meter_y0);
    const ImVec2 meter_max(meter_x1, meter_y1);
    draw->AddRectFilled(meter_min, meter_max, kGaugeTrack, rounding);

    if (available) {
        // Fill bar (the eased gauge), drawn first at moderate alpha so the trend
        // line reads over it.
        const float fraction = std::clamp(shown_percent / 100.0f, 0.0f, 1.0f);
        const ImU32 fill_soft = ImGui::GetColorU32(
            ImVec4(fill.x, fill.y, fill.z, 0.60f));
        if (fraction > 0.0f) {
            draw->AddRectFilled(meter_min,
                                ImVec2(meter_x0 + fraction * meter_w, meter_y1),
                                fill_soft, rounding);
        }
        // Sparkline (the sampled history), over the fill — oldest → newest.
        if (history.count >= 2) {
            std::vector<ImVec2> points;
            points.reserve(static_cast<std::size_t>(history.count));
            for (int index = 0; index < history.count; ++index) {
                const float x =
                    meter_x0 + meter_w * static_cast<float>(index) /
                                   static_cast<float>(history.count - 1);
                const float value =
                    std::clamp(history.at(index), 0.0f, 100.0f) / 100.0f;
                points.emplace_back(x, meter_y1 - value * meter_h);
            }
            draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                              kGaugeSpark, 0, 1.4f);
        }
    } else if (mono) {
        draw->AddText(mono, text_size, ImVec2(meter_x0 + gap, label_pos.y),
                      kClusterBoxMuted, "n/a");
    }
    draw->AddRect(meter_min, meter_max, kClusterBoxBorder, rounding);

    // Right read-out (mono), right-aligned, colored by the gauge (muted for n/a).
    const ImU32 text_color =
        available ? ImGui::GetColorU32(fill) : kClusterBoxMuted;
    float text_w = ImGui::CalcTextSize(right_text.c_str()).x;
    if (mono) {
        text_w = mono->CalcTextSizeA(text_size, FLT_MAX, 0.0f,
                                     right_text.c_str())
                     .x;
    }
    const ImVec2 right_pos(pos.x + total_w - text_w, label_pos.y);
    if (mono) {
        draw->AddText(mono, text_size, right_pos, text_color, right_text.c_str());
    } else {
        draw->AddText(right_pos, text_color, right_text.c_str());
    }
}

// The five gauges, in display order — labels for the node box + detail pane.
constexpr std::array<const char*, kGaugeCount> kGaugeLabels{"CPU", "MEM", "GPU",
                                                            "DSK", "NET"};

// ── Distributed-algorithm-at-work overlay (#590) ──────────────────────────
// The animation layer over the node-box canvas (epic #575 area 5). A thin renderer
// over the live instrumentation read views — system.dataset_partitions (placement /
// rebalance), system.pipeline_jobs (near-data vs wire bytes), system.collective_rounds
// (scatter/gather) — so the canvas visibly animates a live run without inventing any
// number. update_cluster_pipeline() diffs a per-frame snapshot into brief flows;
// draw_pipeline_flow() glides them over the boxes. See docs/gui/cluster-pipeline-animation.md.
// The flow palette (kFlowLocal / kFlowWire / kFlowMove) and the drawing itself
// live in app_operations_internal.hpp, shared with the Search view's cluster
// strip (#643).

// Diff the live instrumentation snapshot against the last-seen baseline, spawning a
// brief animated flow per change — a placed / moved partition, a near-data job's
// local read (+ its wire partials), a completed collective round — and advance the
// in-flight flows + per-node activity by `dt`. Pure bookkeeping over the read views:
// no numbers are invented, and a change with no instrumentation behind it spawns
// nothing. Bounded — at most kMaxSpawnPerUpdate new flows per call (so a burst, e.g.
// returning to the view after a long run, folds into the baseline instead of
// replaying), the flow list capped, and the diff maps pruned to the snapshot — so it
// stays within the render_frame / SDL_WaitEventTimeout cadence (never starves a
// resize). The FIRST call only primes the baseline (spawns nothing).
void update_cluster_pipeline(ClusterPipelineState& state, const PipelineInputs& in,
                             float dt) {
    // Advance + cull in-flight flows; decay node activity + the collective pacing.
    for (PipelineFlow& flow : state.flows) flow.age += dt;
    std::erase_if(state.flows,
                  [](const PipelineFlow& f) { return f.age >= f.lifetime; });
    const float decay = std::exp(-dt * 1.8f);
    for (auto& [id, level] : state.node_activity) level *= decay;
    std::erase_if(state.node_activity,
                  [](const auto& e) { return e.second < 0.01f; });
    if (state.collective_cooldown > 0.0f) state.collective_cooldown -= dt;
    state.peak_bytes *= 0.985;  // relax the byte-weight reference so an idle link is light
    if (state.peak_bytes < 1.0) state.peak_bytes = 1.0;

    int budget = state.primed ? ClusterPipelineState::kMaxSpawnPerUpdate : 0;

    const auto member = [&](const std::string& id) {
        return !id.empty() &&
               std::find(in.nodes.begin(), in.nodes.end(), id) != in.nodes.end();
    };
    const auto bump = [&](const std::string& id, float amount) {
        if (id.empty()) return;
        float& level = state.node_activity[id];
        level = std::min(1.0f, level + amount);
    };
    const auto spawn = [&](PipelineFlow flow) {
        if (state.flows.size() >= ClusterPipelineState::kMaxFlows) {
            state.flows.erase(state.flows.begin());
        }
        bump(flow.from, 0.5f);
        bump(flow.to, 0.6f);
        state.flows.push_back(std::move(flow));
    };
    const auto byte_weight = [&](std::uint64_t bytes) {
        if (static_cast<double>(bytes) > state.peak_bytes) {
            state.peak_bytes = static_cast<double>(bytes);
        }
        return static_cast<float>(
            std::clamp(static_cast<double>(bytes) / state.peak_bytes, 0.15, 1.0));
    };
    const auto row_weight = [](std::uint64_t rows) {
        return rows ? std::clamp(static_cast<float>(rows) / 20000.0f, 0.25f, 1.0f)
                    : 0.4f;
    };

    // 1) Partitions — a new id is a placement (rows → partition → owner node); a
    //    changed owner set is a rebalance move (a partition sliding onto a new node).
    //    The baseline is only written on a new id / a change, so an unchanged catalog
    //    costs one map lookup + one vector compare per partition (no copy).
    for (const PipelinePartitionRef& part : in.partitions) {
        const auto it = state.partition_owners.find(part.id);
        if (it == state.partition_owners.end()) {
            bool placed = false;
            for (const std::string& owner : part.owners) {
                if (budget <= 0 || !member(owner)) continue;
                PipelineFlow flow;
                flow.kind = PipelineFlowKind::PartitionPlaced;
                flow.to = owner;
                flow.lifetime = 1.1f;
                flow.weight = row_weight(part.rows);
                flow.label =
                    part.rows ? (std::to_string(part.rows) + " rows") : "partition";
                spawn(std::move(flow));
                --budget;
                placed = true;
            }
            if (placed) ++state.placements_seen;  // one tally per placed partition
            state.partition_owners.emplace(part.id, part.owners);
        } else if (it->second != part.owners) {
            if (budget > 0) {
                // Pick a newly-added owner (the destination) and a dropped owner (the
                // source) so the chip visibly slides off the old node onto the new one.
                std::string to_node;
                std::string from_node;
                for (const std::string& owner : part.owners) {
                    if (member(owner) &&
                        std::find(it->second.begin(), it->second.end(), owner) ==
                            it->second.end()) {
                        to_node = owner;
                        break;
                    }
                }
                for (const std::string& owner : it->second) {
                    if (std::find(part.owners.begin(), part.owners.end(), owner) ==
                        part.owners.end()) {
                        from_node = owner;
                        break;
                    }
                }
                if (to_node.empty() && !part.owners.empty()) {
                    to_node = part.owners.front();
                }
                if (member(to_node)) {
                    PipelineFlow flow;
                    flow.kind = PipelineFlowKind::PartitionMoved;
                    flow.from = member(from_node) ? from_node : std::string{};
                    flow.to = to_node;
                    flow.lifetime = 1.3f;
                    flow.weight = row_weight(part.rows);
                    flow.label = "rebalance";
                    spawn(std::move(flow));
                    --budget;
                    ++state.moves_seen;
                }
            }
            it->second = part.owners;  // fold into the baseline (even if budget spent)
        }
    }
    // Prune the owner map to the live catalog so it stays bounded.
    if (state.partition_owners.size() > in.partitions.size()) {
        std::unordered_set<std::string> live;
        for (const PipelinePartitionRef& part : in.partitions) live.insert(part.id);
        std::erase_if(state.partition_owners,
                      [&](const auto& e) { return live.find(e.first) == live.end(); });
    }

    // 2) Near-data jobs — a newly-recorded job is a local read on its node (bytes
    //    that stayed home), plus a wire flow toward the root for the partials it moved.
    const std::string root =
        in.participants.empty() ? std::string{} : in.participants.front();
    for (const PipelineJobRef& job : in.jobs) {
        if (state.seen_jobs.find(job.id) != state.seen_jobs.end()) continue;
        state.seen_jobs[job.id] = true;
        if (budget <= 0) continue;
        if (member(job.node)) {
            PipelineFlow flow;
            flow.kind = PipelineFlowKind::NearDataRead;
            flow.to = job.node;
            flow.lifetime = 1.2f;
            flow.weight = byte_weight(job.local_bytes);
            flow.label = job.kind + " · " + util::format_bytes(job.local_bytes);
            spawn(std::move(flow));
            --budget;
        }
        if (budget > 0 && job.wire_bytes > 0 && member(job.node) && member(root) &&
            root != job.node) {
            PipelineFlow flow;
            flow.kind = PipelineFlowKind::WireMoved;
            flow.from = job.node;
            flow.to = root;
            flow.lifetime = 1.0f;
            flow.weight = byte_weight(job.wire_bytes);
            spawn(std::move(flow));
            --budget;
        }
    }
    if (state.seen_jobs.size() > in.jobs.size()) {
        std::unordered_set<std::string> live;
        for (const PipelineJobRef& job : in.jobs) live.insert(job.id);
        std::erase_if(state.seen_jobs,
                      [&](const auto& e) { return live.find(e.first) == live.end(); });
    }

    // 3) Collective rounds — a tick of rounds_completed spawns one paced scatter +
    //    gather set (root ↔ each participant), the live run's heartbeat. The cooldown
    //    caps the pulse rate to a legible ~2 Hz however fast rounds tick (the strip
    //    still shows the true count). A single-node cluster has no peer to flow to, so
    //    a round is a self compute pulse on the lone node.
    if (in.rounds_completed > state.last_rounds && state.primed &&
        state.collective_cooldown <= 0.0f && budget > 0) {
        state.collective_cooldown = 0.45f;
        if (in.participants.size() <= 1) {
            const std::string self =
                !in.participants.empty()
                    ? in.participants.front()
                    : (in.nodes.empty() ? std::string{} : in.nodes.front());
            if (member(self)) {
                PipelineFlow flow;
                flow.kind = PipelineFlowKind::LocalCompute;
                flow.to = self;
                flow.lifetime = 0.9f;
                flow.weight = 0.6f;
                spawn(std::move(flow));
            }
        } else if (member(root)) {
            for (const std::string& peer : in.participants) {
                if (budget <= 0) break;
                if (peer == root || !member(peer)) continue;
                PipelineFlow gather;
                gather.kind = PipelineFlowKind::CollectiveGather;
                gather.from = peer;
                gather.to = root;
                gather.lifetime = 0.6f;
                gather.weight = 0.55f;
                spawn(std::move(gather));
                PipelineFlow scatter;
                scatter.kind = PipelineFlowKind::CollectiveScatter;
                scatter.from = root;
                scatter.to = peer;
                scatter.age = -0.5f;  // begins after the gather leg lands
                scatter.lifetime = 0.6f;
                scatter.weight = 0.55f;
                spawn(std::move(scatter));
                --budget;
            }
        }
    }
    state.last_rounds = in.rounds_completed;
    state.last_wire_bytes = in.wire_bytes;
    state.primed = true;
}

}  // namespace

void App::draw_cluster() {
    // Consume a navigate_to(entity) deep link aimed at this view.
    if (auto pending_entity = take_pending_selection(View::Cluster)) {
        selected_cluster_node_ = *pending_entity;
    }
    ImGui::SeparatorText("Cluster");
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::TextColored(kInkSecondary, "node       %s", cluster_.node_id().c_str());
    for (const std::string& address : cluster_.listen_addresses()) {
        ImGui::TextColored(kInkSecondary, "listening  %s", address.c_str());
    }
    if (mono_font_) ImGui::PopFont();
    ImGui::TextDisabled(
        "All nodes are equal; leader election and failover are planned.");

    ImGui::TextUnformatted("Add another node with:");
    std::string command = cluster_.peer_command();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::InputText("##peer_command", &command, ImGuiInputTextFlags_ReadOnly);
    if (mono_font_) ImGui::PopFont();
    if (ImGui::Button("Copy command")) {
        ImGui::SetClipboardText(command.c_str());
    }

    draw_add_nodes_control();

    ImGui::SeparatorText("Nodes");

    // Spatial cluster view (#582): one selectable box per node in a wrapping
    // grid (the "boxes" feel), reading live from Cluster::nodes() each frame so
    // membership tracks joins/leaves/heals with no separate copy — the canvas the
    // add-nodes control, per-node gauges, and algorithm animation build on. The
    // box-draw pattern mirrors draw_graph_canvas (ImDrawList rects + an invisible
    // hit-test button), and the cost is one cheap pass per frame, so it never
    // starves a live resize.
    const std::vector<zetta::cluster::NodeStatus> nodes = cluster_.nodes();

    // Re-resolve the selection against the live membership: if the selected node
    // has left (or nothing is selected yet), fall back to self so the detail pane
    // always shows a real node and a departed peer can't dangle.
    const auto is_selected_present = [&] {
        for (const zetta::cluster::NodeStatus& node : nodes)
            if (node.node_id == selected_cluster_node_) return true;
        return false;
    };
    if (!is_selected_present()) {
        selected_cluster_node_.clear();
        for (const zetta::cluster::NodeStatus& node : nodes)
            if (node.is_self) selected_cluster_node_ = node.node_id;
    }

    // Live per-node telemetry (#584): the cluster-wide snapshot gathered over the
    // signal plane (each node broadcasts its own system.node_metrics; every node
    // caches its peers'). Each node box animates its own gauges from it; a node
    // with no sample yet (a peer that just joined) shows an "awaiting" note. Pin
    // the snapshot for the frame and index it by id for the per-box lookup.
    const std::vector<zetta::cluster::NodeTelemetrySample> telemetry = node_.telemetry_snapshot();
    std::map<std::string, const zetta::cluster::NodeTelemetrySample*> telemetry_by_id;
    for (const zetta::cluster::NodeTelemetrySample& sample : telemetry) {
        telemetry_by_id[sample.node_id] = &sample;
    }
    // Drop animation state for any node that has left the cluster, so a departed
    // peer's gauges don't dangle (the snapshot already expires its telemetry).
    std::erase_if(cluster_gauges_, [&](const auto& entry) {
        for (const zetta::cluster::NodeStatus& node : nodes)
            if (node.node_id == entry.first) return false;
        return true;
    });
    const float dt = ImGui::GetIO().DeltaTime;

    // Distributed-algorithm-at-work overlay (#590): snapshot the live instrumentation
    // read views ONCE this frame — the partition catalog (placement / rebalance), the
    // near-data pipeline jobs (local vs wire bytes), and the live coordinator's
    // collective-plane progress (rounds) — then diff them into brief animated flows
    // over the boxes. A thin renderer: every value comes from the same source as the
    // system.dataset_partitions / system.pipeline_jobs / system.collective_rounds SQL
    // views, nothing is invented. The reads are cheap (in-memory snapshots), so the
    // overlay never starves a live resize.
    PipelineInputs pipeline;
    for (const zetta::cluster::NodeStatus& node : nodes) pipeline.nodes.push_back(node.node_id);
    if (compute::JobCoordinator* coordinator = node_.job_coordinator()) {
        pipeline.attached = true;
        const zetta::cluster::Collectives& collectives = coordinator->collectives();
        pipeline.rounds_completed = collectives.rounds_completed();
        pipeline.wire_bytes = collectives.wire_bytes();
        pipeline.participants = collectives.participants();
    }
    for (const std::shared_ptr<zetta::data::DatasetPartition>& part : node_.dataset_partitions()) {
        PipelinePartitionRef ref;
        ref.id = part->id();
        ref.owners = part->owner_nodes();
        ref.rows = part->row_range().end - part->row_range().start;
        pipeline.partitions.push_back(std::move(ref));
    }
    for (const zetta::compute::PipelineJobInstrumentation& job : node_.pipeline_jobs()) {
        PipelineJobRef ref;
        ref.id = job.job_id;
        ref.node = job.node;
        ref.kind = std::string(to_string(job.kind));
        ref.local_bytes = job.local_bytes_read;
        ref.wire_bytes = job.wire_bytes_moved;
        ref.rows = job.rows_read;
        ref.succeeded = job.status == zetta::compute::JobStatus::Succeeded;
        pipeline.jobs.push_back(std::move(ref));
    }
    update_cluster_pipeline(cluster_pipeline_, pipeline, dt);

    const float font = ImGui::GetFontSize();
    const float box_w = font * 15.0f;
    const float box_h = font * 11.0f;
    const float gap = font * 0.7f;
    const float pad = font * 0.6f;
    const float rounding = font * 0.3f;

    if (ImGui::BeginChild("##cluster_canvas", ImVec2(0, font * 12.5f),
                          ImGuiChildFlags_Borders)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        // How many boxes fit across the child (at least one, even when narrow).
        const int columns =
            std::max(1, static_cast<int>((avail + gap) / (box_w + gap)));

        // Box centres for the algorithm overlay (#590) — rebuilt each frame so the
        // flows track a live resize / reflow.
        std::unordered_map<std::string, ImVec2> node_centers;

        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const zetta::cluster::NodeStatus& node = nodes[index];
            const int row = static_cast<int>(index) / columns;
            const int col = static_cast<int>(index) % columns;
            const ImVec2 top_left(origin.x + col * (box_w + gap),
                                  origin.y + row * (box_h + gap));
            const ImVec2 bottom_right(top_left.x + box_w, top_left.y + box_h);

            // Click-to-select via an invisible button (hit-test), like the graph
            // canvas. Hover/selected change only fill + border, not layout.
            ImGui::SetCursorScreenPos(top_left);
            ImGui::InvisibleButton(node.node_id.c_str(), ImVec2(box_w, box_h));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) selected_cluster_node_ = node.node_id;
            const bool selected = node.node_id == selected_cluster_node_;

            // Record the box centre + draw a soft activity halo BEHIND the box (so it
            // shows in the gaps around it) while this node is doing pipeline work
            // (#590): the activity level is bumped when a flow touches the node and
            // eased down when the work stops, so the working nodes visibly light up.
            node_centers[node.node_id] =
                ImVec2((top_left.x + bottom_right.x) * 0.5f,
                       (top_left.y + bottom_right.y) * 0.5f);
            const auto activity_it =
                cluster_pipeline_.node_activity.find(node.node_id);
            if (activity_it != cluster_pipeline_.node_activity.end() &&
                activity_it->second > 0.02f) {
                const float glow = font * 0.5f * activity_it->second;
                draw->AddRectFilled(ImVec2(top_left.x - glow, top_left.y - glow),
                                    ImVec2(bottom_right.x + glow, bottom_right.y + glow),
                                    with_alpha(kFlowLocal, activity_it->second * 0.16f),
                                    rounding + glow);
            }

            const ImU32 fill = selected
                                   ? kClusterBoxFillSelected
                                   : (hovered ? kClusterBoxFillHover
                                              : kClusterBoxFill);
            draw->AddRectFilled(top_left, bottom_right, fill, rounding);
            draw->AddRect(top_left, bottom_right,
                          selected ? kClusterBoxBorderSelected
                                   : kClusterBoxBorder,
                          rounding, 0, selected ? 2.0f : 1.0f);

            draw->PushClipRect(top_left, bottom_right, true);
            // Health dot + short id (the dot center vertically aligned with the
            // id line). Green = self/in-sync, amber = syncing, quiet = no status.
            const float dot_radius = font * 0.28f;
            const ImVec2 dot_center(top_left.x + pad + dot_radius,
                                    top_left.y + pad + font * 0.5f);
            draw->AddCircleFilled(dot_center, dot_radius,
                                  ImGui::GetColorU32(cluster_node_color(node)));
            const std::string id = entity_short_id(node.node_id) +
                                   (node.is_self ? " (self)" : "");
            const ImVec2 id_pos(dot_center.x + dot_radius + font * 0.45f,
                                top_left.y + pad);
            if (mono_font_) {
                draw->AddText(mono_font_, font, id_pos, kClusterBoxText,
                              id.c_str());
            } else {
                draw->AddText(id_pos, kClusterBoxText, id.c_str());
            }
            // Address + a one-word sync state below it.
            const float small = font * 0.85f;
            const ImVec2 addr_pos(top_left.x + pad, top_left.y + pad + font * 1.5f);
            if (mono_font_) {
                draw->AddText(mono_font_, small, addr_pos, kClusterBoxMuted,
                              node.address.c_str());
            } else {
                draw->AddText(addr_pos, kClusterBoxMuted, node.address.c_str());
            }
            const ImVec2 state_pos(top_left.x + pad,
                                   top_left.y + pad + font * 2.65f);
            const ImU32 state_color = ImGui::GetColorU32(cluster_node_color(node));
            if (mono_font_) {
                draw->AddText(mono_font_, small, state_pos, state_color,
                              cluster_node_state(node));
            } else {
                draw->AddText(state_pos, state_color, cluster_node_state(node));
            }

            // Live resource gauges (#584): five animated bars + sparklines per box
            // (CPU / MEM / GPU / DSK / NET), driven by this node's telemetry
            // sample. The bars ease toward the live value each frame (the
            // animation); a node with no sample yet shows an awaiting note.
            const float gauge_top = top_left.y + pad + font * 3.65f;
            const float gauge_x = top_left.x + pad;
            const float gauge_w = box_w - pad * 2.0f;
            const float gauge_row_h = font * 1.12f;
            const auto telem = telemetry_by_id.find(node.node_id);
            if (telem != telemetry_by_id.end()) {
                NodeGaugeState& gauges = cluster_gauges_[node.node_id];
                update_node_gauges(gauges, telem->second->metrics, dt);
                for (int g = 0; g < kGaugeCount; ++g) {
                    const ImVec2 gauge_pos(gauge_x, gauge_top + g * gauge_row_h);
                    const ImVec4 fill = g == static_cast<int>(Gauge::Network)
                                            ? kInkMuted
                                            : gauge_load_color(gauges.shown[g]);
                    draw_node_gauge(draw, mono_font_, font, gauge_pos, gauge_w,
                                    gauge_row_h, font * 3.0f, kGaugeLabels[g],
                                    gauges.shown[g], gauges.history[g],
                                    gauges.available[g], fill, gauges.box_text[g]);
                }
            } else {
                const ImVec2 note_pos(gauge_x, gauge_top + font * 1.6f);
                if (mono_font_) {
                    draw->AddText(mono_font_, small, note_pos, kClusterBoxMuted,
                                  "awaiting telemetry");
                } else {
                    draw->AddText(note_pos, kClusterBoxMuted, "awaiting telemetry");
                }
            }
            draw->PopClipRect();
        }

        // Algorithm overlay (#590): draw the in-flight flows OVER the boxes, after
        // the per-box clip rects are balanced, so a comet can cross the gaps between
        // boxes. Each flow is positioned from the live box centres, so it tracks a
        // resize / reflow; a flow to a node that just left the cluster is skipped.
        for (const PipelineFlow& flow : cluster_pipeline_.flows) {
            draw_pipeline_flow(draw, mono_font_, font, node_centers, flow, box_h);
        }

        // Reserve the laid-out area so the child scrolls when nodes overflow.
        const int rows = (static_cast<int>(nodes.size()) + columns - 1) / columns;
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(avail, std::max(0, rows) * (box_h + gap)));
    }
    ImGui::EndChild();

    draw_pipeline_activity(pipeline);
    draw_cluster_node_detail(nodes);
}

// The live pipeline-activity strip below the cluster canvas (#590): the real
// instrumentation numbers the overlay animates, echoed verbatim (a thin renderer —
// never an invented number), plus a legend tying the flow colours to near-data
// (green, stays home) vs over-the-wire (ink). Quiet when no compute plane is
// attached. The `pipeline` snapshot was already read this frame in draw_cluster.
void App::draw_pipeline_activity(const PipelineInputs& pipeline) {
    ImGui::Spacing();
    eyebrow(mono_font_, "PIPELINE");
    if (mono_font_) ImGui::PushFont(mono_font_);
    if (pipeline.attached) {
        ImGui::TextColored(
            kInkSecondary,
            "rounds %llu  ·  wire moved %s  ·  near-data jobs %zu  ·  partitions %zu",
            static_cast<unsigned long long>(pipeline.rounds_completed),
            zetta::util::format_bytes(pipeline.wire_bytes).c_str(), pipeline.jobs.size(),
            pipeline.partitions.size());
        ImGui::TextColored(kInkQuiet,
                           "placed %llu  ·  rebalanced %llu  (since this view opened)",
                           static_cast<unsigned long long>(
                               cluster_pipeline_.placements_seen),
                           static_cast<unsigned long long>(
                               cluster_pipeline_.moves_seen));
    } else {
        ImGui::TextColored(kInkQuiet,
                           "no compute plane attached (a standalone node)");
    }
    if (mono_font_) ImGui::PopFont();

    // Legend swatches — drawn on the window draw list so they don't depend on a
    // bullet glyph being present in the embedded font.
    const auto swatch = [&](ImU32 color, const char* label) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float size = ImGui::GetFontSize() * 0.55f;
        const float y = pos.y + (ImGui::GetTextLineHeight() - size) * 0.5f;
        draw->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + size, y + size), color,
                            size * 0.3f);
        ImGui::Dummy(ImVec2(size + ImGui::GetStyle().ItemInnerSpacing.x,
                            ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextColored(kInkMuted, "%s", label);
    };
    swatch(kFlowLocal, "near data");
    ImGui::SameLine();
    swatch(kFlowWire, "over the wire");
    ImGui::SameLine();
    swatch(kFlowMove, "rebalance");
}

void App::draw_cluster_node_detail(const std::vector<zetta::cluster::NodeStatus>& nodes) {
    const zetta::cluster::NodeStatus* selected = nullptr;
    for (const zetta::cluster::NodeStatus& node : nodes)
        if (node.node_id == selected_cluster_node_) selected = &node;

    ImGui::Spacing();
    if (!selected) {
        ImGui::TextDisabled("Select a node to see its detail.");
        return;
    }

    eyebrow(mono_font_, "NODE");
    mono_line(kInkPrimary, "id        ", selected->node_id);
    mono_line(kInkSecondary, "role      ", to_string(selected->role));
    mono_line(kInkSecondary, "address   ", selected->address);
    mono_line(kInkSecondary, "membership",
              std::string{"  "} +
                  (selected->is_self
                       ? "this node"
                       : (selected->connected ? "connected peer" : "known")));

    // Replication status — the same facts the old table column carried, now as a
    // colour-coded line per the node's sync state.
    ImGui::Spacing();
    eyebrow(mono_font_, "REPLICATION");
    if (selected->is_self) {
        ImGui::TextDisabled("This node is the local replica.");
    } else if (!selected->replication.known) {
        ImGui::TextDisabled("No replication status learned yet.");
    } else {
        const zetta::cluster::NodeReplication& rep = selected->replication;
        mono_line(kInkSecondary, "entries   ", std::to_string(rep.entries));
        mono_line(kInkSecondary, "version   ",
                  rep.version.empty() ? "—" : rep.version);
        char sync[64];
        std::snprintf(sync, sizeof(sync), "%s (%.1fs)",
                      rep.in_sync ? "in sync" : "syncing",
                      rep.seconds_since_sync);
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::TextColored(tone_color(tone_of(*selected)), "sync      %s", sync);
        if (g_mono_font) ImGui::PopFont();
    }

    // Live resource telemetry (#584): the same five gauges as the node box, with
    // fuller per-gauge read-outs (core count / byte totals / in-out rates) and a
    // wider sparkline. Available for any selected node with a telemetry sample
    // (self or a peer, gathered cluster-wide over the signal plane); a node that
    // hasn't reported yet shows a pending note.
    ImGui::Spacing();
    eyebrow(mono_font_, "LIVE TELEMETRY");
    const auto gauges_it = cluster_gauges_.find(selected->node_id);
    if (gauges_it != cluster_gauges_.end() && gauges_it->second.have_data) {
        ImGui::Spacing();
        const NodeGaugeState& gauges = gauges_it->second;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float font = ImGui::GetFontSize();
        const float row_h = font * 1.5f;
        const float width = ImGui::GetContentRegionAvail().x;
        const float right_w = std::min(width * 0.55f, font * 14.0f);
        for (int g = 0; g < kGaugeCount; ++g) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec4 fill = g == static_cast<int>(Gauge::Network)
                                    ? kInkMuted
                                    : gauge_load_color(gauges.shown[g]);
            draw_node_gauge(draw, mono_font_, font, pos, width, row_h, right_w,
                            kGaugeLabels[g], gauges.shown[g], gauges.history[g],
                            gauges.available[g], fill, gauges.detail_text[g]);
            ImGui::Dummy(ImVec2(width, row_h));
        }
    } else {
        ImGui::TextDisabled(
            "Live telemetry pending (each node broadcasts its own resource "
            "utilization over the signal plane about once a second).");
    }

    // For this node, surface its reachable listen addresses + the live peer
    // count (membership = self + connected peers), the facts the header shows.
    if (selected->is_self) {
        ImGui::Spacing();
        eyebrow(mono_font_, "REACHABLE AT");
        for (const std::string& address : cluster_.listen_addresses())
            mono_line(kInkSecondary, "", address);
        std::size_t peers = 0;
        for (const zetta::cluster::NodeStatus& node : nodes)
            if (!node.is_self) ++peers;
        mono_line(kInkMuted, "peers     ", std::to_string(peers));
    }
}

void App::set_provisioner(const std::string& target_id, zetta::cloud::Provisioner& provisioner) {
    provisioners_[target_id] = &provisioner;
}

zetta::cloud::Provisioner* App::provisioner_for(const std::string& target_id) const {
    const auto found = provisioners_.find(target_id);
    return found == provisioners_.end() ? nullptr : found->second;
}

void App::draw_add_nodes_control() {
    // The "Add nodes" control (epic #575 area 1): drive zetta::cloud::add_nodes over an
    // injected Provisioner. The provider catalog supplies the target list + exactly
    // what each target would read/run, so the cloud consent gate can name it up
    // front (the same metadata the Cloud Inspector renders). The given-hardware
    // Local target uses the injected LocalProvisioner and runs on a plain click;
    // every cloud / Kubernetes / on-prem target is **permission-gated, deny-by-
    // default** — adding nodes there runs the provider's CLI to CREATE billable
    // resources (a write), so nothing happens until the user explicitly allows that
    // one action, and consent is never remembered (CLAUDE.md hard rule).
    ImGui::SeparatorText("Add nodes");

    const zetta::cloud::CloudProviderCatalog& catalog = zetta::cloud::CloudProviderCatalog::builtin();
    const zetta::cloud::CloudProvider* target = catalog.find(selected_add_target_);
    if (target == nullptr) {  // a stale id (none should exist) falls back to Local
        selected_add_target_ = "local";
        target = catalog.find("local");
    }
    if (target == nullptr) return;  // empty catalog — nothing to add against

    // Target selector — the same catalog the Cloud Inspector lists. A green dot
    // marks an open target (Local, no consent); amber marks a consent-gated one.
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("Target", target->name.c_str())) {
        for (const zetta::cloud::CloudProvider& provider : catalog.providers()) {
            ImGui::PushID(provider.id.c_str());
            status_dot(provider.requires_consent() ? Tone::Warn : Tone::Active);
            const bool is_selected = provider.id == selected_add_target_;
            if (ImGui::Selectable(provider.name.c_str(), is_selected)) {
                // Switching targets clears the last result line so a stale
                // "added via Local" can't linger under a different target.
                if (selected_add_target_ != provider.id) add_nodes_status_.clear();
                selected_add_target_ = provider.id;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::TextColored(kInkSecondary, "%s", target->summary.c_str());

    // How many to add. Clamp to >= 1 (a zero add is a no-op, so it isn't useful
    // from the button).
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Count", &add_nodes_count_);
    if (add_nodes_count_ < 1) add_nodes_count_ = 1;

    const bool wired = provisioner_for(target->id) != nullptr;
    const std::string plural = add_nodes_count_ == 1 ? " node" : " nodes";

    if (!target->requires_consent()) {
        // Local / given hardware — no cloud, no consent, runs on a plain click.
        ImGui::Spacing();
        ImGui::TextColored(kInkSecondary,
            "Uses the nodes already in this cluster; brings up no new machines and "
            "touches no cloud.");
        ImGui::Spacing();
        ImGui::BeginDisabled(!wired || add_nodes_op_ != nullptr);
        const std::string label =
            "Add " + std::to_string(add_nodes_count_) + plural;
        if (primary_button(label.c_str())) run_add_nodes(*target);
        ImGui::EndDisabled();
        if (!wired) {
            ImGui::TextColored(kInkQuiet,
                "No provisioner is wired for this target in this build.");
        }
    } else {
        // Cloud / Kubernetes / on-prem — permission-gated, deny-by-default. Name
        // what the provider CLI may read up front so consent is informed; the CLI
        // may read these files, zetta itself never reads or prints them.
        ImGui::Spacing();
        eyebrow(mono_font_, "THE CLI MAY READ");
        if (mono_font_) ImGui::PushFont(mono_font_);
        for (const std::string& path : target->config_paths) {
            ImGui::BulletText("%s", path.c_str());
        }
        if (mono_font_) ImGui::PopFont();

        // Editable placement — dropdowns of the detected available options. Region
        // options come from the detected config (az account list-locations); the
        // VM-size options are fetched per-region OFF the UI thread (az vm list-skus).
        // Both are applied to the injected provisioner via set_placement before the
        // add. The operator steers where a burst lands; the backend also falls back
        // across VM sizes automatically on a capacity shortage.
        // Bind the detected configs to a local — provisioner_configs() returns by
        // value, so a pointer into a temporary would dangle after the loop.
        const std::vector<zetta::cloud::ProvisionerConfig> provisioner_configs =
            node_.provisioner_configs();
        const zetta::cloud::ProvisionerConfig* detected_cfg = nullptr;
        for (const zetta::cloud::ProvisionerConfig& pc : provisioner_configs) {
            if (pc.target == target->id) {
                detected_cfg = &pc;
                break;
            }
        }

        // Pre-fill the current region/size from the detected config when the target
        // changes, and reset the size options so they re-fetch for the new target.
        if (add_nodes_placement_target_ != target->id) {
            add_nodes_placement_target_ = target->id;
            add_nodes_region_.clear();
            add_nodes_vm_size_.clear();
            add_nodes_size_options_.clear();
            add_nodes_sizes_region_.clear();
            if (detected_cfg) {
                const nlohmann::json& cfg = detected_cfg->config;
                add_nodes_region_ = cfg.value(
                    "location", cfg.value("region", cfg.value("zone", std::string{})));
                add_nodes_vm_size_ = cfg.value(
                    "vm_size",
                    cfg.value("instance_type",
                              cfg.value("machine_type",
                                        cfg.value("shape", std::string{}))));
            }
        }

        // Region changed (or first render) → fetch its VM-size options off the UI
        // thread. Marking add_nodes_sizes_region_ at launch prevents a re-launch
        // while the fetch is in flight; poll_add_nodes copies the options out.
        const std::string subscription =
            detected_cfg ? detected_cfg->config.value("subscription_id", std::string{})
                         : std::string{};
        if (target->id == "azure" && !add_nodes_region_.empty() &&
            !subscription.empty() && add_nodes_sizes_region_ != add_nodes_region_) {
            add_nodes_sizes_region_ = add_nodes_region_;
            const auto cached = add_nodes_sizes_cache_.find(add_nodes_region_);
            if (cached != add_nodes_sizes_cache_.end()) {
                add_nodes_size_options_ = cached->second;  // instant (already fetched)
                add_nodes_sizes_op_.reset();
            } else {
                add_nodes_size_options_.clear();
                auto sizes_op = std::make_shared<VmSizesOp>();
                sizes_op->subscription = subscription;
                sizes_op->location = add_nodes_region_;
                add_nodes_sizes_op_ = sizes_op;
                zetta::runtime::spawn(fetch_vm_sizes_task(sizes_op), [sizes_op] {
                    sizes_op->done.store(true, std::memory_order_release);
                });
            }
        }

        ImGui::Spacing();
        eyebrow(mono_font_, "PLACEMENT");
        ImGui::BeginDisabled(add_nodes_op_ != nullptr);

        // Region — a dropdown of the detected regions (free-text if none detected).
        ImGui::SetNextItemWidth(240.0f);
        if (detected_cfg && !detected_cfg->available_regions.empty()) {
            if (ImGui::BeginCombo("Region", add_nodes_region_.c_str())) {
                for (const std::string& region : detected_cfg->available_regions) {
                    if (ImGui::Selectable(region.c_str(),
                                          region == add_nodes_region_)) {
                        add_nodes_region_ = region;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::InputText("Region", &add_nodes_region_);
        }

        // VM size — a dropdown of the region's available sizes; a "Loading…" note
        // while fetching, free-text as a fallback when the list is unavailable.
        if (add_nodes_sizes_op_) {
            ImGui::TextColored(kInkQuiet, "Loading VM sizes for %s…",
                               add_nodes_region_.c_str());
        } else if (!add_nodes_size_options_.empty()) {
            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::BeginCombo("VM size", add_nodes_vm_size_.c_str())) {
                for (const std::string& sku : add_nodes_size_options_) {
                    if (ImGui::Selectable(sku.c_str(), sku == add_nodes_vm_size_)) {
                        add_nodes_vm_size_ = sku;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::SetNextItemWidth(240.0f);
            ImGui::InputText("VM size", &add_nodes_vm_size_);
        }
        ImGui::EndDisabled();
        ImGui::TextColored(kInkQuiet,
            "Dropdowns of what your subscription can deploy. The backend also falls "
            "back across sizes on a capacity shortage at create time.");

        // The consent gate — a tinted (amber) callout so the permission boundary
        // reads as a deliberate, deny-by-default stop. The Allow button IS the
        // per-action consent: there is no remembered grant, so a cloud add asks
        // every time. Nothing runs until it is clicked.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, tone_background(Tone::Warn));
        ImGui::BeginChild("##add_nodes_consent", ImVec2(0, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        status_label(Tone::Warn, "Permission required");
        ImGui::TextColored(kInkSecondary,
            "Adding nodes here runs the %s CLI to create %d billable %s on %s. This "
            "is a write and it incurs cost. Nothing runs until you allow it; consent "
            "applies to this one action and is not remembered.",
            target->cli.c_str(), add_nodes_count_,
            add_nodes_count_ == 1 ? "node" : "nodes", target->name.c_str());
        ImGui::Spacing();
        if (wired) {
            const std::string allow = "Allow adding " +
                                      std::to_string(add_nodes_count_) + plural +
                                      " via " + target->name;
            ImGui::BeginDisabled(add_nodes_op_ != nullptr);
            if (primary_button(allow.c_str())) run_add_nodes(*target);
            ImGui::EndDisabled();
        } else {
            // Honest about the seam: the control + consent gate are real, but a
            // configured cloud provisioner for this target hasn't been injected in
            // this build (constructing one needs subscription / region / creds —
            // a separate slice). Injecting it lights this up with no UI change.
            ImGui::TextColored(kInkQuiet,
                "No provisioner is wired for %s in this build. Inject a configured "
                "%s provisioner to enable adding nodes here.",
                target->name.c_str(), target->name.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // An add is running: show a live progress bar (elapsed vs the ETA baseline for
    // this target) instead of the last result. The provision runs on a background
    // coroutine (runtime::spawn), so the frame loop keeps animating this every
    // frame and the window stays responsive; poll_add_nodes() clears it on finish.
    if (add_nodes_op_) {
        ImGui::Spacing();
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() -
                                   add_nodes_op_->started)
                                   .count();
        const auto eta_it = add_nodes_eta_seconds_.find(add_nodes_op_->target_id);
        // Seed the estimate from the last successful add for this target; with no
        // baseline yet, fall back to a nominal cloud-burst duration so the bar
        // still creeps forward (it caps at 99% until the op actually completes).
        const double eta = (eta_it != add_nodes_eta_seconds_.end() &&
                            eta_it->second > 0.0)
                               ? eta_it->second
                               : 90.0;
        const float fraction =
            elapsed < eta ? static_cast<float>(elapsed / eta) : 0.99f;
        const std::string overlay =
            std::to_string(static_cast<int>(elapsed + 0.5)) + "s / ~" +
            std::to_string(static_cast<int>(eta + 0.5)) + "s";
        ImGui::ProgressBar(fraction,
                           ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                           overlay.c_str());
        ImGui::TextColored(kInkSecondary,
                           "Adding via %s on a background coroutine — the UI "
                           "stays responsive.",
                           add_nodes_op_->target_name.c_str());
    } else if (!add_nodes_status_.empty()) {
        ImGui::Spacing();
        if (add_nodes_status_warn_) {
            // A provider CLI / ARM failure can run to many lines (the real cause
            // lives deep in a JSON `details` array), so render it in full —
            // wrapped + scrollable — with a one-click **Copy** so the whole thing
            // can be pasted into an issue or a search, instead of being clipped to
            // one unreadable, unselectable line.
            if (ImGui::SmallButton("Copy error")) {
                ImGui::SetClipboardText(add_nodes_status_.c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_Text, tone_color(Tone::Warn));
            ImGui::BeginChild(
                "##add_nodes_error",
                ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8.0f),
                ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(add_nodes_status_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else {
            // The async add's completion outcome (#632) — the per-panel banner.
            // The warn branch above deliberately stays a scrollable box: a
            // provider CLI failure runs to many lines and needs Copy.
            banner(Tone::Ok, add_nodes_status_);
        }
    }
}

// The provision half of add_nodes, run OFF the UI thread as a coroutine. Takes the
// op by value (a shared_ptr) so the coroutine frame owns its inputs — never capture
// by reference (the dangling-parameter footgun). It runs the provider CLI (the slow
// part; NO Node access), stores the result under the op's mutex, then completes;
// runtime::spawn's completion flips op->done, which poll_add_nodes() picks up.
zetta::runtime::task<> provision_task(std::shared_ptr<AddNodesOp> op) {
    zetta::cloud::AddNodesResult result =
        add_nodes(*op->provisioner, zetta::cloud::AddNodesRequest{op->requested, zetta::cloud::NodeCapability{}});
    {
        std::lock_guard<std::mutex> lock(op->mutex);
        op->result = std::move(result);
    }
    co_return;
}

// Fetch the VM-size dropdown options for a region OFF the UI thread (az vm
// list-skus is slow). Inputs by value into the frame; the completion flips done.
zetta::runtime::task<> fetch_vm_sizes_task(std::shared_ptr<VmSizesOp> op) {
    std::vector<std::string> sizes = zetta::cloud::azure_vm_sizes(op->subscription, op->location);
    {
        std::lock_guard<std::mutex> lock(op->mutex);
        op->result = std::move(sizes);
    }
    co_return;
}

void App::run_add_nodes(const zetta::cloud::CloudProvider& target) {
    // Reached only from the Local "Add" button or a cloud target's explicit
    // "Allow". The slow provision runs OFF the UI thread (runtime::spawn) so a
    // minutes-long `az vmss create` never freezes the window; poll_add_nodes()
    // applies the result on the UI thread when it finishes. No-op if an add is
    // already in flight (the buttons are disabled meanwhile).
    if (add_nodes_op_) return;

    zetta::cloud::Provisioner* provisioner = provisioner_for(target.id);
    if (provisioner == nullptr) {
        add_nodes_status_ =
            "No provisioner is wired for " + target.name + " in this build.";
        add_nodes_status_warn_ = true;
        return;
    }

    // Steer where this burst lands from the editable Region / VM size fields (a
    // no-op on the given-hardware Local target). Applied on the UI thread before
    // the provision coroutine reads the config off-thread.
    provisioner->set_placement(add_nodes_region_, add_nodes_vm_size_);

    auto op = std::make_shared<AddNodesOp>();
    op->target_id = target.id;
    op->target_name = target.name;
    op->requested = static_cast<std::size_t>(add_nodes_count_);
    op->provisioner = provisioner;
    op->started = std::chrono::steady_clock::now();
    add_nodes_op_ = op;
    add_nodes_status_.clear();  // hide the previous result while this one runs

    // Fire the provision coroutine off the UI thread; the completion (on a pool
    // worker) flips `done`, which poll_add_nodes() reads each frame. The captured
    // shared_ptr keeps the op alive until the completion runs.
    zetta::runtime::spawn(provision_task(op), [op] {
        op->done.store(true, std::memory_order_release);
    });
}

void App::poll_add_nodes() {
    // The async VM-size dropdown fetch: when the coroutine finishes, copy the
    // options out and clear the op (the combo then renders them).
    if (add_nodes_sizes_op_ &&
        add_nodes_sizes_op_->done.load(std::memory_order_acquire)) {
        std::shared_ptr<VmSizesOp> sizes = add_nodes_sizes_op_;
        {
            std::lock_guard<std::mutex> lock(sizes->mutex);
            add_nodes_size_options_ = std::move(sizes->result);
        }
        add_nodes_sizes_cache_[sizes->location] = add_nodes_size_options_;  // cache it
        add_nodes_sizes_op_.reset();
    }

    if (!add_nodes_op_ || !add_nodes_op_->done.load(std::memory_order_acquire)) {
        return;
    }

    // The coroutine finished. Take its result and apply it to the node ON THE UI
    // THREAD (the thread that owns the node) — the fast membership + rebalance half.
    std::shared_ptr<AddNodesOp> op = add_nodes_op_;
    zetta::cloud::AddNodesResult result;
    {
        std::lock_guard<std::mutex> lock(op->mutex);
        if (op->result) result = std::move(*op->result);
    }
    apply_added_nodes(node_, result);

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - op->started)
                               .count();
    // Seed the ETA baseline from a run that actually brought something up, so a
    // fast failure doesn't make the next estimate absurdly low.
    if (result.satisfied || !result.members.empty()) {
        add_nodes_eta_seconds_[op->target_id] = elapsed;
    }

    add_nodes_status_warn_ = !result.satisfied;
    add_nodes_status_ = op->target_name + " · " +
                        std::to_string(result.members.size()) + " of " +
                        std::to_string(op->requested) + " node(s) · took " +
                        std::to_string(static_cast<int>(elapsed + 0.5)) + "s";
    if (!result.detail.empty()) add_nodes_status_ += " · " + result.detail;

    add_nodes_op_.reset();
}

}  // namespace zetta::gui
