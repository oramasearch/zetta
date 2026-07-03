#include "gui/repl_console.hpp"

#include "gui/app_internal.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <string>

namespace zetta::gui {

namespace {

// The combo + transcript tag label for a frontend ("SQL", "Python (simulated)").
std::string label_of(repl::Frontend frontend) {
    return std::string(repl::frontend_label(frontend));
}

}  // namespace

void ReplConsole::draw(repl::ConsoleModel& model, const repl::RunHandler& run,
                       ImFont* mono_font) {
    // ── Frontend selector ──────────────────────────────────────────────────
    // Which language the source is written in. The model records it on each
    // statement, so a mixed-frontend transcript stays self-labelled.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const repl::Frontend current = model.frontend();
    if (ImGui::BeginCombo("##repl_frontend", label_of(current).c_str())) {
        for (const repl::Frontend frontend : repl::frontends()) {
            const bool selected = frontend == current;
            if (ImGui::Selectable(label_of(frontend).c_str(), selected)) {
                model.set_frontend(frontend);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Language");

    // ── Multiline editor ───────────────────────────────────────────────────
    // The source is code: render it in Geist Mono, bound straight to the model's
    // input buffer (imgui_stdlib's std::string overload).
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::InputTextMultiline("##repl_input", &model.input(),
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6.0f));
    if (mono_font) ImGui::PopFont();

    // ── Run / Clear controls ───────────────────────────────────────────────
    bool run_clicked = primary_button("Run");
    // Cmd/Ctrl+Enter runs too — the REPL idiom. Only evaluated while this view
    // is drawn, so it can't fire from another panel.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_Enter)) {
        run_clicked = true;
    }
    ImGui::SameLine();
    const bool clear_clicked = ImGui::Button("Clear");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear the transcript");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu in transcript · Cmd/Ctrl+Enter to run", model.size());

    if (run_clicked) {
        // submit() emits the run event (invokes `run`) and records the outcome;
        // a blank statement is a no-op, so nothing scrolls.
        if (model.submit(run)) scroll_to_bottom_ = true;
    }
    if (clear_clicked) {
        model.clear();
        scroll_to_bottom_ = false;
    }

    ImGui::Spacing();
    draw_transcript(model, mono_font);
}

void ReplConsole::draw_transcript(const repl::ConsoleModel& model,
                                  ImFont* mono_font) {
    eyebrow(mono_font, "TRANSCRIPT");
    if (ImGui::BeginChild("##repl_transcript", ImVec2(0, 0),
                          ImGuiChildFlags_Borders)) {
        if (model.empty()) {
            // The next action is the editor + Run directly above (#631).
            empty_state("No statements yet",
                        "Pick a frontend, write a statement, and run it; the "
                        "transcript keeps every statement and its result.",
                        "");
        }
        int index = 0;
        for (const repl::Entry& entry : model.transcript()) {
            ImGui::PushID(index++);

            // Prompt line: a muted "<frontend> >" tag, then the source in mono.
            const std::string prompt = label_of(entry.frontend) + " >";
            eyebrow(mono_font, prompt.c_str());
            if (mono_font) ImGui::PushFont(mono_font);
            ImGui::TextWrapped("%s", entry.source.c_str());
            if (mono_font) ImGui::PopFont();

            draw_result(entry.result, mono_font);

            ImGui::PopID();
            ImGui::Separator();
        }
        if (scroll_to_bottom_) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom_ = false;
        }
    }
    ImGui::EndChild();
}

void ReplConsole::draw_result(const repl::Result& result, ImFont* mono_font) {
    // Errors display inline (red) and replace the grid — a failed statement
    // produced no rows.
    if (!result.error.empty()) {
        ImGui::TextColored(tone_color(Tone::Error), "%s", result.error.c_str());
        return;
    }
    bool any_message = false;
    for (const std::string& message : result.messages) {
        ImGui::TextColored(kInkMuted, "%s", message.c_str());
        any_message = true;
    }
    if (!result.elapsed.empty()) {
        if (any_message) ImGui::SameLine();
        ImGui::TextDisabled("(%s)", result.elapsed.c_str());
    }
    draw_result_grid(result, mono_font);
}

void ReplConsole::draw_result_grid(const repl::Result& result,
                                   ImFont* mono_font) {
    if (result.columns.empty()) return;  // DDL / INSERT — status only, no grid.

    constexpr std::size_t kMaxRows = 1000;
    const std::size_t shown = std::min(result.rows.size(), kMaxRows);

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable;
    // Fit the grid to its content, capped so a large result scrolls in place.
    const std::size_t visible_rows = std::min<std::size_t>(shown + 1, 12);
    const float height =
        ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(visible_rows);

    if (mono_font) ImGui::PushFont(mono_font);
    if (ImGui::BeginTable("##repl_grid", static_cast<int>(result.columns.size()),
                          flags, ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);  // keep the header pinned
        for (const std::string& column : result.columns) {
            ImGui::TableSetupColumn(column.c_str());
        }
        ImGui::TableHeadersRow();
        for (std::size_t row = 0; row < shown; ++row) {
            const std::vector<std::string>& cells = result.rows[row];
            ImGui::TableNextRow();
            for (std::size_t column = 0; column < result.columns.size(); ++column) {
                ImGui::TableSetColumnIndex(static_cast<int>(column));
                if (column < cells.size()) {
                    ImGui::TextUnformatted(cells[column].c_str());
                } else {
                    ImGui::TextDisabled("·");
                }
            }
        }
        ImGui::EndTable();
    }
    if (mono_font) ImGui::PopFont();

    if (result.rows.size() > shown) {
        ImGui::TextColored(kInkQuiet, "... and %zu more",
                           result.rows.size() - shown);
    }
}

}  // namespace zetta::gui
