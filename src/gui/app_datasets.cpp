#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/cluster/cluster.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/util/bytes.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zetta::gui {

void App::draw_datasets() {
    page_header("Datasets",
                "Every dataset: schema, size, format, and partition placement");

    // The GUI form of system.datasets (#585; the area-3 dataset view of epic
    // #575): one dataset == one table, summarized live from the node tree + the
    // replicate-to-all partition catalog. A thin, read-only viewer — schema, row
    // count, on-disk size, storage format, replication strategy, and partition
    // placement across the cluster (which node owns which partition). Recomputed
    // each frame via Node::datasets(), so it never drifts from the truth; the only
    // UI state is the selected dataset's table id (re-resolved each frame).
    const std::vector<zetta::data::DatasetSummary> datasets = node_.datasets();
    std::optional<std::string> pending = take_pending_selection(View::Datasets);

    // Add-dataset affordance (#587): pick a parquet/jsonl/json/csv file and load
    // it as a *partitioned* table, so it joins the list below with its partitions
    // placed across the cluster. A file dropped onto this view does the same (see
    // the SDL_EVENT_DROP_FILE routing in run()). The dialog filter mirrors
    // dataset_file::dialog_extensions().
    const auto open_add_dataset_dialog = [this] {
        static const SDL_DialogFileFilter filters[] = {
            {"Datasets (parquet, jsonl, json, csv)",
             "parquet;pq;jsonl;ndjson;json;csv"},
            {"All files", "*"}};
        SDL_ShowOpenFileDialog(&App::on_open_dataset, this, window_, filters, 2,
                               nullptr, false);
    };

    // The nothing-yet case (#631): the empty state carries the one next action
    // itself, so the header button row isn't duplicated beside it.
    if (datasets.empty()) {
        if (empty_state("No datasets yet",
                        "Add a parquet, jsonl, json, or csv file (or drop one "
                        "on the window) and it becomes a partitioned table "
                        "across the cluster.",
                        "Add dataset...")) {
            open_add_dataset_dialog();
        }
        if (!io_status_.empty()) {
            ImGui::TextColored(kInkSecondary, "%s", io_status_.c_str());
        }
        return;
    }

    if (primary_button("Add dataset...")) open_add_dataset_dialog();
    ImGui::SameLine();
    ImGui::TextDisabled("or drop a file here · %zu dataset(s)", datasets.size());
    // The in-flight add's live progress (#624): the decode runs on a worker,
    // the materialize in bounded owner-thread slices, so this panel keeps
    // rendering — with a real fraction once the row total is known, and a
    // cancel that stops the add and drops the partial table.
    if (dataset_add_) {
        const std::size_t total = dataset_add_->rows_total();
        char progress_line[160];
        float fraction = -1.0f;  // indeterminate while decoding
        using Phase = zetta::data::AsyncDatasetAdd::Phase;
        switch (dataset_add_->phase()) {
            case Phase::Decoding:
                std::snprintf(progress_line, sizeof(progress_line),
                              "Decoding %s · %zu row(s)",
                              dataset_add_->source_path().filename().c_str(),
                              dataset_add_->rows_decoded());
                break;
            case Phase::Materializing:
                std::snprintf(progress_line, sizeof(progress_line),
                              "Adding rows · %zu/%zu",
                              dataset_add_->rows_added(), total);
                fraction = total > 0 ? static_cast<float>(
                                           dataset_add_->rows_added()) /
                                           static_cast<float>(total)
                                     : 0.0f;
                break;
            case Phase::Partitioning:
                std::snprintf(progress_line, sizeof(progress_line),
                              "Placing partitions across the cluster…");
                fraction = 1.0f;
                break;
            case Phase::Done:
                progress_line[0] = '\0';
                break;
        }
        if (progress_line[0] != '\0') {
            status_label(Tone::Active, progress_line);
            ImGui::SameLine();
            ImGui::BeginDisabled(dataset_add_->cancel_requested());
            if (ImGui::SmallButton("Cancel")) dataset_add_->request_cancel();
            ImGui::EndDisabled();
            // ImGui renders an animated negative fraction as an indeterminate
            // marquee (the decode's total is unknown until it finishes).
            const float bar =
                fraction < 0.0f
                    ? -1.0f * static_cast<float>(ImGui::GetTime())
                    : fraction;
            ImGui::ProgressBar(bar, ImVec2(260.0f, 0.0f),
                               fraction < 0.0f ? "" : nullptr);
        }
    } else {
        // The shared ingestion outcome (e.g. "Added dataset … (N rows, M
        // partitions)") — also shown on Home, surfaced here so an add gives
        // feedback in place, toned by whether the ingest failed (#632).
        banner(io_status_error_ ? Tone::Error : Tone::Ok, io_status_);
    }
    ImGui::Spacing();

    // Each row's title is the dataset name (the table name) with the on-disk
    // format and the owning database as meta, so same-named tables across
    // databases stay distinguishable. Datasets have no lifecycle (muted dot).
    std::vector<MasterRow> rows;
    rows.reserve(datasets.size());
    for (const zetta::data::DatasetSummary& dataset : datasets) {
        rows.push_back({.id = dataset.table_id,
                        .title = dataset.name,
                        .meta = dataset.format + " · " + dataset.database,
                        .id_title = true});
    }

    master_detail(
        "datasets", rows, selected_dataset_, std::move(pending),
        "(no datasets)",
        "Select a dataset to view its schema, size, and partition placement "
        "across the cluster.",
        [&](const MasterRow& row) {
            const zetta::data::DatasetSummary* dataset = nullptr;
            for (const zetta::data::DatasetSummary& candidate : datasets) {
                if (candidate.table_id == row.id) {
                    dataset = &candidate;
                    break;
                }
            }
            if (!dataset) return;

            // The selected dataset's detail — identity, shape (format /
            // replication / rows / size), schema, and the placement overlay.
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
                ImGui::TextDisabled("(no columns · the dataset is empty)");
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
        });
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
                                owns ? kCanvasBoxFillSelected : kCanvasBoxFill,
                                rounding);
            draw->AddRect(top_left, bottom_right,
                          owns ? kCanvasBoxBorderSelected : kCanvasBoxBorder,
                          rounding, 0, owns ? 2.0f : 1.0f);

            draw->PushClipRect(top_left, bottom_right, true);
            const bool is_self = node_id == self_id;
            const bool departed = is_member.find(node_id) == is_member.end();
            std::string id = entity_short_id(node_id);
            if (is_self) id += " (self)";
            else if (departed) id += " (gone)";
            const ImVec2 id_pos(top_left.x + pad, top_left.y + pad);
            const ImU32 id_color = departed ? kCanvasBoxMuted : kCanvasBoxText;
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
            const ImU32 caption_color = owns ? kCanvasBoxText : kCanvasBoxMuted;
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
                    draw->AddText(mono_font_, small, rows_pos, kCanvasBoxMuted,
                                  rows_caption);
                } else {
                    draw->AddText(rows_pos, kCanvasBoxMuted, rows_caption);
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
                               entity_short_id(placement.partition_id).c_str());
            ImGui::TableNextColumn();
            char range[64];
            std::snprintf(range, sizeof(range), "[%llu, %llu)",
                          static_cast<unsigned long long>(placement.row_start),
                          static_cast<unsigned long long>(placement.row_end));
            ImGui::TextColored(kInkSecondary, "%s", range);
            ImGui::TableNextColumn();
            // Each owner is a one-click hop to the Cluster view with that
            // node's box selected (#633) — the dataset → cluster edge.
            if (placement.owner_nodes.empty()) {
                ImGui::TextColored(kInkSecondary, "(unplaced)");
            } else {
                for (std::size_t owner_index = 0;
                     owner_index < placement.owner_nodes.size(); ++owner_index) {
                    const std::string& owner =
                        placement.owner_nodes[owner_index];
                    if (owner_index != 0) {
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::TextColored(kInkSecondary, ", ");
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    ImGui::PushID(static_cast<int>(owner_index));
                    entity_link(entity_short_id(owner).c_str(), View::Cluster,
                                owner);
                    ImGui::PopID();
                }
            }
            if (g_mono_font) ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

}  // namespace zetta::gui
