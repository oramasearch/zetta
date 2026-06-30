#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/repl_console.hpp"

#include "zetta/access_key.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/bytes.hpp"
#include "zetta/capability.hpp"
#include "zetta/churn_solution.hpp"
#include "zetta/cloud_inspector.hpp"
#include "zetta/cloud_provider.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/database.hpp"
#include "zetta/deployment.hpp"
#include "zetta/deployment_target.hpp"
#include "zetta/duration.hpp"
#include "zetta/gate.hpp"
#include "zetta/import.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/large_object.hpp"
#include "zetta/model.hpp"
#include "zetta/n8n_import.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/project.hpp"
#include "zetta/project_graph.hpp"
#include "zetta/query/service.hpp"
#include "zetta/repl/governed_console.hpp"
#include "zetta/run.hpp"
#include "zetta/scaling_policy.hpp"
#include "zetta/scope.hpp"
#include "zetta/signals.hpp"
#include "zetta/solution_catalog.hpp"
#include "zetta/table.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/timestamp.hpp"
#include "zetta/validation.hpp"
#include "zetta/version.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zetta_font_sans.hpp"
#include "zetta_font_mono.hpp"

namespace zetta::gui {
namespace {

// Base logical font size; multiplied by the OS content scale and the user's
// View → Text size multiplier. The Geist Mono face is built a touch smaller
// (kMonoScale) for the design system's label / count / metric role.
constexpr float kBaseFontPixels = 17.0f;
constexpr float kMonoScale = 0.86f;

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

// Paint the ImGui style with the Orama design system (light). Flat surfaces,
// hairline borders, modest radii, ink text on canvas. Coral is reserved for the
// "you are here" accent (menu checkmarks, selected-graph node border, the one
// focal action); status uses the semantic green/amber/red, never coral.
void apply_orama_theme(ImGuiStyle& style) {
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kInkPrimary;
    colors[ImGuiCol_TextDisabled] = kInkQuiet;
    colors[ImGuiCol_WindowBg] = kCanvas;
    colors[ImGuiCol_ChildBg] = kWhite;          // child panes read as cards
    colors[ImGuiCol_PopupBg] = kWhite;
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = kWhite;          // inputs: white + hairline border
    colors[ImGuiCol_FrameBgHovered] = wash(0.04f);
    colors[ImGuiCol_FrameBgActive] = wash(0.06f);
    colors[ImGuiCol_TitleBg] = kSidebar;
    colors[ImGuiCol_TitleBgActive] = kSidebar;
    colors[ImGuiCol_MenuBarBg] = kSidebar;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = wash(0.16f);
    colors[ImGuiCol_ScrollbarGrabHovered] = wash(0.24f);
    colors[ImGuiCol_ScrollbarGrabActive] = wash(0.32f);
    colors[ImGuiCol_CheckMark] = kCoral;        // "you are here" (active view)
    colors[ImGuiCol_SliderGrab] = kInkMuted;
    colors[ImGuiCol_SliderGrabActive] = kCoral;
    // Ghost buttons: a faint wash + hairline border, ink text.
    colors[ImGuiCol_Button] = wash(0.03f);
    colors[ImGuiCol_ButtonHovered] = wash(0.06f);
    colors[ImGuiCol_ButtonActive] = wash(0.10f);
    // Selection (Selectable / MenuItem / Header): a soft wash, never a coral or
    // charcoal fill — coral stays reserved for the accent.
    colors[ImGuiCol_Header] = wash(0.05f);
    colors[ImGuiCol_HeaderHovered] = wash(0.04f);
    colors[ImGuiCol_HeaderActive] = wash(0.06f);
    colors[ImGuiCol_Separator] = kBorder;
    colors[ImGuiCol_SeparatorHovered] = kInkQuiet;
    colors[ImGuiCol_SeparatorActive] = kInkMuted;
    colors[ImGuiCol_ResizeGrip] = wash(0.06f);
    colors[ImGuiCol_ResizeGripHovered] = wash(0.12f);
    colors[ImGuiCol_ResizeGripActive] = wash(0.18f);
    colors[ImGuiCol_PlotHistogram] = kInkSecondary;  // metric / confidence bars
    colors[ImGuiCol_PlotHistogramHovered] = kInkPrimary;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.886f, 0.329f, 0.353f, 0.18f);
    colors[ImGuiCol_NavHighlight] = kCoral;

    // Flat + measured: hairline borders, modest radii (design radius-8), no
    // decorative rounding on the full-window root.
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
}

}  // namespace

// Ambient Geist Mono face for the file-local render_* helpers — declared
// extern in app_internal.hpp, defined here (and assigned once per frame at the
// top of render_frame()). Null until the first font-atlas build.
ImFont* g_mono_font = nullptr;

