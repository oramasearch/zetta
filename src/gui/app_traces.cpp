// The Traces view (#640, docs/gui/traces.md): the per-operation "what
// happened" pane over the node's span ring (Node::span_recorder() — the SAME
// source as system.traces / system.spans / get_trace, RULE-7). Left: recent
// traces, newest first, with intent + outcome. Right: the selected trace's
// spans and the selected span's four captures (intent · plan · duration ·
// result shape, error verbatim). The chronological who-asked feed is the
// Activity view (#622, app_activity.cpp); this view is the drill-down into
// one operation's spans. Thin renderer: the ring is read each frame; the
// per-trace grouping below is presentation, not a synced copy.
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/tones.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace zetta::gui {

namespace {

// A per-trace summary for the list rows — the same grouping the
// system.traces view computes (root = the span whose parent is not itself
// recorded; ok = every span succeeded).
struct TraceSummary {
    std::string trace_id;
    std::string name;    // the root span's name
    std::string intent;  // the root span's intent, when it stated one
    std::string started_at;
    double duration_ms = 0.0;
    std::size_t span_count = 0;
    bool ok = true;
    std::string error;  // the first failing span's reason
};

std::vector<TraceSummary> summarize_traces(
    const std::vector<zetta::observability::Span>& spans) {
    std::unordered_set<std::string> recorded_span_ids;
    for (const zetta::observability::Span& span : spans) {
        recorded_span_ids.insert(span.span_id);
    }

    std::vector<TraceSummary> summaries;
    std::unordered_set<std::string> seen;
    // Newest first: walk the ring backwards, appending a summary the first
    // time each trace id appears.
    for (auto span_iterator = spans.rbegin(); span_iterator != spans.rend();
         ++span_iterator) {
        if (!seen.insert(span_iterator->trace_id).second) continue;
        TraceSummary summary;
        summary.trace_id = span_iterator->trace_id;
        const zetta::observability::Span* root = nullptr;
        for (const zetta::observability::Span& span : spans) {
            if (span.trace_id != summary.trace_id) continue;
            ++summary.span_count;
            const bool is_root_candidate =
                span.parent_span_id.empty() ||
                !recorded_span_ids.contains(span.parent_span_id);
            if (is_root_candidate && !root) root = &span;
            if (summary.started_at.empty() ||
                span.started_at < summary.started_at) {
                summary.started_at = span.started_at;
            }
            if (!span.ok) {
                summary.ok = false;
                if (summary.error.empty()) summary.error = span.error;
            }
        }
        if (!root) continue;  // unreachable: the trace has at least one span
        summary.name = root->name;
        summary.duration_ms = root->duration_ms;
        if (const auto intent = root->attributes.find("intent");
            intent != root->attributes.end() && intent->is_string()) {
            summary.intent = intent->get<std::string>();
        }
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::string format_ms(double milliseconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f ms", milliseconds);
    return text;
}

// One attribute of the four captures, rendered as a wrapped mono block ("" /
// missing renders nothing). `value` may be any JSON shape.
void capture_block(const char* label, const nlohmann::json& attributes,
                   const char* key) {
    const auto found = attributes.find(key);
    if (found == attributes.end() || found->is_null()) return;
    eyebrow(g_mono_font, label);
    const std::string text =
        found->is_string() ? found->get<std::string>() : found->dump(2);
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kInkSecondary, "%s", text.c_str());
    ImGui::PopTextWrapPos();
    if (g_mono_font) ImGui::PopFont();
    ImGui::Spacing();
}

}  // namespace

void App::draw_traces() {
    // Consume a navigate_to(entity) deep link aimed at this view.
    if (auto pending_entity = take_pending_selection(View::Traces)) {
        selected_trace_ = *pending_entity;
        selected_span_.clear();
    }

    page_header("Traces",
                "Every operation on this node: intent, timing, and result "
                "shape, threaded by trace");

    const std::vector<zetta::observability::Span> spans =
        node_.span_recorder().snapshot();

    if (spans.empty()) {
        if (empty_state("Nothing has been traced yet",
                        "Run a query, a test case, or an MCP tool call and "
                        "its trace lands here.",
                        "Open SQL query")) {
            navigate_to(View::Query);
        }
        return;
    }

    const std::vector<TraceSummary> traces = summarize_traces(spans);
    std::size_t failed_traces = 0;
    for (const TraceSummary& trace : traces) {
        if (!trace.ok) ++failed_traces;
    }

    stat_tile("traces", std::to_string(traces.size()));
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
    stat_tile("spans", std::to_string(spans.size()));
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
    stat_tile("failed", std::to_string(failed_traces),
              failed_traces == 0 ? Tone::Muted : Tone::Error);
    ImGui::Spacing();

    const float list_width = ImGui::GetContentRegionAvail().x * 0.42f;

    // Left: recent traces, newest first — dot + root name, muted intent.
    ImGui::BeginChild("##trace_list", ImVec2(list_width, 0.0f),
                      ImGuiChildFlags_Borders);
    for (const TraceSummary& trace : traces) {
        ImGui::PushID(trace.trace_id.c_str());
        status_dot(tone_color(tone_of_span(trace.ok)));
        std::string label = trace.name;
        if (!trace.intent.empty()) label += " · " + trace.intent;
        if (ImGui::Selectable(label.c_str(),
                              selected_trace_ == trace.trace_id)) {
            selected_trace_ = trace.trace_id;
            selected_span_.clear();
        }
        mono_line(kInkQuiet,
                  "  ", format_ms(trace.duration_ms) + " · " +
                            std::to_string(trace.span_count) + " span(s) · " +
                            trace.started_at);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected trace's spans + the selected span's four captures.
    // Re-resolved from the ring each frame; an evicted trace simply empties.
    ImGui::SameLine();
    ImGui::BeginChild("##trace_detail", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    const std::vector<zetta::observability::Span> trace_spans =
        node_.span_recorder().trace(selected_trace_);
    if (trace_spans.empty()) {
        ImGui::TextDisabled("Select a trace to see what happened.");
    } else {
        mono_line(kInkQuiet, "trace ", selected_trace_);
        ImGui::Spacing();
        const zetta::observability::Span* selected_span = nullptr;
        for (const zetta::observability::Span& span : trace_spans) {
            ImGui::PushID(span.span_id.c_str());
            status_dot(tone_color(tone_of_span(span.ok)));
            const std::string label = span.name + " · " +
                                      format_ms(span.duration_ms);
            if (ImGui::Selectable(label.c_str(),
                                  selected_span_ == span.span_id)) {
                selected_span_ = span.span_id;
            }
            ImGui::PopID();
            if (selected_span_ == span.span_id) selected_span = &span;
        }
        if (!selected_span && !trace_spans.empty()) {
            selected_span = &trace_spans.front();
        }
        if (selected_span) {
            ImGui::Separator();
            ImGui::Spacing();
            mono_line(kInkQuiet, "span    ", selected_span->span_id);
            mono_line(kInkQuiet, "started ", selected_span->started_at);
            mono_line(kInkQuiet, "took    ",
                      format_ms(selected_span->duration_ms));
            ImGui::Spacing();
            banner(tone_of_span(selected_span->ok),
                   selected_span->ok ? std::string{} : selected_span->error);
            capture_block("INTENT", selected_span->attributes, "intent");
            capture_block("PLAN", selected_span->attributes, "plan");
            capture_block("RESULT SHAPE", selected_span->attributes,
                          "result_shape");
        }
    }
    ImGui::EndChild();
}

}  // namespace zetta::gui
