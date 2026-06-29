#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/repl_console.hpp"

#include "zetta/bytes.hpp"
#include "zetta/database.hpp"
#include "zetta/duration.hpp"
#include "zetta/import.hpp"
#include "zetta/large_object.hpp"
#include "zetta/n8n_import.hpp"
#include "zetta/node.hpp"
#include "zetta/project.hpp"
#include "zetta/query/service.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/table.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace zetta::gui {

void App::draw_explorer() {
    ImGui::SeparatorText("Explorer");
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float col_w =
        (ImGui::GetContentRegionAvail().x - 4.0f * spacing) / 5.0f;

    ImGui::BeginChild("##projects", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_projects();
    ImGui::EndChild();

    ImGui::SameLine();
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

void App::draw_projects() {
    ImGui::SeparatorText("Projects");

    // The full create flow — name + NL intent + n8n artifact import — lives in
    // the New Project modal (SCR-2); this button is its front door. Rename /
    // select / delete for existing projects stay inline below.
    if (ImGui::Button("New project…")) {
        new_project_modal_requested_ = true;
    }
    if (!project_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", project_error_.c_str());
    }

    ImGui::Separator();

    for (const std::string& name : node_.project_names()) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            node_.drop_project(name);
            if (selected_project_ == name) selected_project_.clear();
        }
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected_project_ == name)) {
            selected_project_ = name;
        }
        ImGui::PopID();
    }

    // Details + rename for the selected project. Re-resolved by name each frame
    // (like the database selection) so a dropped/renamed project can't leave a
    // dangling handle.
    const auto project = node_.find_project(selected_project_);
    if (!project) return;

    ImGui::Separator();
    // First UUID segment — enough to identify the project at a glance.
    const std::string short_id =
        project->id().substr(0, project->id().find('-'));
    mono_line(kInkQuiet, "id:       ", short_id);
    mono_line(kInkQuiet, "owner:    ",
              project->owner().empty() ? "-" : project->owner());
    mono_line(kInkQuiet, "created:  ", project->created());
    if (project->updated() != project->created()) {
        mono_line(kInkQuiet, "updated:  ", project->updated());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Rename '%s' to:", project->name().c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool rename = ImGui::InputTextWithHint(
        "##renproject", "new name", &rename_project_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Rename") || rename) {
        if (rename_project_input_.empty()) {
            project_error_ = "name required";
        } else if (!node_.rename_project(selected_project_,
                                         rename_project_input_)) {
            project_error_ = "rename failed (name taken?)";
        } else {
            selected_project_ = rename_project_input_;
            rename_project_input_.clear();
            project_error_.clear();
        }
    }
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
        if (mono_font_) ImGui::PushFont(mono_font_);
        for (std::size_t i = 0; i < outcome.rows.size(); ++i) {
            ImGui::TextWrapped("%zu  %s", i, outcome.rows[i].dump().c_str());
        }
        if (mono_font_) ImGui::PopFont();
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
            row_error_ = "invalid JSON, not added";
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
    if (mono_font_) ImGui::PushFont(mono_font_);
    for (std::size_t i = 0; i < shown; ++i) {
        ImGui::TextWrapped("%zu  %s", i, store.at(i).dump().c_str());
    }
    if (mono_font_) ImGui::PopFont();
    if (store.size() > shown) {
        ImGui::TextDisabled("... and %zu more", store.size() - shown);
    }
}

