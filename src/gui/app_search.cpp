// The Search view (#641, #643): the GUI rendering of THE canonical search
// entry point, Node::search — the same path the `search` MCP capability calls,
// so the two surfaces can never diverge. Pick a registered Search index, type a
// query, and read back the global ranking with the cluster made visible: a
// provenance strip (path · nodes answered · sub-indexes queried · time), a
// node-box strip replaying the scatter-gather as #590 flows, master-detail
// ranked hits whose detail shows the source row's fields, and per-node
// provenance (hits / sub-indexes / latency / wire bytes) with the search's
// trace id.
//
// The search runs on the UI thread because the Node is main-thread-only; a
// live-cluster scatter answers in milliseconds (the per-peer timeout only
// bites when a connected peer stops answering). The last outcome is held as an
// action-outcome snapshot (like the SQL panel's last result) — presentation
// state, never a synced engine copy; the flow replay is animation state
// anchored to the result's real per-node provenance, never an invented number.

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"
#include "gui/labels.hpp"

#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/engine.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zetta::gui {

namespace {

// "1.2 ms" style compact formatting for the provenance surfaces.
std::string format_latency(double milliseconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f ms", milliseconds);
    return text;
}

std::string format_score(double score) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", score);
    return text;
}

// The source row behind a hit, when this node stores it locally (the table's
// row store — a replicated table holds every row; a partition-placed table
// holds this node's shard). Read live each frame, never cached. nullopt = the
// row lives in a partition owned by a peer.
std::optional<nlohmann::json> find_local_row(zetta::Node& node,
                                             const std::string& table_name,
                                             const std::string& row_id) {
    for (const std::string& database_name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(database_name);
        if (!database) continue;
        const std::shared_ptr<zetta::data::Table> table =
            database->find_table(table_name);
        if (!table) continue;
        const zetta::storage::StorageEngine& store = table->store();
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.id_at(i) == row_id) {
                return std::make_optional<nlohmann::json>(store.at(i));
            }
        }
    }
    return std::nullopt;
}

// The searched table's dataset id ("" when no dataset lists it) — the target
// the hit detail's entity_link deep-links to.
std::string dataset_table_id(zetta::Node& node, const std::string& table_name) {
    for (const zetta::data::DatasetSummary& dataset : node.datasets()) {
        if (dataset.name == table_name) return dataset.table_id;
    }
    return {};
}

// Replay a completed search's scatter-gather as #590 flows: a scatter comet
// coordinator → each peer, a gather comet back (slightly delayed, labelled with
// the peer's pre-merge hits), and a local pulse for the coordinator's own
// contribution. Weights come from the real per-peer wire bytes — the animation
// grammar's rule that every flow is anchored to an instrumented number.
void append_search_flows(ClusterPipelineState& state,
                         const zetta::index::SearchResult& result) {
    std::string coordinator;
    for (const zetta::index::NodeContribution& contribution : result.nodes) {
        if (contribution.local) coordinator = contribution.node_id;
    }
    if (coordinator.empty() && !result.nodes.empty()) {
        coordinator = result.nodes.front().node_id;
    }

    std::uint64_t peak_bytes = 1;
    for (const zetta::index::NodeContribution& contribution : result.nodes) {
        peak_bytes = std::max(peak_bytes, contribution.wire_bytes);
    }
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

    std::size_t peer_index = 0;
    for (const zetta::index::NodeContribution& contribution : result.nodes) {
        if (contribution.local) {
            PipelineFlow pulse;
            pulse.kind = PipelineFlowKind::LocalCompute;
            pulse.to = contribution.node_id;
            pulse.lifetime = 1.0f;
            pulse.weight = 0.6f;
            pulse.label = std::to_string(contribution.hits) + " hits";
            spawn(std::move(pulse));
            continue;
        }
        const float delay = 0.06f * static_cast<float>(peer_index);
        const float weight = static_cast<float>(std::clamp(
            static_cast<double>(contribution.wire_bytes) /
                static_cast<double>(peak_bytes),
            0.25, 1.0));
        PipelineFlow scatter;
        scatter.kind = PipelineFlowKind::CollectiveScatter;
        scatter.from = coordinator;
        scatter.to = contribution.node_id;
        scatter.age = -delay;
        scatter.lifetime = 0.85f;
        scatter.weight = weight;
        scatter.label = "query";
        spawn(std::move(scatter));
        PipelineFlow gather;
        gather.kind = PipelineFlowKind::CollectiveGather;
        gather.from = contribution.node_id;
        gather.to = coordinator;
        gather.age = -(0.55f + delay);
        gather.lifetime = 0.9f;
        gather.weight = weight;
        gather.label = std::to_string(contribution.hits) + " hits";
        spawn(std::move(gather));
        ++peer_index;
    }
}

}  // namespace

