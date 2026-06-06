#include "gui/app.hpp"

#include "zetta/node.hpp"
#include "zetta/signals.hpp"
#include "zetta/version.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>

#include "zetta_font_embedded.hpp"

namespace zetta::gui {
namespace {

// Base logical font size; multiplied by the OS content scale per display.
constexpr float kBaseFontPixels = 15.0f;

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

}  // namespace

App::App(Node& node) : node_(node) {}

App::~App() {
    if (initialized_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    if (initialized_) SDL_Quit();
}

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // Open at 70% of the primary display's usable area (excluding the macOS
    // menu bar + dock), with floors so the layout still has room on a small
    // display.
    int win_w = 1280, win_h = 800;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
        usable.w > 0 && usable.h > 0) {
        win_w = static_cast<int>(usable.w * 0.70f);
        win_h = static_cast<int>(usable.h * 0.70f);
        if (win_w < 900) win_w = 900;
        if (win_h < 600) win_h = 600;
    }

    char title[64];
    std::snprintf(title, sizeof(title), "zetta %s", kVersion);
    window_ = SDL_CreateWindow(
        title, win_w, win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_ == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    base_style_ = std::make_unique<ImGuiStyle>(ImGui::GetStyle());
    initialized_ = true;
    apply_display_scale();
    return true;
}

// DPI / per-monitor UI scaling.
//
// Two distinct things conflate here, and SDL3 exposes them separately:
//   1) Pixel density (Retina, HiDPI) — already handled by the
//      imgui_impl_sdlrenderer3 backend, which reads io.DisplayFramebufferScale
//      and calls SDL_SetRenderScale so logical-pixel drawing renders crisply
//      at native pixels. Don't multiply this in again.
//   2) Content scale — the OS "make the UI ~Nx bigger on this display" hint
//      (macOS scaled modes, Windows DPI scaling, GNOME text-scale). ImGui will
//      NOT apply it; that's on us.
//
// SDL_GetWindowDisplayScale = pixel_density * content_scale, so using it
// directly double-scales on Retina. We read SDL_GetDisplayContentScale for the
// content factor only and re-rasterize the font atlas at framebuffer-pixel
// density so text stays sharp when the display changes.
void App::apply_display_scale() {
    const float content = ui_content_scale(window_);
    const float density = ui_pixel_density(window_);
    if (content == applied_content_scale_ &&
        density == applied_pixel_density_) return;
    applied_content_scale_ = content;
    applied_pixel_density_ = density;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = kBaseFontPixels * content;
    cfg.RasterizerDensity = density;
    cfg.FontDataOwnedByAtlas = false;  // bytes live in the embedded header
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kDroidSans),
        static_cast<int>(kDroidSans_len),
        cfg.SizePixels, &cfg);
    io.Fonts->Build();
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
    ImGui_ImplSDLRenderer3_CreateFontsTexture();

    ImGuiStyle& style = ImGui::GetStyle();
    style = *base_style_;
    style.ScaleAllSizes(content);
}

void App::draw_menu_bar(bool& running) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit", "Cmd+Q")) running = false;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("ImGui demo window", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("About zetta");
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void App::draw_dashboard() {
    ImGui::SeparatorText("Node");

    const NodeConfig& cfg = node_.config();
    ImGui::Text("Status:    %s", node_.status_line().c_str());
    ImGui::Text("Data dir:  %s", cfg.data_dir.c_str());
    ImGui::Text("Uptime:    %.1f s", node_.uptime_seconds());
    ImGui::Text("Ticks:     %llu",
                static_cast<unsigned long long>(node_.ticks()));

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Headed mode. The storage + query engine is not built yet; this view "
        "renders the live node heartbeat. Run with --headless for the same "
        "node without a UI.");
}

void App::run() {
    ImGuiIO& io = ImGui::GetIO();

    // Install our handlers last (after SDL_Init may have set its own) so
    // terminal Ctrl-C quits the headed app too, matching headless. The flag is
    // polled below; SDL_WaitEventTimeout's cap bounds the response to ~33ms.
    install_signal_handlers();

    bool running = true;
    while (running) {
        if (stop_requested()) break;
        node_.tick();

        // Sleep until the next input arrives or kIdleFrameMs elapses, so an
        // idle UI doesn't peg the CPU redrawing identical frames. Wait wakes
        // immediately on input, so interaction stays full-speed.
        constexpr int kIdleFrameMs = 33;  // ~30fps when idle
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, kIdleFrameMs)) {
            event.type = 0;  // timed out — skip the drain below
        }
        do {
            if (event.type == 0) break;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                       event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                apply_display_scale();
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

        draw_menu_bar(running);
        draw_dashboard();

        ImGui::End();

        if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);

        ImGui::Render();
        // SDL_WINDOW_HIGH_PIXEL_DENSITY gives a framebuffer larger than the
        // logical window. ImGui draws in logical coords and the SDLRenderer3
        // backend does NOT set the render scale for us, so without this the UI
        // paints into the top-left logical-sized quadrant only.
        SDL_SetRenderScale(renderer_,
                           io.DisplayFramebufferScale.x,
                           io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }
}

}  // namespace zetta::gui
