#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/cluster_telemetry.hpp"

#include "zetta/add_nodes.hpp"
#include "zetta/bytes.hpp"
#include "zetta/cloud_inspector.hpp"
#include "zetta/cloud_provider.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/dataset.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/provisioner.hpp"
#include "zetta/run.hpp"
#include "zetta/scaling_policy.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

// A run's status, color-coded along its lifecycle: pending (neutral) → running
// (amber) → succeeded (green) | failed (red). Same palette as the cluster panel.
ImVec4 run_status_color(RunStatus status) {
    switch (status) {
        case RunStatus::Pending:   return kInkQuiet;
        case RunStatus::Running:   return kWarnColor;
        case RunStatus::Succeeded: return kSuccessColor;
        case RunStatus::Failed:    return kErrorColor;
    }
    return kInkQuiet;
}

// A compute job's status, color-coded along its lifecycle: queued (neutral) →
// running (amber) → succeeded (green) | failed (red). Same palette as runs.
ImVec4 job_status_color(JobStatus status) {
    switch (status) {
        case JobStatus::Queued:    return kInkQuiet;
        case JobStatus::Running:   return kWarnColor;
        case JobStatus::Succeeded: return kSuccessColor;
        case JobStatus::Failed:    return kErrorColor;
    }
    return kInkQuiet;
}

// A scaling action's color: scale-up green (more capacity), hold neutral,
// scale-down amber (shedding nodes). A quick read of the autoscaler's intent.
ImVec4 scaling_action_color(ScalingAction action) {
    switch (action) {
        case ScalingAction::ScaleUp:   return kSuccessColor;
        case ScalingAction::Hold:      return kInkQuiet;
        case ScalingAction::ScaleDown: return kWarnColor;
    }
    return kInkQuiet;
}

// A log line's severity color — error red, warn amber, anything else neutral.
ImVec4 log_level_color(const std::string& level) {
    if (level == "error") return kErrorColor;
    if (level == "warn")  return kWarnColor;
    return kInkQuiet;
}

// Cluster node-box palette (ImU32, for the window draw list) — the same Orama
// canvas grammar as the graph workspace: white cards with hairline borders, a
// soft wash on hover, a coral border (the "you are here" accent) when selected.
constexpr ImU32 kClusterBoxFill = IM_COL32(255, 255, 255, 255);          // white
constexpr ImU32 kClusterBoxFillHover = IM_COL32(242, 242, 240, 255);     // wash
constexpr ImU32 kClusterBoxFillSelected = IM_COL32(247, 238, 239, 255);  // coral wash
constexpr ImU32 kClusterBoxBorder = IM_COL32(224, 224, 224, 255);        // border
constexpr ImU32 kClusterBoxBorderSelected = IM_COL32(226, 84, 90, 255);  // coral
constexpr ImU32 kClusterBoxText = IM_COL32(28, 28, 26, 255);             // ink-primary
constexpr ImU32 kClusterBoxMuted = IM_COL32(95, 95, 92, 255);            // ink-muted

// The health-dot colour for a node, matching the replication palette the table
// used: green = self / in-sync, amber = syncing (known but behind), quiet = no
// status learned yet.
ImVec4 cluster_node_color(const NodeStatus& node) {
    if (node.is_self) return kDotActive;
    if (!node.replication.known) return kInkQuiet;
    return node.replication.in_sync ? kSuccessColor : kWarnColor;
}

// A short one-word sync state for the box caption.
const char* cluster_node_state(const NodeStatus& node) {
    if (node.is_self) return "self";
    if (!node.replication.known) return "no status";
    return node.replication.in_sync ? "in sync" : "syncing";
}

// The node's first UUID segment — enough to tell nodes apart at a glance.
std::string cluster_node_short_id(const std::string& node_id) {
    return node_id.substr(0, node_id.find('-'));
}

// ── Per-node resource gauges (#584) ───────────────────────────────────────
// Animated CPU / memory / GPU / storage / network gauges + sparklines rendered on
// each cluster node box, driven by the cluster-wide telemetry the signal-plane
// exchange gathers (Node::telemetry_snapshot()). The boxes show the live, eased
// bars; the detail pane shows the same gauges with fuller read-outs.

constexpr ImU32 kGaugeTrack = IM_COL32(236, 236, 234, 255);  // light meter track
constexpr ImU32 kGaugeSpark = IM_COL32(95, 95, 92, 140);     // faint ink trend line

// A load gauge's fill, on the semantic status palette (never coral): green under
// moderate load, amber as it climbs, red when saturated. Network throughput is a
// flow, not a good/bad status, so it uses a neutral ink fill instead of this.
ImVec4 gauge_load_color(float percent) {
    if (percent >= 85.0f) return kErrorColor;
    if (percent >= 60.0f) return kWarnColor;
    return kDotActive;
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
std::array<GaugeReading, kGaugeCount> derive_gauges(const NodeMetrics& metrics,
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
            ? format_bytes(metrics.memory_used_bytes) + " / " +
                  format_bytes(metrics.memory_total_bytes) + " · " +
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
                  format_bytes(metrics.vram_used_bytes) + " / " +
                  format_bytes(metrics.vram_total_bytes)
            : "no GPU on this host";

    // Storage — the data_dir filesystem used / total.
    GaugeReading& storage = gauges[static_cast<int>(Gauge::Storage)];
    storage.available = metrics.storage_available;
    storage.value = static_cast<float>(metrics.storage_percent);
    storage.box_text =
        storage.available ? percent_text(metrics.storage_percent) : "n/a";
    storage.detail_text =
        storage.available
            ? format_bytes(metrics.storage_used_bytes) + " / " +
                  format_bytes(metrics.storage_total_bytes) + " · " +
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
        net.detail_text = "in " + format_bytes(metrics.net_rx_bytes_per_sec) +
                          "/s · out " + format_bytes(metrics.net_tx_bytes_per_sec) +
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
void update_node_gauges(NodeGaugeState& state, const NodeMetrics& metrics,
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

// Join lineage refs (sources / steps) for a single wrapped line.
std::string join_refs(const std::vector<std::string>& refs) {
    std::string joined;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (i) joined += ", ";
        joined += refs[i];
    }
    return joined;
}

// Render one trained model produced by a run: its identity + weights fingerprint,
// its evaluation metrics (name→number), and its ranked feature drivers (the
// SHAP-style explainability surface). A pure view over an already-validated Model
// handle; metrics/drivers come from the model's free-form (already schema-
// validated) fields, so no new JSON boundary is introduced.
void render_model_card(const Model& model) {
    const std::string short_id = model.id().substr(0, model.id().find('-'));
    ImGui::SeparatorText(
        (model.name().empty() ? ("model " + short_id) : model.name()).c_str());
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkQuiet, "kind: %s  ·  id: %s", model.kind().c_str(),
                       short_id.c_str());
    if (model.has_weights()) {
        ImGui::TextColored(
            kInkQuiet, "weights: %s%s", format_bytes(model.weights()->size).c_str(),
            model.has_weights_body() ? "" : " (awaiting body transfer)");
    } else {
        ImGui::TextColored(kInkQuiet, "weights: (not yet attached)");
    }
    if (g_mono_font) ImGui::PopFont();

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

    // Metrics — the evaluation scores (auc / f1 / …). A flat name→number map; a
    // value in [0, 1] also gets a bar so a score reads at a glance.
    ImGui::TextDisabled("Metrics");
    if (!model.metrics().is_object() || model.metrics().empty()) {
        ImGui::TextDisabled("(no metrics recorded)");
    } else if (ImGui::BeginTable("##model_metrics", 2, table_flags)) {
        ImGui::TableSetupColumn("Metric");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        for (const auto& [name, value] : model.metrics().items()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(1);
            const double number = value.is_number() ? value.get<double>() : 0.0;
            if (number >= 0.0 && number <= 1.0) {
                ImGui::ProgressBar(static_cast<float>(number), ImVec2(-FLT_MIN, 0));
            } else {
                ImGui::Text("%.4g", number);
            }
        }
        ImGui::EndTable();
    }

    // Ranked drivers — the feature importances, strongest |influence| first. An
    // optional explainability annotation in the model's feature_schema; absent
    // until an Explain job records it.
    ImGui::TextDisabled("Ranked drivers");
    const std::vector<ModelDriver> drivers = model.drivers();
    if (drivers.empty()) {
        ImGui::TextDisabled("(no ranked drivers recorded)");
    } else {
        // Normalize the bars against the strongest |importance| so the ranking
        // reads visually; a leading "+/-" keeps the sign (push up vs. down).
        const double top = std::abs(drivers.front().importance);
        if (ImGui::BeginTable("##model_drivers", 3, table_flags)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Feature");
            ImGui::TableSetupColumn("Importance");
            ImGui::TableHeadersRow();
            int rank = 0;
            for (const ModelDriver& driver : drivers) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", ++rank);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(driver.feature.c_str());
                ImGui::TableSetColumnIndex(2);
                const float fraction =
                    top > 0.0 ? static_cast<float>(std::abs(driver.importance) / top)
                              : 0.0f;
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%+.3f", driver.importance);
                ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
            }
            ImGui::EndTable();
        }
    }
}