App::App(Node& node, Cluster& cluster)
    : node_(node),
      cluster_(cluster),
      query_(std::make_unique<query::QueryService>(node, &cluster)),
      console_model_(std::make_unique<repl::ConsoleModel>()),
      console_(std::make_unique<ReplConsole>()),
      governed_console_(
          std::make_unique<repl::GovernedConsole>(node, *query_)),
      gate_graph_input_(kExampleGraphJson) {
    // Seed the console with a runnable simulated-Python example — the
    // UC-1-at-scale pipeline (add nodes, load the saas parquet, train on
    // `outcome`, explain, rank) — so the passing path is one click away: it is
    // already on the Python frontend, just press Run (cleared on first Run, like
    // any REPL prompt). The columns are spelled out in full (no `...` ellipsis,
    // which is not valid sim-Python). In the console the steps execute through the
    // default step executor (a governed, gated, audited Run); the real distributed
    // near-data run is the cluster.uc1_at_scale test-suite case.
    console_model_->set_frontend(repl::Frontend::SimPython);
    console_model_->input() =
        "nodes = cluster.add_nodes(2, provider='azure')\n"
        "conversations = load_dataset('saas-sales-conversations', columns=['messages', "
        "'avg_response_seconds', 'sentiment', 'questions_asked', 'outcome'])\n"
        "model = train(conversations, target='outcome')\n"
        "drivers = explain(model)\n"
        "at_risk = rank(model)";
}

App::~App() {
    // Wait out any in-flight test run (the worker borrows `this`).
    if (test_worker_ && test_worker_->joinable()) test_worker_->join();
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

    const std::string title = std::string{"Orama Zetta "} + kVersion;
    window_ = SDL_CreateWindow(
        title.c_str(), win_w, win_h,
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
    ImGui::StyleColorsLight();
    apply_orama_theme(ImGui::GetStyle());

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
    if (content == applied_content_scale_ && density == applied_pixel_density_ &&
        ui_scale_ == applied_ui_scale_) {
        return;
    }
    applied_content_scale_ = content;
    applied_pixel_density_ = density;
    applied_ui_scale_ = ui_scale_;
    const float scale = content * ui_scale_;  // OS content scale × user text size

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // Glyph coverage beyond ImGui's default Latin-1: General Punctuation (so a
    // stray en/em-dash, bullet, or ellipsis renders instead of a "?" tofu) plus
    // arrows. Geist covers these. Static so the pointer stays valid until Build.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
        0x2010, 0x205E,  // General Punctuation: – — ' ' " " • …
        0x2190, 0x2199,  // Arrows
        0,
    };

    // The embedded bytes are static (constexpr arrays), so FontDataOwnedByAtlas
    // = false is safe — ImGui reads them at Build() and never frees them.
    const auto add_font = [&](const unsigned char* data, unsigned length,
                              float pixels) -> ImFont* {
        ImFontConfig cfg;
        cfg.SizePixels = pixels;
        cfg.RasterizerDensity = density;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data),
                                              static_cast<int>(length), pixels,
                                              &cfg, kRanges);
    };

    // Geist (sans) is the default UI face; Geist Mono (a touch smaller) is the
    // design system's label / count / metric face, captured for PushFont.
    add_font(kGeistSans, kGeistSans_len, kBaseFontPixels * scale);
    mono_font_ = add_font(kGeistMono, kGeistMono_len,
                          kBaseFontPixels * kMonoScale * scale);

    io.Fonts->Build();
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
    ImGui_ImplSDLRenderer3_CreateFontsTexture();

    ImGuiStyle& style = ImGui::GetStyle();
    style = *base_style_;
    style.ScaleAllSizes(scale);
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMenuBar()) return;

    // A menu entry that selects a view: a checkmark marks the live one, and
    // choosing it makes that view the content area's sole panel.
    const auto view_item = [&](const char* label, View view) {
        if (ImGui::MenuItem(label, nullptr, active_view_ == view)) {
            active_view_ = view;
        }
    };

    if (ImGui::BeginMenu("File")) {
        view_item("Home", View::Home);
        if (ImGui::MenuItem("New project…")) {
            new_project_modal_requested_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Cmd+Q")) running_ = false;
        ImGui::EndMenu();
    }
    // The views, grouped along the product flow: your data → architect the
    // pipeline → operate it → govern it.
    if (ImGui::BeginMenu("Data")) {
        view_item("Explorer", View::Explorer);
        view_item("Datasets", View::Datasets);
        view_item("SQL query", View::Query);
        view_item("Console", View::Console);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Pipeline")) {
        view_item("Graph workspace", View::Graph);
        view_item("Correctness gate", View::Gate);
        view_item("Deploy", View::Deploy);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Operations")) {
        view_item("Runs", View::Runs);
        view_item("Models", View::Models);
        view_item("Compute jobs", View::Jobs);
        view_item("Cluster", View::Cluster);
        view_item("Cloud inspector", View::Cloud);
        view_item("Test suite", View::Tests);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Governance")) {
        view_item("Admin & audit", View::Admin);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Text size")) {
            // Each option is a UI scale on top of the OS content scale; a change
            // requests a font-atlas rebuild before the next frame.
            const struct {
                const char* label;
                float scale;
            } sizes[] = {
                {"Small", 0.85f},  {"Default", 1.0f}, {"Large", 1.15f},
                {"Larger", 1.30f}, {"Largest", 1.5f},
            };
            for (const auto& size : sizes) {
                const bool selected = ui_scale_ == size.scale;
                if (ImGui::MenuItem(size.label, nullptr, selected) &&
                    ui_scale_ != size.scale) {
                    ui_scale_ = size.scale;
                    font_rebuild_requested_ = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("ImGui demo window", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Orama Zetta")) about_requested_ = true;
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

// The content-area switcher: render whichever view the menu bar selected. Each
// draw_* leads with a SeparatorText title, so the active panel is self-labelled.
void App::draw_active_view() {
    switch (active_view_) {
        case View::Home:     draw_home();            break;
        case View::Explorer: draw_explorer();        break;
        case View::Datasets: draw_datasets();        break;
        case View::Query:    draw_query();           break;
        case View::Console:  draw_console();          break;
        case View::Graph:    draw_graph_workspace(); break;
        case View::Gate:     draw_gate();            break;
        case View::Deploy:   draw_deploy();          break;
        case View::Runs:     draw_runs();            break;
        case View::Models:   draw_models();          break;
        case View::Jobs:     draw_jobs();            break;
        case View::Cluster:  draw_cluster();         break;
        case View::Cloud:    draw_cloud();           break;
        case View::Admin:    draw_admin();           break;
        case View::Tests:    draw_test_suite();      break;
    }
}

void App::enqueue_file(std::string path, FileIntent intent) {
    const std::lock_guard<std::mutex> lock(incoming_mutex_);
    incoming_files_.push_back({std::move(path), intent});
}

void App::on_open_records(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::Records);
    }
}

void App::on_open_large_object(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::LargeObject);
    }
}

void App::on_open_n8n(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::N8nArtifact);
    }
}

