#pragma once

// Helpers shared by the per-view operations TUs (app_cluster.cpp,
// app_cloud.cpp, app_runs.cpp, app_models.cpp, app_datasets.cpp,
// app_jobs.cpp) — the pieces that used to be file-local to the monolithic
// app_operations.cpp but are used by more than one view. This is an INTERNAL
// header for the src/gui TUs only — it is NOT part of the public
// include/zetta surface, which is why it may freely include <imgui.h>.

#include <imgui.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zetta::ml {
class Model;
}

namespace zetta::gui {

// Cluster node-box palette (ImU32, for the window draw list) — the same Orama
// canvas grammar as the graph workspace: white cards with hairline borders, a
// soft wash on hover, a coral border (the "you are here" accent) when selected.
// Shared by the Cluster view's node-box canvas and the Datasets view's
// partition-placement overlay (which reuses the spatial grammar).
inline constexpr ImU32 kClusterBoxFill = IM_COL32(255, 255, 255, 255);          // white
inline constexpr ImU32 kClusterBoxFillHover = IM_COL32(242, 242, 240, 255);     // wash
inline constexpr ImU32 kClusterBoxFillSelected = IM_COL32(247, 238, 239, 255);  // coral wash
inline constexpr ImU32 kClusterBoxBorder = IM_COL32(224, 224, 224, 255);        // border
inline constexpr ImU32 kClusterBoxBorderSelected = IM_COL32(226, 84, 90, 255);  // coral
inline constexpr ImU32 kClusterBoxText = IM_COL32(28, 28, 26, 255);             // ink-primary
inline constexpr ImU32 kClusterBoxMuted = IM_COL32(95, 95, 92, 255);            // ink-muted

// The node's first UUID segment — enough to tell nodes apart at a glance.
// Shared by the Cluster view and the Datasets partition-placement overlay.
inline std::string cluster_node_short_id(const std::string& node_id) {
    return node_id.substr(0, node_id.find('-'));
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

}  // namespace zetta::gui