void App::draw_query() {
    ImGui::SeparatorText("SQL query (postgres)");
    ImGui::TextDisabled(
        "Unqualified names use the selected database (%s); use db.table to "
        "target another.",
        selected_db_.empty() ? "none" : selected_db_.c_str());

    // The SQL editor is code: render it in Geist Mono.
    if (mono_font_) ImGui::PushFont(mono_font_);
    ImGui::InputTextMultiline("##query", &query_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6));
    if (mono_font_) ImGui::PopFont();

    if (primary_button("Run")) {
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
        // Results are data: mono.
        if (mono_font_) ImGui::PushFont(mono_font_);
        if (!query_result_columns_.empty()) {
            std::string cols;
            for (std::size_t i = 0; i < query_result_columns_.size(); ++i) {
                if (i) cols += ", ";
                cols += query_result_columns_[i];
            }
            ImGui::TextColored(kInkMuted, "columns: %s", cols.c_str());
        }
        constexpr std::size_t kMaxShown = 500;
        const std::size_t shown = std::min(query_result_rows_.size(), kMaxShown);
        for (std::size_t i = 0; i < shown; ++i) {
            ImGui::TextWrapped("%s", query_result_rows_[i].c_str());
        }
        if (query_result_rows_.size() > shown) {
            ImGui::TextColored(kInkQuiet, "... and %zu more",
                               query_result_rows_.size() - shown);
        }
        if (mono_font_) ImGui::PopFont();
    }
    ImGui::EndChild();
}

namespace {

// Stringify one JSON cell for the result grid: a string verbatim (no quotes),
// null as blank, anything else as its compact JSON dump.
std::string json_cell(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "";
    return value.dump();
}

// Run `source` as SQL and map the query service's Outcome onto the frontend-
// agnostic repl::Result the console renders: status messages + a column-aligned
// row grid on success, the first parse/execute error otherwise.
repl::Result sql_result(query::QueryService& query, const std::string& source,
                        const std::string& database) {
    const query::QueryService::Outcome outcome = query.run(source, database);
    repl::Result result;
    result.elapsed = format_duration(outcome.elapsed);
    if (!outcome.ok()) {
        result.error = outcome.error;
        return result;
    }
    result.messages = outcome.messages;
    result.columns = outcome.columns;
    result.rows.reserve(outcome.rows.size());
    for (const nlohmann::json& row : outcome.rows) {
        std::vector<std::string> cells;
        cells.reserve(result.columns.size());
        for (const std::string& column : result.columns) {
            if (row.is_object() && row.contains(column)) {
                cells.push_back(json_cell(row.at(column)));
            } else if (result.columns.size() == 1 && !row.is_object()) {
                cells.push_back(json_cell(row));  // a single scalar projection
            } else {
                cells.push_back("");
            }
        }
        result.rows.push_back(std::move(cells));
    }
    return result;
}

// The simulated-Python frontend is selectable + emits a run event, but the
// frontend→IR lowering layer (ADR-33 / ARC-15) isn't in this branch yet, so a
// run reports an honest "not wired" result inline rather than pretending. Real
// execution is the governed Console/REPL screen's job (#525); the lowering is
// #522.
repl::Result sim_python_seam() {
    return repl::Result::failure(
        "Simulated-Python isn't wired to the engine yet. Statements will lower "
        "to the ExecutionGraph IR once the frontend layer lands (see #525); use "
        "the SQL frontend for now.");
}

}  // namespace

// The Console (REPL) view (CMP-6 / #524): host the reusable ReplConsole
// component over this node. The App only supplies the run handler (the "run
// event" sink) — SQL through the query service, simulated-Python through the
// honest seam — and the component owns the rest (selector, editor, transcript,
// grid). Held state lives in console_model_, so a future CLI renders the same
// session.
void App::draw_console() {
    ImGui::SeparatorText("Console (REPL)");
    ImGui::TextDisabled(
        "Run SQL or simulated-Python against this node. Unqualified SQL names "
        "use the selected database (%s); use db.table to target another.",
        selected_db_.empty() ? "none" : selected_db_.c_str());

    const repl::RunHandler run =
        [this](repl::Frontend frontend,
               const std::string& source) -> repl::Result {
        switch (frontend) {
            case repl::Frontend::Sql:
                return sql_result(*query_, source, selected_db_);
            case repl::Frontend::SimPython:
                return sim_python_seam();
        }
        return repl::Result::failure("unknown frontend");
    };

    console_->draw(*console_model_, run, mono_font_);
}

void App::draw_large_objects() {
    ImGui::SeparatorText("Large objects");
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
            ImGui::TextColored(kWarnColor, "(syncing body)");
        }
        ImGui::PopID();
    }
}