void App::on_open_model_weights(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::ModelWeights);
    }
}

void App::handle_incoming_files() {
    std::vector<IncomingFile> files;
    {
        const std::lock_guard<std::mutex> lock(incoming_mutex_);
        if (incoming_files_.empty()) return;
        files.swap(incoming_files_);
    }
    for (const IncomingFile& f : files) {
        const std::filesystem::path path = f.path;
        FileIntent intent = f.intent;
        // An n8n artifact is staged for the New Project dialog, not ingested:
        // validate it and remember the path so Create can import it into the
        // new project (see stage_n8n_artifact / draw_new_project_modal).
        if (intent == FileIntent::N8nArtifact) {
            stage_n8n_artifact(f.path);
            continue;
        }
        // Model weights are staged for the Models view's "Add model" form — the
        // bytes are opaque (no validation), attached on Register via set_weights.
        if (intent == FileIntent::ModelWeights) {
            add_model_weights_path_ = f.path;
            add_model_weights_name_ = path.filename().string();
            io_status_ = "Staged weights: " + add_model_weights_name_;
            continue;
        }
        if (intent == FileIntent::Auto) {
            std::string ext = path.extension().string();
            for (char& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            intent = (ext == ".json" || ext == ".jsonl") ? FileIntent::Records
                                                         : FileIntent::LargeObject;
        }
        if (intent == FileIntent::Records) {
            auto db = node_.find_database(selected_db_);
            auto table = db ? db->find_table(selected_table_) : nullptr;
            if (!table) {
                io_status_ = "Import: select a table first (" +
                             path.filename().string() + ")";
                continue;
            }
            const ImportResult r = import_records(*table, path);
            io_status_ = r.ok() ? "Imported " + std::to_string(r.added) +
                                      " record(s) into " + table->name()
                                : "Import failed: " + r.error;
        } else {
            auto db = node_.find_database(selected_db_);
            if (!db) {
                io_status_ = "Store: select a database first (" +
                             path.filename().string() + ")";
                continue;
            }
            const auto lo = db->large_objects().import_file(path);
            io_status_ = lo ? "Stored " + lo->name + " (" +
                                  format_bytes(lo->size) + ") as a large object"
                            : "Store failed: " + path.string();
        }
    }
}

void App::stage_n8n_artifact(const std::string& path) {
    const std::filesystem::path file = path;
    new_project_artifact_path_ = path;
    new_project_artifact_name_ = file.filename().string();

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        new_project_artifact_valid_ = false;
        new_project_artifact_status_ = "cannot open file";
        return;
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

    // Validate against the n8n workflow schema at the staging boundary so the
    // user gets fail-fast feedback here; import_n8n_workflow_file re-validates at
    // the real storage boundary on Create. (No new format is introduced — this
    // reuses the existing n8n_workflow_schema.)
    std::string error;
    if (!validate_n8n_workflow(content, &error)) {
        new_project_artifact_valid_ = false;
        new_project_artifact_status_ = error;
        return;
    }
    new_project_artifact_valid_ = true;
    const auto document =
        nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    const std::size_t node_count =
        (!document.is_discarded() && document.contains("nodes") &&
         document["nodes"].is_array())
            ? document["nodes"].size()
            : 0;
    new_project_artifact_status_ =
        "valid n8n workflow, " + std::to_string(node_count) + " node(s)";
}

void App::reset_new_project_form() {
    new_project_name_.clear();
    new_project_intent_.clear();
    new_project_artifact_path_.clear();
    new_project_artifact_name_.clear();
    new_project_artifact_status_.clear();
    new_project_artifact_valid_ = false;
    new_project_error_.clear();
}

void App::render_frame() {
    // Re-entrancy guard: the resize event watch can fire while we're already
    // rendering (it runs synchronously inside SDL's pump). One frame at a time.
    if (rendering_) return;
    rendering_ = true;
    g_mono_font = mono_font_;  // ambient mono for the file-local render_* helpers

    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;
    ImGui::Begin("##zetta_root", nullptr, kRootFlags);

    // Menu bar (selects the view) + a persistent node-status strip, then the
    // single active view fills the rest of the window. One focused panel at a
    // time instead of every panel stacked in one long scroll.
    draw_menu_bar();
    draw_dashboard();
    ImGui::Separator();
    draw_active_view();

    // The New Project dialog (SCR-2) and the About dialog are modals over the
    // root window; they must be issued every frame so ImGui keeps them alive
    // while open.
    draw_new_project_modal();
    draw_about_modal();

    ImGui::End();

    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);

    ImGui::Render();
    // SDL_WINDOW_HIGH_PIXEL_DENSITY gives a framebuffer larger than the logical
    // window. ImGui draws in logical coords and the SDLRenderer3 backend does
    // NOT set the render scale for us, so without this the UI paints into the
    // top-left logical-sized quadrant only.
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer_, 250, 250, 248, 255);  // Orama canvas
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);

    rendering_ = false;
}

