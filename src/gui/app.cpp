#include "gui/app.hpp"

#include "zetta/bytes.hpp"
#include "zetta/cluster.hpp"
#include "zetta/database.hpp"
#include "zetta/duration.hpp"
#include "zetta/import.hpp"
#include "zetta/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/signals.hpp"
#include "zetta/table.hpp"
#include "zetta/version.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "zetta_font_embedded.hpp"

namespace zetta::gui {
namespace {

// Base logical font size; multiplied by the OS content scale per display.
constexpr float kBaseFontPixels = 15.0f;

// Inline-error text colour (soft red).
constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};

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

App::App(Node& node, Cluster& cluster)
    : node_(node),
      cluster_(cluster),
      query_(std::make_unique<query::QueryService>(node, &cluster)) {}

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

    const std::string title = std::string{"zetta "} + kVersion;
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
    ImGui::Text("Node: %s", node_.status_line().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|  data dir: %s", node_.config().data_dir.c_str());
    if (!io_status_.empty()) {
        ImGui::TextDisabled("%s", io_status_.c_str());
    }
    ImGui::Spacing();
}

void App::draw_explorer() {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float col_w =
        (ImGui::GetContentRegionAvail().x - 3.0f * spacing) / 4.0f;

    ImGui::BeginChild("##databases", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_databases();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##tables", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_tables();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##rows", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_rows();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##largeobjects", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_large_objects();
    ImGui::EndChild();
}

void App::draw_databases() {
    ImGui::SeparatorText("Databases");

    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submit = ImGui::InputTextWithHint(
        "##newdb", "new database name", &new_db_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Create database") || submit) {
        if (new_db_input_.empty()) {
            db_error_ = "name required";
        } else if (!node_.create_database(new_db_input_)) {
            db_error_ = "database already exists";
        } else {
            selected_db_ = new_db_input_;
            selected_table_.clear();
            new_db_input_.clear();
            db_error_.clear();
        }
    }
    if (!db_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", db_error_.c_str());
    }

    ImGui::Separator();

    for (const std::string& name : node_.database_names()) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            node_.drop_database(name);
            if (selected_db_ == name) {
                selected_db_.clear();
                selected_table_.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected_db_ == name)) {
            selected_db_ = name;
            selected_table_.clear();
        }
        ImGui::PopID();
    }

    // The virtual, read-only `system` database (computed system tables like
    // system.zetta_nodes). It isn't a real on-disk database, so it's listed
    // apart and can't be renamed/dropped.
    ImGui::Separator();
    if (ImGui::Selectable("system  (virtual)", selected_db_ == "system")) {
        selected_db_ = "system";
        selected_table_.clear();
    }

    // Rename (the "U" in CRUD) the selected database.
    if (const auto db = node_.find_database(selected_db_)) {
        ImGui::Separator();
        ImGui::TextDisabled("Rename '%s' to:", db->name().c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool rename = ImGui::InputTextWithHint(
            "##rendb", "new name", &rename_db_input_,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Rename") || rename) {
            if (rename_db_input_.empty()) {
                db_error_ = "name required";
            } else if (!node_.rename_database(selected_db_, rename_db_input_)) {
                db_error_ = "rename failed (name taken?)";
            } else {
                selected_db_ = rename_db_input_;
                rename_db_input_.clear();
                db_error_.clear();
            }
        }
    }
}

void App::draw_tables() {
    ImGui::SeparatorText("Tables");

    if (selected_db_ == "system") {
        ImGui::TextDisabled("in 'system' (read-only)");
        ImGui::Separator();
        for (const char* table_name : {"zetta_nodes"}) {
            if (ImGui::Selectable(table_name, selected_table_ == table_name)) {
                selected_table_ = table_name;
            }
        }
        return;
    }

    const auto db = node_.find_database(selected_db_);
    if (!db) {
        ImGui::TextDisabled("Select a database.");
        return;
    }

    ImGui::TextDisabled("in '%s'", db->name().c_str());

    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submit = ImGui::InputTextWithHint(
        "##newtable", "new table name", &new_table_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Create table") || submit) {
        if (new_table_input_.empty()) {
            table_error_ = "name required";
        } else if (!db->create_table(new_table_input_)) {
            table_error_ = "table already exists";
        } else {
            selected_table_ = new_table_input_;
            new_table_input_.clear();
            table_error_.clear();
        }
    }
    if (!table_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", table_error_.c_str());
    }

    ImGui::Separator();

    for (const std::string& name : db->table_names()) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            db->drop_table(name);
            if (selected_table_ == name) selected_table_.clear();
        }
        ImGui::SameLine();
        const auto table = db->find_table(name);
        const std::size_t rows = table ? table->row_count() : 0;
        const std::string label = name + "  (" + std::to_string(rows) + ")";
        if (ImGui::Selectable(label.c_str(), selected_table_ == name)) {
            selected_table_ = name;
        }
        ImGui::PopID();
    }
}