void App::draw_new_project_modal() {
    if (new_project_modal_requested_) {
        ImGui::OpenPopup("New project");
        new_project_modal_requested_ = false;
        new_project_error_.clear();
    }

    // Centre the dialog on first appearance; let it auto-size to its content.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32.0f, 0.0f),
                             ImGuiCond_Appearing);

    bool stay_open = true;  // the window's close (x) button toggles this
    if (!ImGui::BeginPopupModal("New project", &stay_open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        new_project_modal_open_ = false;
        return;
    }
    // Mirror the open state so run() routes file drops to the n8n staging area
    // while this dialog is up (see the drop handling in run()).
    new_project_modal_open_ = true;

    ImGui::TextDisabled(
        "Name the project, describe what it should do, and optionally import an "
        "n8n workflow as a starting artifact.");
    ImGui::Spacing();

    // 1) Name (required).
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##np_name", "e.g. churn-risk", &new_project_name_);

    // 2) Natural-language intent (optional — captured via Project::capture_intent,
    //    which itself rejects empty/whitespace-only text).
    ImGui::Spacing();
    ImGui::TextUnformatted("Intent");
    ImGui::InputTextMultiline("##np_intent", &new_project_intent_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f));
    ImGui::TextDisabled(
        "Optional. e.g. \"Score every active account for churn risk and tell me "
        "why.\"");

    // 3) n8n workflow artifact (optional). Import via the native dialog or by
    //    dropping a .json on this dialog; staged + validated, imported on Create.
    ImGui::Spacing();
    ImGui::TextUnformatted("n8n workflow (artifact)");
    if (ImGui::Button("Import n8n workflow...")) {
        static const SDL_DialogFileFilter filters[] = {
            {"n8n workflow (JSON)", "json"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(&App::on_open_n8n, this, window_, filters, 2,
                               nullptr, false);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or drop a workflow .json on this dialog");

    ImGui::BeginChild("##np_dropzone",
                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 2.4f),
                      ImGuiChildFlags_Borders);
    if (new_project_artifact_path_.empty()) {
        ImGui::TextDisabled("(no workflow staged)");
        ImGui::TextDisabled("Drop an n8n workflow JSON here, or use the button.");
    } else {
        ImGui::TextUnformatted(new_project_artifact_name_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            new_project_artifact_path_.clear();
            new_project_artifact_name_.clear();
            new_project_artifact_status_.clear();
            new_project_artifact_valid_ = false;
        }
        if (!new_project_artifact_status_.empty()) {
            ImGui::TextColored(
                new_project_artifact_valid_ ? kSuccessColor : kErrorColor, "%s",
                new_project_artifact_status_.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    if (!new_project_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", new_project_error_.c_str());
    }

    // Create runs the whole flow atomically. A staged-but-invalid artifact
    // blocks the create so the project is never built with a broken artifact.
    const bool artifact_blocks =
        !new_project_artifact_path_.empty() && !new_project_artifact_valid_;
    if (primary_button("Create project")) {
        if (new_project_name_.empty()) {
            new_project_error_ = "name required";
        } else if (artifact_blocks) {
            new_project_error_ =
                "the staged n8n file is not a valid workflow; fix or clear it";
        } else if (auto project = node_.create_project(new_project_name_)) {
            std::string summary = "Created project '" + project->name() + "'";
            if (project->capture_intent(new_project_intent_)) {
                summary += " + intent";
            }
            if (!new_project_artifact_path_.empty()) {
                const N8nImportResult imported = import_n8n_workflow_file(
                    *project, new_project_artifact_path_);
                summary += imported.ok()
                               ? " + n8n artifact (" + new_project_artifact_name_ +
                                     ")"
                               : " (artifact import failed: " + imported.error +
                                     ")";
            }
            selected_project_ = project->name();
            io_status_ = summary;
            reset_new_project_form();
            ImGui::CloseCurrentPopup();
        } else {
            new_project_error_ = "a project by that name already exists";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        reset_new_project_form();
        ImGui::CloseCurrentPopup();
    }

    if (!stay_open) {  // window close (x)
        reset_new_project_form();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

}  // namespace zetta::gui