bool App::on_sdl_event(void* userdata, SDL_Event* event) {
    // Render synchronously while the window is being live-resized (or exposed),
    // so the UI tracks the drag instead of the OS stretching the last frame.
    // macOS runs a modal runloop during resize that starves the normal loop.
    App* app = static_cast<App*>(userdata);
    if (event->type == SDL_EVENT_WINDOW_RESIZED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event->type == SDL_EVENT_WINDOW_EXPOSED) {
        app->render_frame();
    }
    return true;  // watchers don't filter; keep the event
}

void App::run() {
    // Install our handlers last (after SDL_Init may have set its own) so
    // terminal Ctrl-C quits the headed app too, matching headless. The flag is
    // polled below; SDL_WaitEventTimeout's cap bounds the response to ~33ms.
    install_signal_handlers();

    // Re-render during live resize (see on_sdl_event).
    SDL_AddEventWatch(&App::on_sdl_event, this);

    while (running_) {
        if (stop_requested()) break;
        node_.tick();

        // A user text-size change (View → Text size) rebuilds the font atlas
        // here, between frames — never mid-frame, while the atlas is in use.
        if (font_rebuild_requested_) {
            apply_display_scale();
            font_rebuild_requested_ = false;
        }

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
                running_ = false;
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                       event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                apply_display_scale();
            } else if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    // While the New Project dialog is up, a dropped file is its
                    // n8n artifact (the rest of the UI is blocked by the modal);
                    // otherwise it routes by extension as before.
                    enqueue_file(event.drop.data,
                                 new_project_modal_open_
                                     ? FileIntent::N8nArtifact
                                     : FileIntent::Auto);
                }
            }
        } while (SDL_PollEvent(&event));

        // Ingest any files dropped or picked since the last frame (the dialog
        // callback runs on another thread; this drains on the main thread).
        handle_incoming_files();

        render_frame();
    }

    SDL_RemoveEventWatch(&App::on_sdl_event, this);
}

}  // namespace zetta::gui