// Render the full detail of one run: identity + status, logs, outputs, the
// trained model(s) it produced (metrics + ranked drivers), and lineage. A pure
// view over the (already schema-validated) Run handle — no mutation. `models` are
// the node's models whose producing-run ref is this run (resolved by the caller).
void render_run_detail(const Run& run,
                       const std::vector<std::shared_ptr<Model>>& models) {
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkPrimary, "run %s", run.id().c_str());
    if (g_mono_font) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(run_status_color(run.status()), "[%s]",
                       std::string(to_string(run.status())).c_str());
    if (!run.project().empty()) mono_line(kInkQuiet, "project:  ", run.project());
    if (!run.graph().empty()) mono_line(kInkQuiet, "graph:    ", run.graph());
    mono_line(kInkQuiet, "created:  ", run.created());
    if (run.updated() != run.created()) {
        mono_line(kInkQuiet, "updated:  ", run.updated());
    }

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

    // Logs — time + level (color-coded) + message; the failing step + reason of
    // a failed run live here.
    ImGui::SeparatorText("Logs");
    if (run.logs().empty()) {
        ImGui::TextDisabled("(no log entries)");
    } else {
        for (const RunLogEntry& entry : run.logs()) {
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kInkQuiet, "%s", entry.time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(log_level_color(entry.level), "%-5s",
                               entry.level.c_str());
            if (g_mono_font) ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", entry.message.c_str());
        }
    }

    // Outputs — the produced datasets / models / indexes.
    ImGui::SeparatorText("Outputs");
    if (run.outputs().empty()) {
        ImGui::TextDisabled("(no outputs)");
    } else if (ImGui::BeginTable("##run_outputs", 3, table_flags)) {
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Ref");
        ImGui::TableHeadersRow();
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        for (const RunOutput& output : run.outputs()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(output.kind.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(output.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(output.ref.c_str());
        }
        if (g_mono_font) ImGui::PopFont();
        ImGui::EndTable();
    }

    // Trained models — the model(s) this run produced, with their metrics and
    // ranked drivers (the FEAT-5 "surface the trained Model + metrics + ranked
    // drivers" slice). Linked by the Model's producing-run ref (Model::run() ==
    // this run's id), the durable edge — the executor's logical model output ref
    // is just a placeholder handle in R1.
    if (!models.empty()) {
        ImGui::SeparatorText("Trained models");
        for (const std::shared_ptr<Model>& model : models) {
            ImGui::PushID(model->id().c_str());
            render_model_card(*model);
            ImGui::PopID();
        }
    }

    // Lineage — where the results came from (sources, steps), how fresh, and how
    // confident (a 0..1 score, shown as a bar).
    ImGui::SeparatorText("Lineage");
    const RunLineage& lineage = run.lineage();
    const bool has_lineage = !lineage.sources.empty() || !lineage.steps.empty() ||
                             !lineage.freshness.empty() || lineage.confidence != 0.0;
    if (!has_lineage) {
        ImGui::TextDisabled("(no lineage)");
        return;
    }
    ImGui::TextDisabled("sources:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", lineage.sources.empty()
                                 ? "—"
                                 : join_refs(lineage.sources).c_str());
    ImGui::TextDisabled("steps:");
    ImGui::SameLine();
    ImGui::TextWrapped(
        "%s", lineage.steps.empty() ? "—" : join_refs(lineage.steps).c_str());
    ImGui::TextDisabled("freshness: %s",
                        lineage.freshness.empty() ? "—" : lineage.freshness.c_str());
    ImGui::TextDisabled("confidence:");
    ImGui::SameLine();
    ImGui::ProgressBar(static_cast<float>(lineage.confidence), ImVec2(-FLT_MIN, 0));
}

}  // namespace

void App::draw_cluster() {
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
    const std::vector<NodeStatus> nodes = cluster_.nodes();

    // Re-resolve the selection against the live membership: if the selected node
    // has left (or nothing is selected yet), fall back to self so the detail pane
    // always shows a real node and a departed peer can't dangle.
    const auto is_selected_present = [&] {
        for (const NodeStatus& node : nodes)
            if (node.node_id == selected_cluster_node_) return true;
        return false;
    };
    if (!is_selected_present()) {
        selected_cluster_node_.clear();
        for (const NodeStatus& node : nodes)
            if (node.is_self) selected_cluster_node_ = node.node_id;
    }

    // Live per-node telemetry (#584): the cluster-wide snapshot gathered over the
    // signal plane (each node broadcasts its own system.node_metrics; every node
    // caches its peers'). Each node box animates its own gauges from it; a node
    // with no sample yet (a peer that just joined) shows an "awaiting" note. Pin
    // the snapshot for the frame and index it by id for the per-box lookup.
    const std::vector<NodeTelemetrySample> telemetry = node_.telemetry_snapshot();
    std::map<std::string, const NodeTelemetrySample*> telemetry_by_id;
    for (const NodeTelemetrySample& sample : telemetry) {
        telemetry_by_id[sample.node_id] = &sample;
    }
    // Drop animation state for any node that has left the cluster, so a departed
    // peer's gauges don't dangle (the snapshot already expires its telemetry).
    std::erase_if(cluster_gauges_, [&](const auto& entry) {
        for (const NodeStatus& node : nodes)
            if (node.node_id == entry.first) return false;
        return true;
    });
    const float dt = ImGui::GetIO().DeltaTime;

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

        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const NodeStatus& node = nodes[index];
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
            const std::string id = cluster_node_short_id(node.node_id) +
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

        // Reserve the laid-out area so the child scrolls when nodes overflow.
        const int rows = (static_cast<int>(nodes.size()) + columns - 1) / columns;
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(avail, std::max(0, rows) * (box_h + gap)));
    }
    ImGui::EndChild();

    draw_cluster_node_detail(nodes);
}

