// zetta — SDL3 + Dear ImGui desktop app.
//
// This is a minimal, HiDPI-correct starter: SDL3 (static, Metal-backed on
// macOS) drives the window + input + renderer; Dear ImGui draws the UI in
// immediate mode. The non-obvious parts worth keeping are the per-monitor
// scaling (apply_display_scale) and the framebuffer render-scale fix for
// SDL_WINDOW_HIGH_PIXEL_DENSITY — both documented inline below.

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>

#include "zetta_font_embedded.hpp"

namespace zetta {

// DPI / per-monitor UI scaling.
//
// Two distinct things conflate here, and SDL3 exposes them separately:
//   1) Pixel density (Retina, HiDPI) — already handled by the
//      imgui_impl_sdlrenderer3 backend, which reads
//      io.DisplayFramebufferScale and calls SDL_SetRenderScale so
//      drawing in logical pixels renders crisply at native pixels.
//      Don't multiply this in again.
//   2) Content scale — the OS's "the user wants the UI ~Nx bigger on
//      this display" hint (macOS scaled modes, Windows DPI scaling,
//      GNOME text-scale). This one is on us; ImGui won't apply it.
//
// SDL_GetWindowDisplayScale = pixel_density * content_scale, which is
// why naively using it doubles the UI on a Retina laptop. We want only
// the content scale, hence SDL_GetDisplayContentScale. On a stock Retina
// MBP with no extra scaling this returns 1.0 and apply_display_scale is a
// no-op; on a 4K external in a scaled mode it returns 1.5 / 2.0 and the
// UI grows to match.
constexpr float kBaseFontPixels = 15.0f;
ImGuiStyle g_base_style;
float g_applied_content_scale = 0.0f;
float g_applied_pixel_density = 0.0f;

float ui_content_scale(SDL_Window* window) {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) return 1.0f;
    const float s = SDL_GetDisplayContentScale(display);
    return s > 0.0f ? s : 1.0f;
}

float ui_pixel_density(SDL_Window* window) {
    const float d = SDL_GetWindowPixelDensity(window);
    return d > 0.0f ? d : 1.0f;
}

void apply_display_scale(SDL_Window* window) {
    const float content = ui_content_scale(window);
    const float density = ui_pixel_density(window);
    if (content == g_applied_content_scale &&
        density == g_applied_pixel_density) return;
    g_applied_content_scale = content;
    g_applied_pixel_density = density;

    // Font: rasterize the atlas at framebuffer-pixel size so the
    // renderer's SetRenderScale upscale is a no-op for texel→pixel
    // mapping (crisp text on Retina). Glyph metrics stay at logical
    // size via SizePixels so layout doesn't change.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = kBaseFontPixels * content;
    cfg.RasterizerDensity = density;
    cfg.FontDataOwnedByAtlas = false;  // bytes live in embedded header
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kDroidSans),
        static_cast<int>(kDroidSans_len),
        cfg.SizePixels,
        &cfg);
    io.Fonts->Build();
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
    ImGui_ImplSDLRenderer3_CreateFontsTexture();

    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base_style;
    style.ScaleAllSizes(content);
}

void draw_menu_bar(bool& running, bool& show_demo) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit", "Cmd+Q")) running = false;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("ImGui demo window", nullptr, &show_demo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("About zetta");
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

}  // namespace zetta

int main(int /*argc*/, char* /*argv*/[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Open at 70% of the primary display's usable area (i.e. excluding
    // the macOS menu bar + dock), with sensible floors for tiny
    // displays so the layout still has room to render.
    int win_w = 1280, win_h = 800;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
        usable.w > 0 && usable.h > 0) {
        win_w = static_cast<int>(usable.w * 0.70f);
        win_h = static_cast<int>(usable.h * 0.70f);
        if (win_w < 900) win_w = 900;
        if (win_h < 600) win_h = 600;
    }
    SDL_Window* window = SDL_CreateWindow(
        "zetta",
        win_w, win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    zetta::g_base_style = ImGui::GetStyle();
    zetta::apply_display_scale(window);

    bool running = true;
    bool show_demo = true;
    while (running) {
        // Sleep until the next input arrives or kIdleFrameMs elapses.
        // Without this the loop pegs ~60fps (or whatever vsync gives
        // us) burning CPU on identical frames. Wait wakes immediately
        // on mouse/keyboard, so interactive feel stays full-speed;
        // idle frames render at ~1/kIdleFrameMs fps.
        constexpr int kIdleFrameMs = 33;  // ~30fps when idle
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, kIdleFrameMs)) {
            // Timed out with no event — skip the inner drain.
            event.type = 0;
        }
        do {
            if (event.type == 0) break;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                       event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                zetta::apply_display_scale(window);
            }
        } while (SDL_PollEvent(&event));

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        constexpr ImGuiWindowFlags kRootFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
        ImGui::Begin("##zetta_root", nullptr, kRootFlags);

        zetta::draw_menu_bar(running, show_demo);

        ImGui::SeparatorText("zetta");
        ImGui::TextWrapped(
            "SDL3 + Dear ImGui starter. Edit src/main.cpp to build your UI; "
            "the ImGui demo window (View menu) is a live reference for every "
            "available widget.");

        ImGui::End();

        if (show_demo) ImGui::ShowDemoWindow(&show_demo);

        ImGui::Render();
        // SDL_WINDOW_HIGH_PIXEL_DENSITY gives us a framebuffer larger
        // than the logical window. ImGui draws in logical coords; the
        // SDLRenderer3 backend does NOT call SDL_SetRenderScale for us
        // (only its clip rects honor FramebufferScale), so without
        // this line the UI paints into the top-left logical-sized
        // quadrant of the framebuffer and the rest stays as the clear
        // color. The official example flags this same fix as commented-
        // out boilerplate; ours actually needs it because we enable
        // HIGH_PIXEL_DENSITY.
        SDL_SetRenderScale(renderer,
                           io.DisplayFramebufferScale.x,
                           io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
