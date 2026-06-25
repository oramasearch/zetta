#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations keep SDL3 + Dear ImGui headers out of this file — only
// app.cpp (and no other translation unit) needs the full definitions.
struct SDL_Window;
struct SDL_Renderer;
struct ImGuiStyle;

namespace zetta {

class Node;
class Cluster;
namespace query {
class QueryService;
}

namespace gui {

// The headed run mode: an SDL3 + Dear ImGui window rendering a live view of a
// zetta::Node. Owns the window, renderer, and ImGui context for its lifetime —
// construct it, init(), run(), then let it destruct.
class App {
public:
    App(Node& node, Cluster& cluster);
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
    // A file to ingest, queued from a drop event or the native file dialog
    // (whose callback may fire on another thread — hence the mutex). Auto
    // routes by extension: .json/.jsonl import as rows, anything else stores as
    // a large object.
    enum class FileIntent { Auto, Records, LargeObject };
    struct IncomingFile {
        std::string path;
        FileIntent intent;
    };

    // Re-rasterize the font and rescale the style for the current display.
    // See app.cpp for the content-scale vs pixel-density rationale.
    void apply_display_scale();
    void draw_menu_bar(bool& running);
    void draw_dashboard();
    void draw_explorer();    // Projects | Databases | Tables | Rows | Large Objects columns
    void draw_projects();    // top-level AI projects (create/select/rename/delete)
    void draw_databases();
    void draw_tables();
    void draw_rows();
    void draw_large_objects();
    void draw_query();       // the SQL text area + results
    void draw_cluster();     // role, listen address, peers, join command

    void handle_incoming_files();  // drain + route the queue (main thread)
    void enqueue_file(std::string path, FileIntent intent);
    static void on_open_records(void* userdata, const char* const* files, int filter);
    static void on_open_large_object(void* userdata, const char* const* files, int filter);

    Node& node_;
    Cluster& cluster_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool initialized_ = false;
    bool show_demo_ = false;

    // Explorer selection (held by name; re-resolved each frame so a dropped
    // database/table can't leave a dangling handle) and input fields. ImGui's
    // imgui_stdlib InputText overloads bind directly to std::string.
    std::string selected_project_;
    std::string selected_db_;
    std::string selected_table_;
    std::string new_project_input_;
    std::string new_db_input_;
    std::string new_table_input_;
    std::string rename_project_input_;
    std::string rename_db_input_;
    std::string doc_input_{R"({"name": "example", "value": 1})"};
    // Per-column inline error messages.
    std::string project_error_;
    std::string db_error_;
    std::string table_error_;
    std::string row_error_;

    // Query panel: the SQL being composed, the dialect runner, and the last
    // result (rows pre-dumped to strings for display, plus status/error).
    std::unique_ptr<query::QueryService> query_;
    std::string query_input_{
        "-- Postgres-flavored SQL. Examples:\n"
        "CREATE DATABASE shop;\n"
        "CREATE TABLE shop.items (sku text, qty int);\n"
        "INSERT INTO shop.items (sku, qty) VALUES ('A1', 5), ('B2', 3);\n"
        "SELECT * FROM shop.items WHERE sku = 'A1';"};
    std::vector<std::string> query_result_columns_;
    std::vector<std::string> query_result_rows_;
    std::string query_status_;
    std::string query_error_;
    std::string query_time_;  // formatted elapsed time of the last run

    // File ingestion (drag-and-drop + native dialog) and its last status line.
    std::mutex incoming_mutex_;
    std::vector<IncomingFile> incoming_files_;
    std::string io_status_;

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