void App::draw_search() {
    // A deep link (navigate_to(View::Search, index_id)) preselects the target.
    if (auto pending_entity = take_pending_selection(View::Search)) {
        search_target_ = *pending_entity;
    }
    page_header("Search",
                "Query a search index: distributed across the cluster when the "
                "table is partitioned");

    // Advance + cull the scatter-gather replay (presentation-only state).
    const float dt = ImGui::GetIO().DeltaTime;
    for (PipelineFlow& flow : search_flows_.flows) flow.age += dt;
    std::erase_if(search_flows_.flows,
                  [](const PipelineFlow& f) { return f.age >= f.lifetime; });
    const float decay = std::exp(-dt * 1.8f);
    for (auto& [id, level] : search_flows_.node_activity) level *= decay;
    std::erase_if(search_flows_.node_activity,
                  [](const auto& entry) { return entry.second < 0.01f; });

    // The registered Search-kind indexes — the live source the target picker
    // renders from each frame (no copies).
    std::vector<std::shared_ptr<zetta::index::Index>> search_indexes;
    for (const std::shared_ptr<zetta::index::Index>& index : node_.indexes()) {
        if (index && index->kind() == zetta::index::IndexKind::Search) {
            search_indexes.push_back(index);
        }
    }
    if (search_indexes.empty()) {
        if (empty_state("No indexes yet",
                        "Deploy a solution with an index step, or add an index "
                        "over a dataset's table.",
                        "Open the graph workspace")) {
            navigate_to(View::Graph);
        }
        return;
    }

    // --- the query row --------------------------------------------------------
    section_card("##search-input", "Query", [&] {
        // Target picker: the index's table (its id in mono beneath the hood).
        if (search_target_.empty()) search_target_ = search_indexes.front()->id();
        const char* preview = search_indexes.front()->table().c_str();
        for (const auto& index : search_indexes) {
            if (index->id() == search_target_) preview = index->table().c_str();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo("##search-target", preview)) {
            for (const auto& index : search_indexes) {
                const bool selected = index->id() == search_target_;
                const std::string label =
                    index->table() + " · " + index->backend();
                if (ImGui::Selectable(label.c_str(), selected)) {
                    search_target_ = index->id();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 12.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##search-query", "Type a query and press enter...",
            &search_query_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
        ImGui::InputInt("Top", &search_top_k_, 0);
        if (search_top_k_ < 1) search_top_k_ = 1;
        ImGui::SameLine();
        if (primary_button("Search") || submitted) {
            zetta::index::SearchRequest request;
            request.target = search_target_;
            request.query = search_query_;
            request.top_k = static_cast<std::size_t>(search_top_k_);
            zetta::index::SearchResult result =
                node_.search(request, span_recorder_);
            search_error_ = result.ok() ? std::string() : result.status.message;
            search_result_ = std::make_unique<zetta::index::SearchResult>(
                std::move(result));
            search_selected_hit_.clear();
            search_flows_ = {};  // a fresh replay for the new search
            if (search_result_->ok()) {
                append_search_flows(search_flows_, *search_result_);
            }
        }
    });

    // --- the last outcome ------------------------------------------------------
    banner(Tone::Error, search_error_);
    if (!search_result_ || !search_result_->ok()) return;
    const zetta::index::SearchResult& result = *search_result_;

    // The provenance strip — the search's shape at a glance: which path
    // answered (distributed = the whole cluster, Ok tone; consolidated = the
    // degraded single-node index, muted), who answered, from how many
    // per-partition sub-indexes, at what cost.
    std::size_t sub_indexes_queried = 0;
    for (const zetta::index::NodeContribution& contribution : result.nodes) {
        sub_indexes_queried += contribution.sub_indexes;
    }
    const auto tile_gap = [] {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, 0.0f));
        ImGui::SameLine();
    };
    stat_tile("Path", std::string(label_of(result.path)),
              result.path == zetta::index::SearchPath::Distributed
                  ? Tone::Ok
                  : Tone::Muted);
    term_hint(result.path == zetta::index::SearchPath::Distributed
                  ? "Every node searched its own share of the data."
                  : "Only this node's index answered (no placed partitions "
                    "or no cluster attached).");
    tile_gap();
    stat_tile("Nodes answered", std::to_string(result.nodes.size()));
    tile_gap();
    stat_tile("Sub-indexes queried", std::to_string(sub_indexes_queried));
    tile_gap();
    stat_tile("Hits", std::to_string(result.hits.size()));
    tile_gap();
    stat_tile("Time", format_latency(result.duration_ms));
    ImGui::Spacing();

    draw_search_cluster_strip(result);

    // --- ranked hits: master-detail, the detail showing the source row --------
    const std::string linked_table_id = dataset_table_id(node_, result.spec.table);
    std::vector<MasterRow> rows;
    rows.reserve(result.hits.size());
    for (std::size_t rank = 0; rank < result.hits.size(); ++rank) {
        const zetta::index::SearchHit& hit = result.hits[rank];
        rows.push_back({.id = hit.row_id,
                        .title = std::to_string(rank + 1) + "  " + hit.row_id,
                        .meta = format_score(hit.score),
                        .id_title = true});
    }
    master_detail(
        "search-hits", rows, search_selected_hit_,
        /*pending_selection=*/std::nullopt, "(no rows matched the query)",
        "Select a hit to view its score and source row.",
        [&](const MasterRow& row) {
            std::size_t rank = 0;
            const zetta::index::SearchHit* hit = nullptr;
            for (std::size_t i = 0; i < result.hits.size(); ++i) {
                if (result.hits[i].row_id == row.id) {
                    hit = &result.hits[i];
                    rank = i + 1;
                    break;
                }
            }
            if (!hit) return;
            detail_header(hit->row_id, Tone::Muted,
                          "rank " + std::to_string(rank));
            ImGui::Spacing();
            mono_line(kInkSecondary, "score", format_score(hit->score));
            mono_line(kInkSecondary, "source",
                      result.spec.table + " · " + result.spec.backend);
            if (!linked_table_id.empty()) {
                entity_link("Open dataset", View::Datasets, linked_table_id);
            }
            ImGui::Spacing();

            // The source row's fields, read live from the local store: the
            // indexed fields first (the snippet), the rest muted after them.
            const std::optional<nlohmann::json> source_row =
                find_local_row(node_, result.spec.table, hit->row_id);
            if (!source_row) {
                ImGui::TextColored(
                    kInkMuted,
                    "Row content is not stored on this node · it lives in a "
                    "partition owned by another node.");
                return;
            }
            const auto field_text = [](const nlohmann::json& value) {
                return value.is_string() ? value.get<std::string>()
                                         : value.dump();
            };
            for (const std::string& field : result.spec.fields) {
                if (!source_row->contains(field)) continue;
                eyebrow(g_mono_font, to_upper(field).c_str());
                ImGui::TextWrapped("%s", field_text((*source_row)[field]).c_str());
                ImGui::Spacing();
            }
            for (const auto& [key, value] : source_row->items()) {
                if (std::find(result.spec.fields.begin(),
                              result.spec.fields.end(),
                              key) != result.spec.fields.end()) {
                    continue;
                }
                mono_line(kInkQuiet, key.c_str(), field_text(value));
            }
        },
        ImGui::GetTextLineHeightWithSpacing() * 12.0f);
    ImGui::Spacing();

    // Per-node provenance: the scatter-gather made checkable (#641/#643) — the
    // distributed-first foundation wants the operator to SEE the whole cluster
    // answering, node by node.
    section_card("##search-provenance", "Per-node provenance", [&] {
        if (ImGui::BeginTable("##search-nodes", 6,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Node");
            ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 7.0f);
            ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 4.0f);
            ImGui::TableSetupColumn("Sub-indexes",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 6.0f);
            ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 6.0f);
            ImGui::TableSetupColumn("Bytes moved",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 6.0f);
            ImGui::TableHeadersRow();
            for (const zetta::index::NodeContribution& contribution :
                 result.nodes) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (g_mono_font) ImGui::PushFont(g_mono_font);
                ImGui::TextUnformatted(contribution.node_id.c_str());
                if (g_mono_font) ImGui::PopFont();
                ImGui::TableSetColumnIndex(1);
                status_dot(tone_color(contribution.local ? Tone::Active
                                                         : Tone::Ok));
                ImGui::SameLine();
                ImGui::TextUnformatted(contribution.local ? "this node"
                                                          : "other node");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", contribution.hits);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%zu", contribution.sub_indexes);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(
                    format_latency(contribution.latency_ms).c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%llu",
                            static_cast<unsigned long long>(
                                contribution.wire_bytes));
            }
            ImGui::EndTable();
        }
        if (!result.trace_id.empty() && span_recorder_) {
            ImGui::Spacing();
            mono_line(kInkQuiet, "trace", result.trace_id);
            ImGui::SameLine();
            entity_link("View trace", View::Traces, result.trace_id);
        }
    });
}

