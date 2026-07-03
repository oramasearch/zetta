#pragma once

// Helpers shared by the per-view operations TUs (app_cluster.cpp,
// app_cloud.cpp, app_runs.cpp, app_models.cpp, app_datasets.cpp,
// app_jobs.cpp) and the Search view's cluster strip (app_search.cpp) — the
// pieces that used to be file-local to the monolithic app_operations.cpp but
// are used by more than one view. This is an INTERNAL header for the src/gui
// TUs only — it is NOT part of the public include/zetta surface, which is why
// it may freely include <imgui.h>.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "gui/cluster_pipeline.hpp"

namespace zetta::ml {
class Model;
}

namespace zetta::gui {

// The canvas node-box palette (kCanvasBox*) lives in app_internal.hpp with
// the rest of the design-system tokens (#630) — one home for every colour.

// An entity id's first UUID segment — enough to tell entities apart at a
// glance. Shared by the Cluster view, the Datasets partition-placement
// overlay, and the master-detail rows of the Runs / Models / Jobs views.
inline std::string entity_short_id(const std::string& entity_id) {
    return entity_id.substr(0, entity_id.find('-'));
}

// Join lineage refs (sources / steps) for a single wrapped line. Shared by the
// Runs, Cloud, and Datasets views.
inline std::string join_refs(const std::vector<std::string>& refs) {
    std::string joined;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (i) joined += ", ";
        joined += refs[i];
    }
    return joined;
}

// Render one trained model produced by a run: its identity + weights
// fingerprint, its evaluation metrics, and its ranked feature drivers. Defined
// in app_models.cpp; also used by the Runs view's run detail (the trained
// model(s) a run produced render inline).
void render_model_card(const zetta::ml::Model& model);

// ── The flow-animation grammar (#590) ──────────────────────────────────────
// The distributed-algorithm-at-work vocabulary: brief eased "flows" gliding
// over node boxes. Shared by the Cluster view's overlay (which diffs the live
// instrumentation read views into flows — update_cluster_pipeline in
// app_cluster.cpp) and the Search view's cluster strip (which replays a
// completed search's scatter-gather from its per-node provenance, #643).
//
// Flow palette: near-data / local work reads GREEN (it stays home), wire /
// collective movement reads INK (it crosses the network). Coral stays reserved
// for the selection accent (Orama discipline).
inline constexpr ImU32 kFlowLocal = IM_COL32(29, 158, 117, 235);  // kDotActive — near data / placed
inline constexpr ImU32 kFlowWire = IM_COL32(95, 95, 92, 220);     // kInkMuted — over the wire
inline constexpr ImU32 kFlowMove = IM_COL32(68, 68, 65, 235);     // kInkSecondary — rebalance move

// Re-pack an ImU32 with a new alpha (0..1) — for fading a flow over its lifetime.
inline ImU32 with_alpha(ImU32 color, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const ImU32 a = static_cast<ImU32>(alpha * 255.0f + 0.5f);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// The flow's draw colour by kind (green = stays local, ink = crosses the wire).
inline ImU32 flow_color(PipelineFlowKind kind) {
    switch (kind) {
        case PipelineFlowKind::PartitionPlaced:
        case PipelineFlowKind::NearDataRead:
        case PipelineFlowKind::LocalCompute:
            return kFlowLocal;
        case PipelineFlowKind::PartitionMoved:
            return kFlowMove;
        case PipelineFlowKind::CollectiveScatter:
        case PipelineFlowKind::CollectiveGather:
        case PipelineFlowKind::WireMoved:
            return kFlowWire;
    }
    return kFlowWire;
}

// Draw one in-flight flow over a node-box canvas. `centers` maps node id → box
// centre (rebuilt each frame, so the animation tracks a live resize / reflow); a
// flow whose endpoint has left the canvas is silently skipped. A travelling flow
// (distinct from / to) eases a comet with a short trail along a faint connecting
// line; a from-data / local flow (no source) pulses an expanding ring on the target
// box. Pure draw on the window draw list — no ImGui layout.
inline void draw_pipeline_flow(ImDrawList* draw, ImFont* mono, float font,
                               const std::unordered_map<std::string, ImVec2>& centers,
                               const PipelineFlow& flow, float box_h) {
    if (flow.age < 0.0f) return;  // scheduled but not yet started
    const float t = std::clamp(flow.age / flow.lifetime, 0.0f, 1.0f);
    const float fade = 1.0f - t;
    const ImU32 base = flow_color(flow.kind);
    const auto find = [&](const std::string& id) -> const ImVec2* {
        const auto it = centers.find(id);
        return it == centers.end() ? nullptr : &it->second;
    };

    const bool travelling = !flow.from.empty() && flow.from != flow.to;
    if (travelling) {
        const ImVec2* a = find(flow.from);
        const ImVec2* b = find(flow.to);
        if (!a || !b) return;
        const float line_alpha = 0.30f * std::sin(t * 3.14159265f);
        draw->AddLine(*a, *b, with_alpha(kFlowWire, line_alpha), 1.4f);
        const float ease = t * t * (3.0f - 2.0f * t);  // smoothstep
        const float radius = font * 0.18f + flow.weight * font * 0.30f;
        for (int trail = 0; trail < 4; ++trail) {
            const float back = std::max(0.0f, ease - trail * 0.06f);
            const ImVec2 p(a->x + (b->x - a->x) * back, a->y + (b->y - a->y) * back);
            draw->AddCircleFilled(p, radius * (1.0f - trail * 0.16f),
                                  with_alpha(base, fade * (1.0f - trail * 0.22f)));
        }
        if (t > 0.7f) {  // a soft landing ring as it arrives
            const float ring = box_h * 0.18f * (t - 0.7f) / 0.3f;
            draw->AddCircle(*b, ring + radius, with_alpha(base, fade * 0.7f), 0, 2.0f);
        }
        if (!flow.label.empty() && t < 0.6f && mono) {
            const ImVec2 head(a->x + (b->x - a->x) * ease, a->y + (b->y - a->y) * ease);
            const float ts = font * 0.78f;
            draw->AddText(mono, ts, ImVec2(head.x + radius + 2.0f, head.y - ts * 0.5f),
                          with_alpha(base, fade), flow.label.c_str());
        }
    } else {
        const ImVec2* c = find(flow.to);
        if (!c) return;
        const float max_r = box_h * (0.30f + flow.weight * 0.22f);
        for (int ring = 0; ring < 2; ++ring) {
            const float rt = std::clamp(t + ring * 0.18f, 0.0f, 1.0f);
            draw->AddCircle(*c, max_r * rt, with_alpha(base, (1.0f - rt) * 0.85f), 0,
                            2.0f);
        }
        draw->AddCircleFilled(*c, font * 0.22f * fade + 1.0f, with_alpha(base, fade));
        if (!flow.label.empty() && t < 0.55f && mono) {
            const float ts = font * 0.78f;
            const float tw =
                mono->CalcTextSizeA(ts, FLT_MAX, 0.0f, flow.label.c_str()).x;
            draw->AddText(mono, ts, ImVec2(c->x - tw * 0.5f, c->y - max_r - ts),
                          with_alpha(base, fade), flow.label.c_str());
        }
    }
}

}  // namespace zetta::gui