void App::draw_rows() {
    ImGui::SeparatorText("Rows");

    // System tables are computed + read-only: query them and show the result.
    if (selected_db_ == "system") {
        if (selected_table_.empty()) {
            ImGui::TextDisabled("Select a system table.");
            return;
        }
        const query::QueryService::Outcome outcome = query_->run(
            "SELECT * FROM system." + selected_table_ + ";", selected_db_);
        if (!outcome.ok()) {
            ImGui::TextColored(kErrorColor, "%s", outcome.error.c_str());
            return;
        }
        ImGui::TextDisabled("system.%s  (%zu rows)", selected_table_.c_str(),
                            outcome.rows.size());
        for (std::size_t i = 0; i < outcome.rows.size(); ++i) {
            ImGui::TextWrapped("%zu  %s", i, outcome.rows[i].dump().c_str());
        }
        return;
    }

    const auto db = node_.find_database(selected_db_);
    if (!db) {
        ImGui::TextDisabled("Select a database.");
        return;
    }
    const auto table = db->find_table(selected_table_);
    if (!table) {
        ImGui::TextDisabled("Select or create a table.");
        return;
    }

    ImGui::TextDisabled("%s.%s  (%zu rows)", db->name().c_str(),
                        table->name().c_str(), table->row_count());

    if (ImGui::Button("Import JSON/JSONL...")) {
        static const SDL_DialogFileFilter filters[] = {
            {"JSON / JSON Lines", "json;jsonl"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(&App::on_open_records, this, window_, filters, 2,
                               nullptr, false);
    }

    ImGui::TextUnformatted("Add a JSON object:");
    ImGui::InputTextMultiline("##doc", &doc_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5));
    if (ImGui::Button("Add object")) {
        // Parse without exceptions — a malformed doc yields a discarded value
        // we report inline rather than throwing across the UI loop.
        auto doc = nlohmann::json::parse(doc_input_, nullptr,
                                         /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            row_error_ = "invalid JSON — not added";
        } else {
            table->add_row(std::move(doc));
            row_error_.clear();
        }
    }
    if (!row_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "%s", row_error_.c_str());
    }

    ImGui::Spacing();
    const StorageEngine& store = table->store();
    if (store.size() == 0) {
        ImGui::TextDisabled("(empty)");
    }
    // Cap how many rows we render — a persisted table can hold far more than is
    // useful to draw every frame.
    constexpr std::size_t kMaxShown = 500;
    const std::size_t shown = std::min(store.size(), kMaxShown);
    for (std::size_t i = 0; i < shown; ++i) {
        ImGui::TextWrapped("%zu  %s", i, store.at(i).dump().c_str());
    }
    if (store.size() > shown) {
        ImGui::TextDisabled("... and %zu more", store.size() - shown);
    }
}

void App::draw_query() {
    if (!ImGui::CollapsingHeader("Query (postgres)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::TextDisabled(
        "Unqualified names use the selected database (%s); use db.table to "
        "target another.",
        selected_db_.empty() ? "none" : selected_db_.c_str());

    ImGui::InputTextMultiline("##query", &query_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6));

    if (ImGui::Button("Run")) {
        const query::QueryService::Outcome outcome =
            query_->run(query_input_, selected_db_);
        query_status_.clear();
        query_error_.clear();
        query_result_columns_.clear();
        query_result_rows_.clear();
        query_time_ = format_duration(outcome.elapsed);
        if (!outcome.ok()) {
            query_error_ = outcome.error;
        } else {
            for (std::size_t i = 0; i < outcome.messages.size(); ++i) {
                if (i) query_status_ += "   ";
                query_status_ += outcome.messages[i];
            }
            query_result_columns_ = outcome.columns;
            for (const nlohmann::json& row : outcome.rows) {
                query_result_rows_.push_back(row.dump());
            }
        }
    }

    if (!query_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", query_error_.c_str());
    } else if (!query_status_.empty()) {
        ImGui::TextDisabled("%s", query_status_.c_str());
    }
    if (!query_time_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", query_time_.c_str());
    }

    if (ImGui::BeginChild("##query_results", ImVec2(0, 160),
                          ImGuiChildFlags_Borders)) {
        if (!query_result_columns_.empty()) {
            std::string cols;
            for (std::size_t i = 0; i < query_result_columns_.size(); ++i) {
                if (i) cols += ", ";
                cols += query_result_columns_[i];
            }
            ImGui::TextDisabled("columns: %s", cols.c_str());
        }
        constexpr std::size_t kMaxShown = 500;
        const std::size_t shown = std::min(query_result_rows_.size(), kMaxShown);
        for (std::size_t i = 0; i < shown; ++i) {
            ImGui::TextWrapped("%s", query_result_rows_[i].c_str());
        }
        if (query_result_rows_.size() > shown) {
            ImGui::TextDisabled("... and %zu more",
                                query_result_rows_.size() - shown);
        }
    }
    ImGui::EndChild();
}

