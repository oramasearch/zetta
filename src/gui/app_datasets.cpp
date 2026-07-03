#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/cluster/cluster.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/util/bytes.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zetta::gui {

void App::draw_datasets() {
    ImGui::SeparatorText("Datasets");

    // The GUI form of system.datasets (#585; the area-3 dataset view of epic
    // #575): one dataset == one table, summarized live from the node tree + the
    // replicate-to-all partition catalog. A thin, read-only viewer — schema, row
    // count, on-disk size, storage format, replication strategy, and partition
    // placement across the cluster (which node owns which partition). Recomputed
    // each frame via Node::datasets(), so it never drifts from the truth; the only
    // UI state is the selected dataset's table id (re-resolved each frame).
    const std::vector<zetta::data::DatasetSummary> datasets = node_.datasets();

    // Add-dataset affordance (#587): pick a parquet/jsonl/json/csv file and load
    // it as a *partitioned* table, so it joins the list below with its partitions
    // placed across the cluster. A file dropped onto this view does the same (see
    // the SDL_EVENT_DROP_FILE routing in run()). The dialog filter mirrors
    // dataset_file::dialog_extensions().
    if (primary_button("Add dataset...")) {
        static const SDL_DialogFileFilter filters[] = {
            {"Datasets (parquet, jsonl, json, csv)",
             "parquet;pq;jsonl;ndjson;json;csv"},
            {"All files", "*"}};
        SDL_ShowOpenFileDialog(&App::on_open_dataset, this, window_, filters, 2,
                               nullptr, false);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or drop a file here · %zu dataset(s)", datasets.size());
    // The shared ingestion status line (e.g. "Added dataset … (N rows, M
    // partitions)") — also shown on Home, surfaced here so an add gives feedback
    // in place.
    if (!io_status_.empty()) {
        ImGui::TextColored(kInkSecondary, "%s", io_status_.c_str());
    }
    ImGui::Spacing();

    // Re-resolve the selection against the live datasets: if the selected table is
    // gone (or nothing is selected yet), fall back to the first dataset so the
    // detail pane always shows a real one.
    const auto find_selected = [&]() -> const zetta::data::DatasetSummary* {
        for (const zetta::data::DatasetSummary& dataset : datasets)
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
    for (const zetta::data::DatasetSummary& dataset : datasets) {
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
    if (const zetta::data::DatasetSummary* dataset = find_selected()) {
        eyebrow(mono_font_, "DATASET");
        mono_line(kInkPrimary, "name        ", dataset->name);
        mono_line(kInkSecondary, "database    ", dataset->database);
        mono_line(kInkQuiet, "table id    ", dataset->table_id);

        ImGui::Spacing();
        eyebrow(mono_font_, "SHAPE");
        mono_line(kInkSecondary, "format      ", dataset->format);
        mono_line(kInkSecondary, "replication ", dataset->replication);
        mono_line(kInkSecondary, "rows        ", std::to_string(dataset->rows));
        mono_line(kInkSecondary, "size        ", zetta::util::format_bytes(dataset->size_bytes));
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

void App::draw_dataset_placement(const zetta::data::DatasetSummary& dataset) {
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
    std::unordered_map<std::string, std::vector<const zetta::data::DatasetPlacement*>> owned;
    std::vector<std::string> node_order;
    const auto note_node = [&](const std::string& node_id) {
        if (owned.find(node_id) == owned.end()) {
            owned.emplace(node_id, std::vector<const zetta::data::DatasetPlacement*>{});
            node_order.push_back(node_id);
        }
    };
    const std::vector<zetta::cluster::NodeStatus> nodes = cluster_.nodes();
    std::unordered_set<std::string> is_member;
    std::string self_id;
    for (const zetta::cluster::NodeStatus& node : nodes) {
        note_node(node.node_id);
        is_member.insert(node.node_id);
        if (node.is_self) self_id = node.node_id;
    }
    for (const zetta::data::DatasetPlacement& placement : dataset.partitions) {
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
            const std::vector<const zetta::data::DatasetPlacement*>& parts = owned[node_id];
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
            for (const zetta::data::DatasetPlacement* placement : parts) {
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
        for (const zetta::data::DatasetPlacement& placement : dataset.partitions) {
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

}  // namespace zetta::gui