void App::draw_cluster_node_detail(const std::vector<NodeStatus>& nodes) {
    const NodeStatus* selected = nullptr;
    for (const NodeStatus& node : nodes)
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
        const NodeReplication& rep = selected->replication;
        mono_line(kInkSecondary, "entries   ", std::to_string(rep.entries));
        mono_line(kInkSecondary, "version   ",
                  rep.version.empty() ? "—" : rep.version);
        char sync[64];
        std::snprintf(sync, sizeof(sync), "%s (%.1fs)",
                      rep.in_sync ? "in sync" : "syncing",
                      rep.seconds_since_sync);
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::TextColored(rep.in_sync ? kSuccessColor : kWarnColor, "sync      %s",
                           sync);
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
        for (const NodeStatus& node : nodes)
            if (!node.is_self) ++peers;
        mono_line(kInkMuted, "peers     ", std::to_string(peers));
    }
}

void App::set_provisioner(const std::string& target_id, Provisioner& provisioner) {
    provisioners_[target_id] = &provisioner;
}

Provisioner* App::provisioner_for(const std::string& target_id) const {
    const auto found = provisioners_.find(target_id);
    return found == provisioners_.end() ? nullptr : found->second;
}

void App::draw_add_nodes_control() {
    // The "Add nodes" control (epic #575 area 1): drive zetta::add_nodes over an
    // injected Provisioner. The provider catalog supplies the target list + exactly
    // what each target would read/run, so the cloud consent gate can name it up
    // front (the same metadata the Cloud Inspector renders). The given-hardware
    // Local target uses the injected LocalProvisioner and runs on a plain click;
    // every cloud / Kubernetes / on-prem target is **permission-gated, deny-by-
    // default** — adding nodes there runs the provider's CLI to CREATE billable
    // resources (a write), so nothing happens until the user explicitly allows that
    // one action, and consent is never remembered (CLAUDE.md hard rule).
    ImGui::SeparatorText("Add nodes");

    const CloudProviderCatalog& catalog = CloudProviderCatalog::builtin();
    const CloudProvider* target = catalog.find(selected_add_target_);
    if (target == nullptr) {  // a stale id (none should exist) falls back to Local
        selected_add_target_ = "local";
        target = catalog.find("local");
    }
    if (target == nullptr) return;  // empty catalog — nothing to add against

    // Target selector — the same catalog the Cloud Inspector lists. A green dot
    // marks an open target (Local, no consent); amber marks a consent-gated one.
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("Target", target->name.c_str())) {
        for (const CloudProvider& provider : catalog.providers()) {
            ImGui::PushID(provider.id.c_str());
            status_dot(provider.requires_consent() ? kWarnColor : kDotActive);
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
        ImGui::BeginDisabled(!wired);
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

        // The consent gate — a tinted (amber) callout so the permission boundary
        // reads as a deliberate, deny-by-default stop. The Allow button IS the
        // per-action consent: there is no remembered grant, so a cloud add asks
        // every time. Nothing runs until it is clicked.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(0.980f, 0.933f, 0.855f, 1.0f));
        ImGui::BeginChild("##add_nodes_consent", ImVec2(0, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        status_dot(kWarnColor);
        ImGui::TextColored(kWarnColor, "Permission required");
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
            if (primary_button(allow.c_str())) run_add_nodes(*target);
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

    if (!add_nodes_status_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(add_nodes_status_warn_ ? kWarnColor : kSuccessColor,
                           "%s", add_nodes_status_.c_str());
    }
}

void App::run_add_nodes(const CloudProvider& target) {
    // The single point that touches the Provisioner — reached only from the Local
    // "Add" button or a cloud target's explicit "Allow". Synchronous on the UI
    // thread (which owns the node), like run_cloud_inspect, so the add and the
    // partition rebalance it triggers race with nothing. The Node-aware add_nodes
    // overload also rebalances sharded partitions onto any new members (#519); it
    // never tears a node down.
    Provisioner* provisioner = provisioner_for(target.id);
    if (provisioner == nullptr) {
        add_nodes_status_ =
            "No provisioner is wired for " + target.name + " in this build.";
        add_nodes_status_warn_ = true;
        return;
    }

    const std::size_t count = static_cast<std::size_t>(add_nodes_count_);
    const AddNodesResult result = add_nodes(node_, *provisioner, count);

    // Report what came up, honestly: members.size() of the requested count, plus
    // the backend's detail (for given hardware, why it's bounded by the existing
    // cluster). Amber when short (best-effort), green when the full count was met.
    add_nodes_status_warn_ = !result.satisfied;
    add_nodes_status_ = target.name + " · " + std::to_string(result.members.size()) +
                        " of " + std::to_string(count) + " node(s)";
    if (!result.detail.empty()) add_nodes_status_ += " · " + result.detail;
}

void App::draw_cloud() {
    ImGui::SeparatorText("Cloud inspector");

    // The GUI surface over the provider backends (local / on-prem / Kubernetes /
    // AWS / GCP / Azure / OCI). **Permission-gated, deny-by-default**: inspecting
    // a provider runs its read-only identity/inventory commands (which use the
    // provider's credentials via its CLI), and zetta does NOTHING until the user
    // explicitly consents — per inspection, never remembered (CLAUDE.md hard
    // rule). Only on consent does run_cloud_inspect() shell out (through the
    // shared CommandRunner) and cache the result on the node
    // (system.zetta_cloud_status). The credential FILES are never read or printed
    // — only the read-only commands run.
    ImGui::TextColored(kInkMuted,
        "Inspect a deployment target. Running a provider's read-only commands "
        "(which read its config and credentials) requires your explicit "
        "permission, every time. Deny by default: nothing runs until you allow "
        "it. Credential files are never displayed.");
    ImGui::Spacing();

    const CloudProviderCatalog& catalog = CloudProviderCatalog::builtin();

    const float list_w = ImGui::GetContentRegionAvail().x * 0.30f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 18.0f;

    // Left: the provider list, grouped under mono category eyebrows. A green dot
    // = open (Local, reads nothing); an amber dot = locked behind consent.
    ImGui::BeginChild("##cloud_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    std::string current_category;
    for (const CloudProvider& provider : catalog.providers()) {
        if (provider.category != current_category) {
            current_category = provider.category;
            if (&provider != &catalog.providers().front()) ImGui::Spacing();
            eyebrow(mono_font_, to_upper(current_category).c_str());
        }
        ImGui::PushID(provider.id.c_str());
        status_dot(provider.requires_consent() ? kWarnColor : kDotActive);
        if (ImGui::Selectable(provider.name.c_str(),
                              selected_cloud_provider_ == provider.id)) {
            // Selecting a different provider resets any consent — consent never
            // carries across providers (deny-by-default, ask every time).
            if (selected_cloud_provider_ != provider.id) {
                cloud_consent_provider_.clear();
            }
            selected_cloud_provider_ = provider.id;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected provider's detail + the consent gate.
    ImGui::SameLine();
    ImGui::BeginChild("##cloud_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    const CloudProvider* provider = catalog.find(selected_cloud_provider_);
    if (!provider) {
        ImGui::TextDisabled("Select a provider to inspect.");
        ImGui::EndChild();
        return;
    }

    ImGui::Text("%s", provider->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kInkQuiet, "%s", provider->category.c_str());
    ImGui::TextColored(kInkSecondary, "%s", provider->summary.c_str());

    // Local needs no consent: it is this running node, reached in memory. Show
    // the node facts we already hold (no cloud access) and stop.
    if (!provider->requires_consent()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "THIS NODE");
        ImGui::Spacing();
        status_dot(kDotActive);
        ImGui::TextColored(kSuccessColor,
                           "No external access. Nothing to authorize.");
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "id        %s",
                           cluster_.node_id().c_str());
        ImGui::TextColored(kInkSecondary, "data dir  %s",
                           node_.config().data_dir.c_str());
        for (const std::string& address : cluster_.listen_addresses()) {
            ImGui::TextColored(kInkSecondary, "listening %s", address.c_str());
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::EndChild();
        return;
    }

    // What an inspect WOULD touch — named up front so consent is informed. The
    // paths + commands render in mono (they are code). The CLI may read these
    // files; zetta itself never reads or prints them.
    ImGui::Spacing();
    eyebrow(mono_font_, "THE CLI MAY READ");
    if (mono_font_) ImGui::PushFont(mono_font_);
    for (const std::string& path : provider->config_paths) {
        ImGui::BulletText("%s", path.c_str());
    }
    if (mono_font_) ImGui::PopFont();
    ImGui::Spacing();
    eyebrow(mono_font_, "AND ZETTA WOULD RUN, READ-ONLY");
    if (mono_font_) ImGui::PushFont(mono_font_);
    for (const InspectCommand& command : provider->inspect_commands) {
        ImGui::BulletText("%s", command.display.c_str());
    }
    if (mono_font_) ImGui::PopFont();

    // The consent gate — a tinted callout box so the permission boundary reads
    // as a deliberate stop, deny-by-default. Until the user explicitly allows
    // this one inspection nothing is read or run. On allow, run_cloud_inspect()
    // shells out to the read-only commands and caches the result; the result is
    // rendered below and stays queryable as system.zetta_cloud_status.
    ImGui::Spacing();
    const bool consented = cloud_consent_provider_ == provider->id;
    // Amber (caution) wash while locked; green (success) wash once granted.
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          consented ? ImVec4(0.882f, 0.961f, 0.933f, 1.0f)
                                    : ImVec4(0.980f, 0.933f, 0.855f, 1.0f));
    ImGui::BeginChild("##consent", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (!consented) {
        status_dot(kWarnColor);
        ImGui::TextColored(kWarnColor, "Permission required");
        ImGui::TextColored(kInkSecondary,
            "zetta will not run the commands above until you allow it. Consent "
            "applies to this one inspection and is not remembered. Credential "
            "files are never read or shown.");
        ImGui::Spacing();
        const std::string allow_label =
            "Allow inspection of " + provider->name;
        if (primary_button(allow_label.c_str())) {
            cloud_consent_provider_ = provider->id;
            run_cloud_inspect(*provider);  // the one cloud call, behind consent
        }
    } else {
        status_dot(kDotActive);
        ImGui::TextColored(kSuccessColor, "Permission granted for this inspection");
        draw_cloud_inspect_result(*provider);
        ImGui::Spacing();
        if (ImGui::Button("Re-inspect")) {
            run_cloud_inspect(*provider);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revoke permission")) {
            cloud_consent_provider_.clear();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

// Render the cached result of the consented inspect for `provider` (found on the
// node). Reads only already-produced state — it triggers no cloud call.
void App::draw_cloud_inspect_result(const CloudProvider& provider) {
    const std::vector<CloudInspectReport> reports = node_.cloud_inspections();
    const CloudInspectReport* report = nullptr;
    for (const CloudInspectReport& candidate : reports) {
        if (candidate.provider_id == provider.id) {
            report = &candidate;
            break;
        }
    }
    if (report == nullptr) {
        ImGui::TextColored(kInkSecondary, "Inspecting…");
        return;
    }

    // A provider described but not yet runnable (bare-metal needs a host).
    if (!report->ran) {
        status_dot(kWarnColor);
        ImGui::TextColored(kInkSecondary, "%s", report->detail.c_str());
        return;
    }

    // Headline reachability / identity.
    status_dot(report->reachable ? kDotActive : kErrorColor);
    if (report->authenticated && !report->identity.empty()) {
        ImGui::TextColored(kSuccessColor, "Connected");
        ImGui::SameLine();
        ImGui::TextColored(kInkSecondary, "· %s", report->identity.c_str());
    } else if (report->reachable) {
        ImGui::TextColored(kInkSecondary, "Reachable");
    } else {
        ImGui::TextColored(kErrorColor, "Not reachable");
    }
    if (!report->detail.empty()) {
        ImGui::TextColored(kInkSecondary, "%s", report->detail.c_str());
    }

    // Regions / resources, in mono (they are identifiers).
    if (!report->locations.empty()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "REGIONS");
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextWrapped("%s (%zu)", join_refs(report->locations).c_str(),
                           report->locations.size());
        if (mono_font_) ImGui::PopFont();
    }
    if (!report->resources.empty()) {
        ImGui::Spacing();
        eyebrow(mono_font_, "NODES");
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextWrapped("%s (%zu)", join_refs(report->resources).c_str(),
                           report->resources.size());
        if (mono_font_) ImGui::PopFont();
    }

    // Per-command outcomes — a green/red dot, the command (mono), and an error
    // when one failed, so a missing CLI or a parse failure is legible.
    ImGui::Spacing();
    eyebrow(mono_font_, "COMMANDS");
    for (const CloudInspectCommand& command : report->commands) {
        status_dot(command.ok ? kDotActive : kErrorColor);
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "%s", command.display.c_str());
        if (mono_font_) ImGui::PopFont();
        if (!command.ok && !command.error.empty()) {
            ImGui::TextColored(kErrorColor, "    %s", command.error.c_str());
        }
    }
    if (!report->checked_at.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kInkQuiet, "checked at %s", report->checked_at.c_str());
    }
}

void App::run_cloud_inspect(const CloudProvider& provider) {
    // The single point where the GUI touches a cloud — reached only from the
    // consent gate's Allow / Re-inspect actions. Synchronous: the inspect runs on
    // the UI thread (which owns the node), so recording the result races with
    // nothing. The default CommandRunner shells out to the provider's CLI; the
    // result is cached for the panel and for system.zetta_cloud_status.
    CloudInspector inspector;
    node_.record_cloud_inspection(inspector.inspect(provider));
}

void App::draw_runs() {
    ImGui::SeparatorText("Runs / outputs / lineage");

    // A run is the record of one execution of a deployed graph (status / logs /
    // outputs / lineage). This panel is a read-only viewer: runs arrive via the
    // API, tests, or replication from a peer (executing a graph — StartRun — is
    // a later slice). Held by id and re-resolved each frame, like the explorer.
    const std::vector<std::shared_ptr<Run>> runs = node_.runs();
    ImGui::TextDisabled("%zu run(s)", runs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 14.0f;

    // Left: the run list. Runs have no human name, so each row is a status badge
    // plus the first segment of the UUID — enough to tell them apart.
    ImGui::BeginChild("##run_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (runs.empty()) ImGui::TextDisabled("(no runs)");
    for (const std::shared_ptr<Run>& run : runs) {
        ImGui::PushID(run->id().c_str());
        ImGui::TextColored(run_status_color(run->status()), "%-9s",
                           std::string(to_string(run->status())).c_str());
        ImGui::SameLine();
        const std::string short_id = run->id().substr(0, run->id().find('-'));
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (ImGui::Selectable(short_id.c_str(), selected_run_ == run->id())) {
            selected_run_ = run->id();
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected run's status / logs / outputs / lineage.
    ImGui::SameLine();
    ImGui::BeginChild("##run_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const auto run = node_.find_run(selected_run_)) {
        // Resolve the trained model(s) this run produced via the durable reverse
        // link (Model::run() == run id), so the detail pane can surface their
        // metrics + ranked drivers alongside the run's outputs.
        std::vector<std::shared_ptr<Model>> run_models;
        for (const std::shared_ptr<Model>& model : node_.models()) {
            if (model->run() == run->id()) run_models.push_back(model);
        }
        render_run_detail(*run, run_models);
    } else {
        ImGui::TextDisabled(
            "Select a run to view its status, logs, outputs, models, and "
            "lineage.");
    }
    ImGui::EndChild();
}

void App::reset_add_model_form() {
    add_model_name_.clear();
    add_model_kind_ = "classifier";
    add_model_metrics_.clear();
    add_model_features_.clear();
    add_model_weights_path_.clear();
    add_model_weights_name_.clear();
    add_model_error_.clear();
}

void App::draw_add_model_form() {
    // The inline register/import affordance, collapsed by default so the list
    // stays the focus. A model is keyed by id (no name collision), so Register
    // always creates a new one. The pasted metrics / feature_schema are parsed
    // here, and the assembled value is re-validated against the Model JSON Schema
    // (validate_model_value) at this boundary — so malformed input (e.g. a
    // non-numeric metric) fails fast here instead of being rejected on the next
    // reconcile. Weights are optional and attached after create via set_weights.
    if (!ImGui::CollapsingHeader("Add a model")) return;

    ImGui::TextDisabled(
        "Register a trained model on this node. Kind is required; metrics and "
        "feature schema are optional JSON; weights are optional. It then appears "
        "below and in system.zetta_models.");
    ImGui::Spacing();

    // 1) Name (optional) + kind (required).
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##am_name", "optional, e.g. churn-v1",
                             &add_model_name_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Kind");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##am_kind", "required, e.g. classifier",
                             &add_model_kind_);

    // 2) Metrics (optional) — a JSON object of name→number (the schema requires
    //    numeric values). Surfaced in the detail pane's Metrics table.
    ImGui::Spacing();
    ImGui::TextUnformatted("Metrics (JSON)");
    ImGui::InputTextMultiline("##am_metrics", &add_model_metrics_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::TextDisabled(
        "Optional object of numbers, e.g. {\"auc\": 0.91, \"f1\": 0.85}.");

    // 3) Feature schema (optional) — a free-form JSON object. A `drivers` array
    //    inside it renders as the detail pane's ranked drivers.
    ImGui::Spacing();
    ImGui::TextUnformatted("Feature schema (JSON)");
    ImGui::InputTextMultiline("##am_features", &add_model_features_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::TextDisabled(
        "Optional object. A \"drivers\":[{\"feature\",\"importance\"}] array shows "
        "as ranked drivers.");

    // 4) Weights (optional) — a file staged via the native dialog (or a drop
    //    routed to FileIntent::ModelWeights), attached on Register.
    ImGui::Spacing();
    ImGui::TextUnformatted("Weights (file)");
    if (ImGui::Button("Choose weights file...")) {
        SDL_ShowOpenFileDialog(&App::on_open_model_weights, this, window_, nullptr,
                               0, nullptr, false);
    }
    ImGui::SameLine();
    if (add_model_weights_path_.empty()) {
        ImGui::TextDisabled("optional; opaque bytes, fingerprinted on attach");
    } else {
        if (mono_font_) ImGui::PushFont(mono_font_);
        ImGui::TextColored(kInkSecondary, "%s", add_model_weights_name_.c_str());
        if (mono_font_) ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            add_model_weights_path_.clear();
            add_model_weights_name_.clear();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (!add_model_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", add_model_error_.c_str());
    }

    if (primary_button("Register model")) {
        add_model_error_.clear();

        // Inline JSON-object parser for the optional metrics / feature_schema
        // fields: blank → an empty object; otherwise the text must parse to an
        // object (the Model schema requires both to be objects).
        const auto blank = [](const std::string& text) {
            return text.find_first_not_of(" \t\r\n") == std::string::npos;
        };
        const auto parse_object = [&](const std::string& text, const char* label,
                                      nlohmann::json& out,
                                      std::string& error) -> bool {
            if (blank(text)) {
                out = nlohmann::json::object();
                return true;
            }
            nlohmann::json parsed =
                nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded()) {
                error = std::string(label) + " is not valid JSON";
                return false;
            }
            if (!parsed.is_object()) {
                error = std::string(label) + " must be a JSON object";
                return false;
            }
            out = std::move(parsed);
            return true;
        };

        ModelValue value;
        value.name = add_model_name_;
        value.kind = add_model_kind_;
        nlohmann::json metrics;
        nlohmann::json features;
        std::string error;
        if (blank(add_model_kind_)) {
            add_model_error_ = "kind is required";
        } else if (!parse_object(add_model_metrics_, "metrics", metrics, error)) {
            add_model_error_ = error;
        } else if (!parse_object(add_model_features_, "feature schema", features,
                                 error)) {
            add_model_error_ = error;
        } else {
            value.metrics = std::move(metrics);
            value.feature_schema = std::move(features);
            // Re-validate the assembled value against the Model JSON Schema (e.g.
            // metrics values must be numbers) before create — the same gate the
            // record must pass when it reloads / reconciles. create_model stamps
            // `created` + leaves weights unset, both schema-valid, so a value that
            // passes here also passes on reload.
            if (!validate_model_value(zetta::to_json(value), &error)) {
                add_model_error_ = error;
            } else if (auto model = node_.create_model(value)) {
                std::string summary = "Registered model";
                if (!model->name().empty()) summary += " '" + model->name() + "'";
                summary += " (" + model->kind() + ")";
                // Attach optional weights from the staged file (set_weights writes
                // the body to disk, fingerprints it, and replicates the metadata).
                if (!add_model_weights_path_.empty()) {
                    std::ifstream in(add_model_weights_path_, std::ios::binary);
                    if (in) {
                        const std::string bytes(
                            (std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
                        summary += model->set_weights(bytes)
                                       ? " + weights (" +
                                             format_bytes(bytes.size()) + ")"
                                       : " (weights attach failed)";
                    } else {
                        summary += " (weights file unreadable)";
                    }
                }
                selected_model_ = model->id();
                io_status_ = summary;
                reset_add_model_form();
            } else {
                add_model_error_ = "could not create the model";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) reset_add_model_form();
}

void App::draw_models() {
    ImGui::SeparatorText("Models");

    // The GUI form of system.zetta_models: the trained-model registry (ENT-14 /
    // FEAT-6). A thin, read-only viewer over the replicated, already-validated
    // Model handles — models arrive from a train job, the API, or replication —
    // plus the inline "Add model" affordance to register/import one. Held by id and
    // re-resolved each frame, like the Runs panel.
    const std::vector<std::shared_ptr<Model>> models = node_.models();
    ImGui::TextDisabled("%zu model(s)", models.size());

    draw_add_model_form();

    const float list_w = ImGui::GetContentRegionAvail().x * 0.30f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 18.0f;

    // Left: the model list. Models have no lifecycle status, so each row is its
    // kind tag plus the name (or the first UUID segment when unnamed) — enough to
    // tell them apart.
    ImGui::BeginChild("##model_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (models.empty()) ImGui::TextDisabled("(no models)");
    for (const std::shared_ptr<Model>& model : models) {
        ImGui::PushID(model->id().c_str());
        ImGui::TextColored(kInkQuiet, "%-11s", model->kind().c_str());
        ImGui::SameLine();
        const std::string short_id =
            model->id().substr(0, model->id().find('-'));
        const std::string label =
            model->name().empty() ? short_id : model->name();
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (ImGui::Selectable(label.c_str(), selected_model_ == model->id())) {
            selected_model_ = model->id();
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected model's detail — metrics, ranked drivers, and weights
    // fingerprint (render_model_card, reused from the Runs panel), plus its
    // creation stamp and lineage refs.
    ImGui::SameLine();
    ImGui::BeginChild("##model_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const std::shared_ptr<Model> model = node_.find_model(selected_model_)) {
        render_model_card(*model);
        ImGui::Spacing();
        mono_line(kInkQuiet, "created:  ", model->created());
        if (!model->project().empty()) {
            mono_line(kInkQuiet, "project:  ", model->project());
        }
        if (!model->run().empty()) {
            mono_line(kInkQuiet, "run:      ", model->run());
        }
    } else {
        ImGui::TextDisabled(
            "Select a model to view its metrics, ranked drivers, and weights "
            "fingerprint.");
    }
    ImGui::EndChild();
}

void App::draw_datasets() {
    ImGui::SeparatorText("Datasets");

    // The GUI form of system.datasets (#585; the area-3 dataset view of epic
    // #575): one dataset == one table, summarized live from the node tree + the
    // replicate-to-all partition catalog. A thin, read-only viewer — schema, row
    // count, on-disk size, storage format, replication strategy, and partition
    // placement across the cluster (which node owns which partition). Recomputed
    // each frame via Node::datasets(), so it never drifts from the truth; the only
    // UI state is the selected dataset's table id (re-resolved each frame).
    const std::vector<DatasetSummary> datasets = node_.datasets();
    ImGui::TextDisabled("%zu dataset(s)", datasets.size());

    // Re-resolve the selection against the live datasets: if the selected table is
    // gone (or nothing is selected yet), fall back to the first dataset so the
    // detail pane always shows a real one.
    const auto find_selected = [&]() -> const DatasetSummary* {
        for (const DatasetSummary& dataset : datasets)
            if (dataset.table_id == selected_dataset_) return &dataset;
        return nullptr;
    };
    if (!find_selected() && !datasets.empty()) {
        selected_dataset_ = datasets.front().table_id;
    }

    const float list_w = ImGui::GetContentRegionAvail().x * 0.30f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 20.0f;

    // Left: the dataset list. Each row is the on-disk format tag plus the dataset
    // name (the table name); the owning database is a muted sub-line so same-named
    // tables across databases stay distinguishable.
    ImGui::BeginChild("##dataset_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (datasets.empty()) ImGui::TextDisabled("(no datasets)");
    for (const DatasetSummary& dataset : datasets) {
        ImGui::PushID(dataset.table_id.c_str());
        ImGui::TextColored(kInkQuiet, "%-6s", dataset.format.c_str());
        ImGui::SameLine();
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (ImGui::Selectable(dataset.name.c_str(),
                              selected_dataset_ == dataset.table_id)) {
            selected_dataset_ = dataset.table_id;
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::TextColored(kInkQuiet, "      %s", dataset.database.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected dataset's detail — identity, shape (format / replication
    // / rows / size), schema, and the partition-placement overlay.
    ImGui::SameLine();
    ImGui::BeginChild("##dataset_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const DatasetSummary* dataset = find_selected()) {
        eyebrow(mono_font_, "DATASET");
        mono_line(kInkPrimary, "name        ", dataset->name);
        mono_line(kInkSecondary, "database    ", dataset->database);
        mono_line(kInkQuiet, "table id    ", dataset->table_id);

        ImGui::Spacing();
        eyebrow(mono_font_, "SHAPE");
        mono_line(kInkSecondary, "format      ", dataset->format);
        mono_line(kInkSecondary, "replication ", dataset->replication);
        mono_line(kInkSecondary, "rows        ", std::to_string(dataset->rows));
        mono_line(kInkSecondary, "size        ", format_bytes(dataset->size_bytes));
        mono_line(kInkSecondary, "columns     ",
                  std::to_string(dataset->columns.size()));

        ImGui::Spacing();
        eyebrow(mono_font_, "SCHEMA");
        if (dataset->columns.empty()) {
            ImGui::TextDisabled("(no columns — the dataset is empty)");
        } else {
            // The inferred field names (first-seen order), wrapped in mono.
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::PushStyleColor(ImGuiCol_Text, kInkSecondary);
            ImGui::TextWrapped("%s", join_refs(dataset->columns).c_str());
            ImGui::PopStyleColor();
            if (g_mono_font) ImGui::PopFont();
        }

        ImGui::Spacing();
        eyebrow(mono_font_, "PARTITION PLACEMENT ACROSS THE CLUSTER");
        draw_dataset_placement(*dataset);
    } else {
        ImGui::TextDisabled(
            "Select a dataset to view its schema, size, and partition placement "
            "across the cluster.");
    }
    ImGui::EndChild();
}

void App::draw_dataset_placement(const DatasetSummary& dataset) {
    // A non-partitioned (replicate-to-all) dataset has no catalog entries — it
    // lives in full on every node — so there is no per-partition placement to show.
    if (dataset.partitions.empty()) {
        ImGui::TextDisabled(
            "Not partitioned (%s): the dataset is replicated in full to every "
            "node.",
            dataset.replication.c_str());
        return;
    }

    // Map each node to the partitions of THIS dataset it owns. The node universe
    // is the union of live cluster membership and every owner referenced by the
    // catalog, so a partition owned by a node that has since left (or a foreign
    // owner not yet connected) still appears — the placement is the catalog's
    // global truth, not just this node's local view.
    std::unordered_map<std::string, std::vector<const DatasetPlacement*>> owned;
    std::vector<std::string> node_order;
    const auto note_node = [&](const std::string& node_id) {
        if (owned.find(node_id) == owned.end()) {
            owned.emplace(node_id, std::vector<const DatasetPlacement*>{});
            node_order.push_back(node_id);
        }
    };
    const std::vector<NodeStatus> nodes = cluster_.nodes();
    std::unordered_set<std::string> is_member;
    std::string self_id;
    for (const NodeStatus& node : nodes) {
        note_node(node.node_id);
        is_member.insert(node.node_id);
        if (node.is_self) self_id = node.node_id;
    }
    for (const DatasetPlacement& placement : dataset.partitions) {
        for (const std::string& owner : placement.owner_nodes) {
            note_node(owner);
            owned[owner].push_back(&placement);
        }
    }

    ImGui::TextDisabled("%zu partition(s) across %zu node(s)",
                        dataset.partitions.size(), node_order.size());

    // One node box per node (the same spatial grammar as the Cluster view): a node
    // that owns a partition of this dataset is highlighted (coral), one that owns
    // none is dimmed — so the overlay reads "which node holds which slice" at a
    // glance. The box caption is the share it owns (count of partitions).
    const float font = ImGui::GetFontSize();
    const float box_w = font * 11.0f;
    const float box_h = font * 4.2f;
    const float gap = font * 0.7f;
    const float pad = font * 0.6f;
    const float rounding = font * 0.3f;

    if (ImGui::BeginChild("##dataset_placement_canvas", ImVec2(0, font * 10.0f),
                          ImGuiChildFlags_Borders)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const int columns =
            std::max(1, static_cast<int>((avail + gap) / (box_w + gap)));

        for (std::size_t index = 0; index < node_order.size(); ++index) {
            const std::string& node_id = node_order[index];
            const std::vector<const DatasetPlacement*>& parts = owned[node_id];
            const bool owns = !parts.empty();
            const int row = static_cast<int>(index) / columns;
            const int col = static_cast<int>(index) % columns;
            const ImVec2 top_left(origin.x + col * (box_w + gap),
                                  origin.y + row * (box_h + gap));
            const ImVec2 bottom_right(top_left.x + box_w, top_left.y + box_h);

            // A node holding a slice of this dataset is the highlighted state
            // (coral wash + border, the "you are here"/owner accent); one holding
            // none is the plain card. No hit-test — this is a read-only overlay.
            draw->AddRectFilled(top_left, bottom_right,
                                owns ? kClusterBoxFillSelected : kClusterBoxFill,
                                rounding);
            draw->AddRect(top_left, bottom_right,
                          owns ? kClusterBoxBorderSelected : kClusterBoxBorder,
                          rounding, 0, owns ? 2.0f : 1.0f);

            draw->PushClipRect(top_left, bottom_right, true);
            const bool is_self = node_id == self_id;
            const bool departed = is_member.find(node_id) == is_member.end();
            std::string id = cluster_node_short_id(node_id);
            if (is_self) id += " (self)";
            else if (departed) id += " (gone)";
            const ImVec2 id_pos(top_left.x + pad, top_left.y + pad);
            const ImU32 id_color = departed ? kClusterBoxMuted : kClusterBoxText;
            if (mono_font_) {
                draw->AddText(mono_font_, font, id_pos, id_color, id.c_str());
            } else {
                draw->AddText(id_pos, id_color, id.c_str());
            }

            const float small = font * 0.85f;
            char caption[64];
            std::snprintf(caption, sizeof(caption), "%zu partition%s",
                          parts.size(), parts.size() == 1 ? "" : "s");
            const ImVec2 caption_pos(top_left.x + pad, top_left.y + pad + font * 1.5f);
            const ImU32 caption_color = owns ? kClusterBoxText : kClusterBoxMuted;
            if (mono_font_) {
                draw->AddText(mono_font_, small, caption_pos, caption_color,
                              caption);
            } else {
                draw->AddText(caption_pos, caption_color, caption);
            }
            // The covered row span, so the box says not just how many but how much.
            std::uint64_t rows_here = 0;
            for (const DatasetPlacement* placement : parts) {
                rows_here += placement->row_end - placement->row_start;
            }
            if (owns) {
                char rows_caption[64];
                std::snprintf(rows_caption, sizeof(rows_caption), "%llu rows",
                              static_cast<unsigned long long>(rows_here));
                const ImVec2 rows_pos(top_left.x + pad,
                                      top_left.y + pad + font * 2.6f);
                if (mono_font_) {
                    draw->AddText(mono_font_, small, rows_pos, kClusterBoxMuted,
                                  rows_caption);
                } else {
                    draw->AddText(rows_pos, kClusterBoxMuted, rows_caption);
                }
            }
            draw->PopClipRect();
        }

        const int rows =
            (static_cast<int>(node_order.size()) + columns - 1) / columns;
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(avail, std::max(0, rows) * (box_h + gap)));
    }
    ImGui::EndChild();

    // The literal per-partition placement table: which slice → which owners — the
    // catalog rows behind the overlay above, in row-range order.
    if (ImGui::BeginTable("##dataset_partitions", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("partition");
        ImGui::TableSetupColumn("row range");
        ImGui::TableSetupColumn("owner nodes");
        ImGui::TableHeadersRow();
        for (const DatasetPlacement& placement : dataset.partitions) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kInkSecondary, "%s",
                               cluster_node_short_id(placement.partition_id).c_str());
            ImGui::TableNextColumn();
            char range[64];
            std::snprintf(range, sizeof(range), "[%llu, %llu)",
                          static_cast<unsigned long long>(placement.row_start),
                          static_cast<unsigned long long>(placement.row_end));
            ImGui::TextColored(kInkSecondary, "%s", range);
            ImGui::TableNextColumn();
            std::vector<std::string> owners;
            owners.reserve(placement.owner_nodes.size());
            for (const std::string& owner : placement.owner_nodes) {
                owners.push_back(cluster_node_short_id(owner));
            }
            ImGui::TextColored(kInkSecondary, "%s",
                               owners.empty() ? "(unplaced)"
                                              : join_refs(owners).c_str());
            if (g_mono_font) ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

void App::reset_scaling_controls() {
    scaling_objective_ = 0;          // minimize_cost
    scaling_min_nodes_ = 1;
    scaling_max_nodes_ = 8;
    scaling_cost_cap_ = 0.0;
    scaling_target_seconds_ = 600.0;
    scaling_nodes_ = 2;
    scaling_progress_ = 0.4;
    scaling_elapsed_ = 120.0;
    scaling_cost_ = 50.0;
}

void App::draw_job_scaling_controls() {
    // Per-job objective/scaling controls — a LIVE PREVIEW over the built
    // ScalingPolicy::evaluate(). It is a "what-if" calculator, not a write path:
    // ScalingPolicy is not persisted and the autoscaler is unbuilt (both later
    // slices), so this scores an editable policy + sample with the real, pure
    // evaluate() and shows the resulting decision — set the reward objective +
    // bounds, add/remove nodes, watch the recommendation move. Nothing here
    // mutates the job.
    ImGui::SeparatorText("Objective & scaling (preview)");
    ImGui::TextDisabled(
        "Score a reward objective against a sample; no autoscaler is wired yet, "
        "so this previews ScalingPolicy::evaluate(); it doesn't change the job.");

    // 1) Objective — "set the reward". The three reward models the autoscaler can
    //    optimize, in declaration order (the combo index maps to ScalingObjective).
    static constexpr std::array<ScalingObjective, 3> kObjectives{
        ScalingObjective::MinimizeCost, ScalingObjective::RuntimeTarget,
        ScalingObjective::DataShape};
    if (scaling_objective_ < 0 ||
        scaling_objective_ >= static_cast<int>(kObjectives.size())) {
        scaling_objective_ = 0;
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo(
            "Reward objective",
            std::string(to_string(kObjectives[scaling_objective_])).c_str())) {
        for (int index = 0; index < static_cast<int>(kObjectives.size());
             ++index) {
            const bool is_selected = index == scaling_objective_;
            if (ImGui::Selectable(
                    std::string(to_string(kObjectives[index])).c_str(),
                    is_selected)) {
                scaling_objective_ = index;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const ScalingObjective objective = kObjectives[scaling_objective_];

    // 2) Bounds — the hard envelope the autoscaler must stay inside (min/max node
    //    count + a cost cap; 0 = unlimited). A runtime target only matters for the
    //    RuntimeTarget objective, so it is shown only then.
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Min nodes", &scaling_min_nodes_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max nodes", &scaling_max_nodes_);
    if (scaling_min_nodes_ < 1) scaling_min_nodes_ = 1;
    if (scaling_max_nodes_ < scaling_min_nodes_) {
        scaling_max_nodes_ = scaling_min_nodes_;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Cost cap (0 = unlimited)", &scaling_cost_cap_, 0.0, 0.0,
                       "%.1f");
    if (scaling_cost_cap_ < 0.0) scaling_cost_cap_ = 0.0;
    if (objective == ScalingObjective::RuntimeTarget) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputDouble("Target seconds", &scaling_target_seconds_, 0.0, 0.0,
                           "%.1f");
        if (scaling_target_seconds_ < 0.0) scaling_target_seconds_ = 0.0;
    }

    // 3) Sample — the live signals the autoscaler scores (progress, elapsed,
    //    cost) plus the current allocation. "Add/remove nodes" are the +/- buttons
    //    that move the sample's node count within the bounds.
    ImGui::Spacing();
    ImGui::TextDisabled("Allocated nodes");
    ImGui::SameLine();
    if (ImGui::SmallButton("- node") && scaling_nodes_ > 0) --scaling_nodes_;
    ImGui::SameLine();
    ImGui::Text("%d", scaling_nodes_);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ node")) ++scaling_nodes_;
    if (scaling_nodes_ < 0) scaling_nodes_ = 0;

    ImGui::SetNextItemWidth(220.0f);
    float progress = static_cast<float>(scaling_progress_);
    if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f, "%.2f")) {
        scaling_progress_ = progress;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Elapsed seconds", &scaling_elapsed_, 0.0, 0.0, "%.1f");
    if (scaling_elapsed_ < 0.0) scaling_elapsed_ = 0.0;
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Accrued cost", &scaling_cost_, 0.0, 0.0, "%.1f");
    if (scaling_cost_ < 0.0) scaling_cost_ = 0.0;

    // 4) Score it — the real, pure decision. reward (higher is better),
    //    recommended node count, the scale up/hold/down action relative to the
    //    sample, whether the cost cap holds, and the human-readable reason.
    ScalingPolicy policy;
    policy.objective = objective;
    policy.bounds.min_nodes = static_cast<std::uint32_t>(scaling_min_nodes_);
    policy.bounds.max_nodes = static_cast<std::uint32_t>(scaling_max_nodes_);
    policy.bounds.cost_cap = scaling_cost_cap_;
    policy.target_seconds = scaling_target_seconds_;
    const ScalingSample sample{scaling_progress_, scaling_elapsed_, scaling_cost_,
                               static_cast<std::uint32_t>(scaling_nodes_)};
    const ScalingDecision decision = policy.evaluate(sample);

    ImGui::SeparatorText("Decision");
    ImGui::Text("recommended nodes: %u", decision.recommended_nodes);
    ImGui::SameLine();
    ImGui::TextColored(scaling_action_color(decision.action), "(%s)",
                       std::string(to_string(decision.action)).c_str());
    ImGui::Text("reward: %.4g", decision.reward);
    if (!decision.within_cost_cap) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "  cost cap breached");
    }
    ImGui::TextWrapped("%s", decision.reason.c_str());
}

void App::draw_jobs() {
    ImGui::SeparatorText("Compute jobs");

    // The GUI form of system.zetta_jobs: the live compute-job registry (one row
    // per unit of native distributed accelerated compute — featurize / train /
    // explain / score / a DDF op — with its lifecycle status). Read-only over the
    // replicated, already-validated ComputeJob handles, like the Runs panel —
    // creating/scheduling a job is a later slice. The detail pane adds the per-job
    // objective/scaling preview (draw_job_scaling_controls).
    const std::vector<std::shared_ptr<ComputeJob>> jobs = node_.jobs();
    ImGui::TextDisabled("%zu job(s)", jobs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 22.0f;

    // Left: the job list. Jobs have no human name, so each row is a color-coded
    // status badge plus the kind and the first UUID segment.
    ImGui::BeginChild("##job_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (jobs.empty()) ImGui::TextDisabled("(no jobs)");
    for (const std::shared_ptr<ComputeJob>& job : jobs) {
        ImGui::PushID(job->id().c_str());
        ImGui::TextColored(job_status_color(job->status()), "%-9s",
                           std::string(to_string(job->status())).c_str());
        ImGui::SameLine();
        const std::string short_id = job->id().substr(0, job->id().find('-'));
        const std::string label =
            std::string(to_string(job->kind())) + " · " + short_id;
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (ImGui::Selectable(label.c_str(), selected_job_ == job->id())) {
            selected_job_ = job->id();
        }
        if (mono_font_) ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected job's detail + the objective/scaling preview.
    ImGui::SameLine();
    ImGui::BeginChild("##job_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const std::shared_ptr<ComputeJob> job = node_.find_job(selected_job_)) {
        // Reset the scaling controls whenever the selection changes, so they
        // start from sensible defaults per job rather than leaking the previous
        // job's edits.
        if (scaling_loaded_job_ != job->id()) {
            reset_scaling_controls();
            scaling_loaded_job_ = job->id();
        }

        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::TextColored(kInkPrimary, "job %s", job->id().c_str());
        if (g_mono_font) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(job_status_color(job->status()), "[%s]",
                           std::string(to_string(job->status())).c_str());
        mono_line(kInkQuiet, "kind:     ", std::string(to_string(job->kind())));
        mono_line(kInkQuiet, "created:  ", job->created());
        if (job->updated() != job->created()) {
            mono_line(kInkQuiet, "updated:  ", job->updated());
        }
        if (!job->detail().empty()) {
            ImGui::TextWrapped("detail: %s", job->detail().c_str());
        }

        // Id references — the job's place in the pipeline (project / graph / run /
        // scaling-policy reward). Shown only when set.
        const auto ref_line = [](const char* label, const std::string& value) {
            if (!value.empty()) mono_line(kInkQuiet, label, value);
        };
        ref_line("project:  ", job->project());
        ref_line("graph:    ", job->graph());
        ref_line("run:      ", job->run());
        ref_line("policy:   ", job->scaling_policy());

        draw_job_scaling_controls();
    } else {
        ImGui::TextDisabled(
            "Select a job to view its status, references, and scaling controls.");
    }
    ImGui::EndChild();
}

}  // namespace zetta::gui