void App::draw_large_objects() {
    ImGui::SeparatorText("Large Objects");
    const auto db = node_.find_database(selected_db_);
    if (!db) {
        ImGui::TextDisabled("Select a database.");
        return;
    }
    if (ImGui::Button("Store file...")) {
        SDL_ShowOpenFileDialog(&App::on_open_large_object, this, window_,
                               nullptr, 0, nullptr, false);
    }
    ImGui::TextDisabled("or drop any file on the window");
    ImGui::Separator();

    const std::vector<LargeObject> objects = db->large_objects().list();
    if (objects.empty()) ImGui::TextDisabled("(none)");
    for (const LargeObject& lo : objects) {
        ImGui::PushID(lo.id.c_str());
        if (ImGui::SmallButton("x")) db->large_objects().unlink(lo.id);
        ImGui::SameLine();
        ImGui::TextWrapped("%s  (%s)", lo.name.c_str(),
                           format_bytes(lo.size).c_str());
        // The metadata replicates immediately; the body transfers separately,
        // so a freshly-replicated object may not have its bytes here yet.
        if (!lo.body_present) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "(syncing body)");
        }
        ImGui::PopID();
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

void App::draw_cluster() {
    if (!ImGui::CollapsingHeader("Cluster", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::Text("Node: %s", cluster_.node_id().c_str());
    for (const std::string& address : cluster_.listen_addresses()) {
        ImGui::Text("Listening on: %s", address.c_str());
    }
    ImGui::TextDisabled(
        "All nodes are equal — leader election + failover are planned.");

    ImGui::TextUnformatted("Add another node with:");
    std::string command = cluster_.peer_command();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##peer_command", &command, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Copy command")) {
        ImGui::SetClipboardText(command.c_str());
    }

    ImGui::SeparatorText("Nodes & replication");
    const std::vector<NodeStatus> nodes = cluster_.nodes();
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("nodes", 6, table_flags)) {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Role");
        ImGui::TableSetupColumn("Entries");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Replication");
        ImGui::TableHeadersRow();
        for (const NodeStatus& node : nodes) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // First UUID segment — enough to tell nodes apart at a glance.
            const std::string id = node.node_id.substr(0, node.node_id.find('-'));
            ImGui::TextUnformatted(node.is_self ? (id + " (self)").c_str()
                                                : id.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(node.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(to_string(node.role).c_str());
            ImGui::TableSetColumnIndex(3);
            if (node.replication.known) {
                ImGui::Text("%zu", node.replication.entries);
            } else {
                ImGui::TextDisabled("—");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(node.replication.known
                                       ? node.replication.version.c_str()
                                       : "—");
            ImGui::TableSetColumnIndex(5);
            if (node.is_self) {
                ImGui::TextDisabled("—");
            } else if (!node.replication.known) {
                ImGui::TextDisabled("no status yet");
            } else if (node.replication.in_sync) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f),
                                   "in sync (%.1fs)",
                                   node.replication.seconds_since_sync);
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                                   "syncing (%.1fs)",
                                   node.replication.seconds_since_sync);
            }
        }
        ImGui::EndTable();
    }
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
            } else if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    enqueue_file(event.drop.data, FileIntent::Auto);
                }
            }
        } while (SDL_PollEvent(&event));

        // Ingest any files dropped or picked since the last frame (the dialog
        // callback runs on another thread; this drains on the main thread).
        handle_incoming_files();

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
        draw_query();
        draw_cluster();
        ImGui::Separator();
        draw_explorer();

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