// The cluster strip: one box per answering node (coordinator + every scattered
// peer, from the result's provenance), the completed search's scatter-gather
// replayed over them as #590 flows. The same node-box + flow grammar as the
// Cluster view's canvas, compacted — the "distributed search made visible"
// payoff of #643.
void App::draw_search_cluster_strip(const zetta::index::SearchResult& result) {
    section_card("##search-cluster", "The cluster answering", [&] {
        const float font = ImGui::GetFontSize();
        const float box_w = font * 11.0f;
        const float box_h = font * 3.6f;
        const float gap = font * 1.4f;
        const float pad = font * 0.55f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = std::max(ImGui::GetContentRegionAvail().x, box_w);

        // Lay the boxes in a wrapping row and remember their centres — the
        // flow endpoints track a live resize / reflow.
        std::unordered_map<std::string, ImVec2> centers;
        float x = origin.x;
        float y = origin.y;
        for (const zetta::index::NodeContribution& contribution : result.nodes) {
            if (x + box_w > origin.x + avail && x > origin.x) {
                x = origin.x;
                y += box_h + gap;
            }
            const ImVec2 top_left(x, y);
            const ImVec2 bottom_right(x + box_w, y + box_h);
            // The soft "this node is working" ring while the replay touches it.
            const auto activity =
                search_flows_.node_activity.find(contribution.node_id);
            if (activity != search_flows_.node_activity.end()) {
                draw->AddRect(ImVec2(top_left.x - 2.0f, top_left.y - 2.0f),
                              ImVec2(bottom_right.x + 2.0f, bottom_right.y + 2.0f),
                              with_alpha(kFlowLocal, activity->second * 0.8f),
                              7.0f, 0, 2.0f);
            }
            draw->AddRectFilled(top_left, bottom_right, kCanvasBoxFill, 6.0f);
            // Coral border on the coordinating node — the "you are here"
            // accent; peers keep the hairline.
            draw->AddRect(top_left, bottom_right,
                          contribution.local ? kCanvasBoxBorderSelected
                                             : kCanvasBoxBorder,
                          6.0f, 0, contribution.local ? 2.0f : 1.0f);
            const std::string id = entity_short_id(contribution.node_id);
            const ImVec2 id_pos(top_left.x + pad, top_left.y + pad);
            const std::string shape = std::to_string(contribution.hits) +
                                      " hits · " +
                                      std::to_string(contribution.sub_indexes) +
                                      " sub-indexes";
            const ImVec2 shape_pos(top_left.x + pad, top_left.y + pad + font * 1.25f);
            const ImVec2 latency_pos(top_left.x + pad,
                                     top_left.y + pad + font * 2.35f);
            const float small = font * 0.85f;
            if (g_mono_font) {
                draw->AddText(g_mono_font, font, id_pos, kCanvasBoxText,
                              id.c_str());
                draw->AddText(g_mono_font, small, shape_pos, kCanvasBoxMuted,
                              shape.c_str());
                draw->AddText(g_mono_font, small, latency_pos, kCanvasBoxMuted,
                              format_latency(contribution.latency_ms).c_str());
            } else {
                draw->AddText(id_pos, kCanvasBoxText, id.c_str());
                draw->AddText(shape_pos, kCanvasBoxMuted, shape.c_str());
                draw->AddText(latency_pos, kCanvasBoxMuted,
                              format_latency(contribution.latency_ms).c_str());
            }
            centers[contribution.node_id] =
                ImVec2((top_left.x + bottom_right.x) * 0.5f,
                       (top_left.y + bottom_right.y) * 0.5f);
            x += box_w + gap;
        }

        // The replay, over the boxes (a comet can cross the gaps between them).
        for (const PipelineFlow& flow : search_flows_.flows) {
            draw_pipeline_flow(draw, g_mono_font, font, centers, flow, box_h);
        }

        // Reserve the laid-out area for ImGui's layout.
        ImGui::Dummy(ImVec2(avail, (y + box_h) - origin.y + font * 0.25f));
    });
    ImGui::Spacing();
}

}  // namespace zetta::gui
