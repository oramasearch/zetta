#pragma once

#include <memory>
#include <string>

// Forward declarations keep SDL3 + Dear ImGui headers out of this file — only
// app.cpp (and no other translation unit) needs the full definitions.
struct SDL_Window;
struct SDL_Renderer;
struct ImGuiStyle;

namespace zetta {

class Node;

namespace gui {

// The headed run mode: an SDL3 + Dear ImGui window rendering a live view of a
// zetta::Node. Owns the window, renderer, and ImGui context for its lifetime —
// construct it, init(), run(), then let it destruct.
class App {
public:
    explicit App(Node& node);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Initialize SDL + ImGui and open the window. Returns false on failure
    // (a diagnostic is written to stderr first).
    bool init();

    // Run the event/render loop until the user quits. Ticks the node once per
    // frame. Must be called only after a successful init().
    void run();

private:
    // Re-rasterize the font and rescale the style for the current display.
    // See app.cpp for the content-scale vs pixel-density rationale.
    void apply_display_scale();
    void draw_menu_bar(bool& running);
    void draw_dashboard();
    void draw_store();

    Node& node_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool initialized_ = false;
    bool show_demo_ = false;

    // "Store" panel: the JSON document being composed, and the last add error.
    char doc_input_[4096] = R"({"name": "example", "value": 1})";
    std::string store_error_;

    // Display-scale tracking — the last values we rasterized/scaled for, so a
    // no-change display event is a cheap no-op.
    float applied_content_scale_ = 0.0f;
    float applied_pixel_density_ = 0.0f;

    // Snapshot of the default ImGui style, taken once at init and re-applied
    // (then scaled) on every display-scale change. Held by pointer so this
    // header needn't include <imgui.h> for the full ImGuiStyle definition.
    std::unique_ptr<ImGuiStyle> base_style_;
};

}  // namespace gui
}  // namespace zetta
