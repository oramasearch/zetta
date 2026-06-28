#pragma once

// Shared design-system tokens + small render helpers for the headed GUI, split
// out of app.cpp so every per-panel translation unit (app_home.cpp,
// app_explorer.cpp, app_pipeline.cpp, app_operations.cpp, app_governance.cpp,
// and app.cpp itself) can use them. This is an INTERNAL header for the src/gui
// TUs only — it is NOT part of the public include/zetta surface, which is why it
// may freely include <imgui.h>.

#include <imgui.h>

#include <cctype>
#include <string>

namespace zetta::gui {

// ── Orama design-system palette (light) ───────────────────────────────────
// Exact tokens from design-system/colors_and_type.css. Coral is the brand
// "you are here" accent — used surgically (active nav, one focal action), NEVER
// for status. Status uses the semantic green/amber/red below. See CLAUDE.md.
inline constexpr ImVec4 kCoral{0.886f, 0.329f, 0.353f, 1.0f};       // #E2545A
inline constexpr ImVec4 kCoralHover{0.831f, 0.263f, 0.286f, 1.0f};  // #D44349
inline constexpr ImVec4 kInkPrimary{0.110f, 0.110f, 0.102f, 1.0f};  // #1C1C1A
inline constexpr ImVec4 kInkSecondary{0.267f, 0.267f, 0.255f, 1.0f};// #444441
inline constexpr ImVec4 kInkMuted{0.373f, 0.373f, 0.361f, 1.0f};    // #5F5F5C
inline constexpr ImVec4 kInkQuiet{0.490f, 0.490f, 0.478f, 1.0f};    // #7D7D7A
inline constexpr ImVec4 kCanvas{0.980f, 0.980f, 0.973f, 1.0f};      // #FAFAF8
inline constexpr ImVec4 kSidebar{0.961f, 0.957f, 0.949f, 1.0f};     // #F5F4F2
inline constexpr ImVec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};             // #FFFFFF
inline constexpr ImVec4 kBorder{0.878f, 0.878f, 0.878f, 1.0f};      // #E0E0E0

// Semantic state palette (product only) — for status, never coral.
inline constexpr ImVec4 kSuccessColor{0.059f, 0.431f, 0.337f, 1.0f};// fresh-fg #0F6E56
inline constexpr ImVec4 kDotActive{0.114f, 0.620f, 0.459f, 1.0f};   // dot-active #1D9E75
inline constexpr ImVec4 kWarnColor{0.522f, 0.310f, 0.043f, 1.0f};   // stale-fg #854F0B
inline constexpr ImVec4 kErrorColor{0.706f, 0.137f, 0.094f, 1.0f};  // error #B42318

// Correctness-gate status colours: green pass, amber warn, red fail.
inline constexpr ImVec4 kPassColor = kSuccessColor;
inline constexpr ImVec4 kFailColor = kErrorColor;

// "unsaved edits" / dirty marker colour (stale amber).
inline constexpr ImVec4 kUnsavedColor = kWarnColor;

// A black wash at the given alpha — the design system's hover/selected washes
// and ghost-button fills (rgba(0,0,0,a)), which blend over whatever is behind.
inline constexpr ImVec4 wash(float alpha) { return ImVec4(0.0f, 0.0f, 0.0f, alpha); }

// A small, valid execution graph (the IR's canonical JSON form) prefilled into
// the gate panel so the passing path is one click away — a churn-flavoured
// connect→clean→model→serve pipeline. (Also the App ctor's default gate input.)
inline constexpr const char* kExampleGraphJson = R"({
  "nodes": [
    { "id": "load",  "kind": "connect", "name": "load_customers" },
    { "id": "clean", "kind": "clean",   "name": "dedupe" },
    { "id": "churn", "kind": "model",   "name": "churn_model" },
    { "id": "serve", "kind": "serve",   "name": "expose_scores" }
  ],
  "edges": [
    { "from": "load",  "to": "clean" },
    { "from": "clean", "to": "churn" },
    { "from": "churn", "to": "serve" }
  ]
})";

// An empty graph (the IR's canonical JSON form) — fails NonEmptyGraphCheck, so
// it demonstrates the deploy-blocking path.
inline constexpr const char* kEmptyGraphJson = R"({
  "nodes": [],
  "edges": []
})";

// Ambient Geist Mono face for the file-local render_* helpers (which aren't App
// methods, so they can't reach App::mono_font_). Set once per frame at the top of
// render_frame(); null until the first atlas build. Like ImGui's current-font
// concept, but used to mono just the data bits (ids / timestamps / numbers).
// Declared here, defined once in app.cpp.
extern ImFont* g_mono_font;

inline std::string to_upper(std::string text) {
    for (char& character : text) {
        character =
            static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

// Draw a small filled status dot inline (the design system's 6px indicator
// dot), then SameLine so the caller's next item sits to its right. Green =
// active/open, amber = locked/attention.
inline void status_dot(ImVec4 color) {
    const float size = ImGui::GetFontSize();
    const float radius = size * 0.26f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 center(pos.x + radius, pos.y + size * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(center, radius,
                                                ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(radius * 2.0f, size));
    ImGui::SameLine(0.0f, 7.0f);
}

// A mono uppercase eyebrow label (the design system's section-label role). Needs
// the mono font pushed by the caller when available.
inline void eyebrow(ImFont* mono, const char* text) {
    if (mono) ImGui::PushFont(mono);
    ImGui::TextColored(kInkMuted, "%s", text);
    if (mono) ImGui::PopFont();
}

// Render one line of mono data (an id, timestamp, or number) in the given
// colour, falling back to the default font when mono isn't available yet.
inline void mono_line(ImVec4 color, const char* label, const std::string& value) {
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(color, "%s%s", label, value.c_str());
    if (g_mono_font) ImGui::PopFont();
}

// The one focal action on a surface: a charcoal-filled, white-text button (the
// design system's primary button). Everything else stays a ghost button. Coral
// is never a button fill here (there is no "send").
inline bool primary_button(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, kInkPrimary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kInkSecondary);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kInkMuted);
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return clicked;
}

}  // namespace zetta::gui
