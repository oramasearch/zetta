// The Search view (#641): the GUI rendering of THE canonical search entry
// point, Node::search — the same path the `search` MCP capability calls, so
// the two surfaces can never diverge. Pick a registered Search index, type a
// query, and read back the global ranking with per-node provenance (which node
// answered, hits / latency / wire bytes) and the search's trace id.
//
// The search runs on the UI thread because the Node is main-thread-only; a
// live-cluster scatter answers in milliseconds (the per-peer timeout only
// bites when a connected peer stops answering). The last outcome is held as an
// action-outcome snapshot (like the SQL panel's last result) — presentation
// state, never a synced engine copy.

#include "gui/app.hpp"
#include "gui/app_internal.hpp"

#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/node.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace zetta::gui {

namespace {

// "1.2 ms" / "340 B" style compact formatting for the provenance table.
std::string format_latency(double milliseconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f ms", milliseconds);
    return text;
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

    // The registered Search-kind indexes — the live source the target picker
    // renders from each frame (no copies).
    std::vector<std::shared_ptr<zetta::index::Index>> search_indexes;
    for (const std::shared_ptr<zetta::index::Index>& index : node_.indexes()) {
        if (index && index->kind() == zetta::index::IndexKind::Search) {
            search_indexes.push_back(index);
        }
    }
    if (search_indexes.empty()) {
        if (empty_state("No search index yet",
                        "Register a search index over a table (an `index` step "
                        "in a pipeline, or create_index), then query it from "
                        "here or the `search` MCP tool.",
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
        }
    });

    // --- the last outcome ------------------------------------------------------
    banner(Tone::Error, search_error_);
    if (!search_result_ || !search_result_->ok()) return;
    const zetta::index::SearchResult& result = *search_result_;

    // The search's shape: which path answered, who answered, at what cost.
    stat_tile("Path", std::string(zetta::index::to_string(result.path)),
              result.path == zetta::index::SearchPath::Distributed
                  ? Tone::Ok
                  : Tone::Muted);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, 0.0f));
    ImGui::SameLine();
    stat_tile("Nodes answered", std::to_string(result.nodes.size()));
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, 0.0f));
    ImGui::SameLine();
    stat_tile("Hits", std::to_string(result.hits.size()));
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, 0.0f));
    ImGui::SameLine();
    stat_tile("Time", format_latency(result.duration_ms));
    ImGui::Spacing();

    section_card("##search-results", "Ranked hits", [&] {
        if (result.hits.empty()) {
            ImGui::TextColored(kInkMuted, "No rows matched the query.");
            return;
        }
        if (ImGui::BeginTable("##search-hits", 3,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 3.0f);
            ImGui::TableSetupColumn("Row id");
            ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 5.0f);
            ImGui::TableHeadersRow();
            int rank = 1;
            for (const zetta::index::SearchHit& hit : result.hits) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(kInkQuiet, "%d", rank++);
                ImGui::TableSetColumnIndex(1);
                if (g_mono_font) ImGui::PushFont(g_mono_font);
                ImGui::TextUnformatted(hit.row_id.c_str());
                if (g_mono_font) ImGui::PopFont();
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", hit.score);
            }
            ImGui::EndTable();
        }
    });

    // Per-node provenance: the scatter-gather made visible (#641) — the
    // distributed-first foundation wants the operator to SEE the whole cluster
    // answering.
    section_card("##search-provenance", "Per-node provenance", [&] {
        if (ImGui::BeginTable("##search-nodes", 5,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Node");
            ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 7.0f);
            ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 4.0f);
            ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 6.0f);
            ImGui::TableSetupColumn("Wire bytes",
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
                                                          : "peer");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", contribution.hits);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(
                    format_latency(contribution.latency_ms).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu",
                            static_cast<unsigned long long>(
                                contribution.wire_bytes));
            }
            ImGui::EndTable();
        }
        if (!result.trace_id.empty() && span_recorder_) {
            ImGui::Spacing();
            mono_line(kInkQuiet, "trace", result.trace_id);
        }
    });
}

}  // namespace zetta::gui
