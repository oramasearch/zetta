#pragma once

// Shared design-system tokens + small render helpers for the headed GUI, split
// out of app.cpp so every per-panel translation unit (app_home.cpp,
// app_explorer.cpp, app_pipeline.cpp, app_runs.cpp, app_models.cpp,
// app_datasets.cpp, app_jobs.cpp, app_cluster.cpp, app_cloud.cpp,
// app_governance.cpp, app_test_suite.cpp, and app.cpp itself) can use them.
// This is an INTERNAL header for the src/gui TUs only — it is NOT part of the
// public include/zetta surface, which is why it may freely include <imgui.h>.

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

// ── Status tones (#636) ────────────────────────────────────────────────────
// The closed set of visual states a status can render as. Panels obtain a Tone
// via the `tone_of(...)` overloads in tones.hpp (the ONE mapping site) and
// colour with tone_color / tone_background — never with ad-hoc palette picks.
enum class Tone { Ok, Active, Warn, Error, Muted };

inline constexpr ImVec4 tone_color(Tone tone) {
    switch (tone) {
        case Tone::Ok:     return kSuccessColor;
        case Tone::Active: return kDotActive;
        case Tone::Warn:   return kWarnColor;
        case Tone::Error:  return kErrorColor;
        case Tone::Muted:  return kInkQuiet;
    }
    return kInkQuiet;
}

// The light tint behind a banner/badge of the given tone (the design system's
// fresh/stale/error background tints; muted rides the sidebar surface).
inline constexpr ImVec4 tone_background(Tone tone) {
    switch (tone) {
        case Tone::Ok:     return ImVec4(0.902f, 0.957f, 0.937f, 1.0f);  // #E6F4EF
        case Tone::Active: return ImVec4(0.902f, 0.957f, 0.937f, 1.0f);  // #E6F4EF
        case Tone::Warn:   return ImVec4(0.984f, 0.949f, 0.890f, 1.0f);  // #FBF2E3
        case Tone::Error:  return ImVec4(0.984f, 0.918f, 0.910f, 1.0f);  // #FBEAE8
        case Tone::Muted:  return kSidebar;
    }
    return kSidebar;
}

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

// ── The widget grammar (#630) ──────────────────────────────────────────────
// Panels compose these (plus plain ImGui tables/plots) instead of hand-rolling
// visuals. A panel may NOT define local colours, paddings, or one-off buttons —
// a new visual becomes a grammar widget here first, then gets used. See
// docs/gui/architecture.md.

// Ambient Geist heading face (a larger Geist cut) for page_header titles. Set
// once per frame beside g_mono_font; null until the first atlas build.
// Declared here, defined once in app.cpp.
extern ImFont* g_heading_font;

// The title row page_header drew this frame — header_action right-aligns onto
// it. {0,0} outside a page_header. (Per-frame ambient, like g_mono_font.)
// g_page_header_action_right walks leftward as actions are placed.
inline ImVec2 g_page_header_title_min{};
inline float g_page_header_action_right = 0.0f;

// One per view, first thing in the panel: the view's name (heading face) and a
// one-line muted subtitle. Identical geometry everywhere. Actions belonging to
// the header render to its right via header_action(...) immediately after.
inline void page_header(const char* title, const char* subtitle) {
    ImGui::Spacing();
    g_page_header_title_min = ImGui::GetCursorScreenPos();
    g_page_header_action_right =
        g_page_header_title_min.x + ImGui::GetContentRegionAvail().x;
    if (g_heading_font) ImGui::PushFont(g_heading_font);
    ImGui::TextColored(kInkPrimary, "%s", title);
    if (g_heading_font) ImGui::PopFont();
    if (subtitle && subtitle[0] != '\0') {
        ImGui::TextColored(kInkMuted, "%s", subtitle);
    }
    ImGui::Spacing();
}

// A ghost button right-aligned on the page_header title row. Call directly
// after page_header; multiple actions stack right-to-left. Returns clicked.
inline bool header_action(const char* label) {
    const ImVec2 resume = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcTextSize(label).x +
                        ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorScreenPos(
        ImVec2(g_page_header_action_right - width, g_page_header_title_min.y));
    const bool clicked = ImGui::Button(label);
    g_page_header_action_right -=
        width + ImGui::GetStyle().ItemSpacing.x;  // next action lands left
    ImGui::SetCursorScreenPos(resume);
    return clicked;
}

// The bordered content unit: an eyebrow title + the caller-drawn body on a
// white card. Cards never nest. `card_id` must be unique within the view.
template <typename DrawContents>
inline void section_card(const char* card_id, const char* title,
                         DrawContents&& draw_contents) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kWhite);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild(card_id, ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    if (title && title[0] != '\0') {
        eyebrow(g_mono_font, to_upper(title).c_str());
        ImGui::Spacing();
    }
    draw_contents();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// A dashboard/summary number: mono value coloured by tone (Muted renders ink —
// a plain number, not a status), eyebrow label beneath. Callers lay tiles out
// horizontally with SameLine.
inline void stat_tile(const char* label, const std::string& value,
                      Tone tone = Tone::Muted) {
    ImGui::BeginGroup();
    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(tone == Tone::Muted ? kInkPrimary : tone_color(tone),
                       "%s", value.c_str());
    if (g_mono_font) ImGui::PopFont();
    eyebrow(g_mono_font, label);
    ImGui::EndGroup();
}

// The ONLY way a panel renders "nothing here" (#631): one headline, one
// sentence of muted body, and the single next action. Returns true when the
// action is clicked; pass "" to render no action.
inline bool empty_state(const char* headline, const char* body,
                        const char* action_label) {
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(kInkPrimary, "%s", headline);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kInkMuted, "%s", body);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool clicked = false;
    if (action_label && action_label[0] != '\0') {
        clicked = primary_button(action_label);
        ImGui::Spacing();
    }
    return clicked;
}

// Per-panel action feedback (#632): a tinted strip with a status dot and the
// message — for errors, the engine's path-qualified reason verbatim. Renders
// nothing for an empty message, so callers can pass their status line as-is.
// No modal alerts, ever.
inline void banner(Tone tone, const std::string& message) {
    if (message.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tone_background(tone));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("##banner", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    status_dot(tone_color(tone));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kInkPrimary, "%s", message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

}  // namespace zetta::gui
