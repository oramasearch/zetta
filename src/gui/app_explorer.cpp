#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/repl_console.hpp"

#include "zetta/governance/access_key.hpp"
#include "zetta/util/bytes.hpp"
#include "zetta/data/database.hpp"
#include "zetta/util/duration.hpp"
#include "zetta/data/import.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/node.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/query/executor.hpp"
#include "zetta/query/service.hpp"
#include "zetta/repl/governed_console.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/data/table.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zetta::gui {

void App::draw_explorer() {
    // Consume a navigate_to(entity) deep link aimed at this view.
    if (auto pending_entity = take_pending_selection(View::Explorer)) {
        selected_project_ = *pending_entity;
    }
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
    // The column's action outcome (#632): the last failure's reason, or the
    // New Project modal's create summary, as a banner where the action landed.
    banner(Tone::Error, project_error_);
    if (project_error_.empty()) banner(Tone::Ok, project_status_);

    ImGui::Separator();

    const std::vector<std::string> project_names = node_.project_names();
    if (project_names.empty()) {
        // The next action is the New project… button directly above (#631).
        empty_state("No projects yet",
                    "A project holds intents, artifacts, and execution "
                    "graphs.",
                    "");
    }
    for (const std::string& name : project_names) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            project_status_.clear();
            if (node_.drop_project(name)) {
                project_error_.clear();
                if (selected_project_ == name) selected_project_.clear();
            } else {
                project_error_ = "could not delete project '" + name + "'";
            }
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
        project_status_.clear();
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
    banner(Tone::Error, db_error_);

    ImGui::Separator();

    const std::vector<std::string> database_names = node_.database_names();
    if (database_names.empty()) {
        // The next action is the create field directly above (#631).
        empty_state("No databases yet",
                    "Create one above to hold tables and large objects.",
                    "");
    }
    for (const std::string& name : database_names) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            if (node_.drop_database(name)) {
                db_error_.clear();
                if (selected_db_ == name) {
                    selected_db_.clear();
                    selected_table_.clear();
                }
            } else {
                db_error_ = "could not drop database '" + name + "'";
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
    // system.nodes). It isn't a real on-disk database, so it's listed
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
        // The catalog comes from zetta-core — the same list `system.tables`
        // serves — so the GUI never owns a copy of it (#684, RULE-7).
        for (const std::string_view table_name :
             zetta::query::system_table_names()) {
            const std::string name{table_name};
            if (ImGui::Selectable(name.c_str(), selected_table_ == name)) {
                selected_table_ = name;
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
    banner(Tone::Error, table_error_);

    ImGui::Separator();

    const std::vector<std::string> table_names = db->table_names();
    if (table_names.empty()) {
        // The next action is the create field directly above (#631).
        empty_state("No tables yet",
                    "Create one above, or add a dataset and it arrives as a "
                    "partitioned table.",
                    "");
    }
    for (const std::string& name : table_names) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            if (db->drop_table(name)) {
                table_error_.clear();
                if (selected_table_ == name) selected_table_.clear();
            } else {
                table_error_ = "could not drop table '" + name + "'";
            }
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
            banner(Tone::Error, outcome.error);
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
    banner(Tone::Error, row_error_);

    ImGui::Spacing();
    const zetta::storage::StorageEngine& store = table->store();
    if (store.size() == 0) {
        // The next actions are the add/import affordances directly above (#631).
        empty_state("No rows yet",
                    "Add a JSON object above, or import a JSON/JSONL file.",
                    "");
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
    ImGui::SeparatorText("SQL query (Postgres)");
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
        query_time_ = zetta::util::format_duration(outcome.elapsed);
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

    // The run's outcome as a banner (#632): the dialect's error verbatim, or
    // the engine's per-statement messages, with the elapsed time folded in.
    const std::string query_suffix =
        query_time_.empty() ? std::string{} : " (" + query_time_ + ")";
    if (!query_error_.empty()) {
        banner(Tone::Error, query_error_ + query_suffix);
    } else if (!query_status_.empty()) {
        banner(Tone::Ok, query_status_ + query_suffix);
    }

    if (ImGui::BeginChild("##query_results", ImVec2(0, 160),
                          ImGuiChildFlags_Borders)) {
        // Nothing run yet this session: say what belongs here (#631). The next
        // action is the Run button directly above.
        if (query_result_columns_.empty() && query_result_rows_.empty() &&
            query_status_.empty() && query_error_.empty()) {
            empty_state("No results yet",
                        "Write a statement above and run it; rows, columns, "
                        "and timing land here.",
                        "");
        }
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

// The governed Console/REPL screen (SCR-8 / #525): host the reusable CMP-6
// ReplConsole component over this node, routing every statement through the
// governed run handler (zetta-core repl::GovernedConsole). Governance — the
// capability check against the acting key, the simulated-Python → IR lowering +
// correctness gate, and the audit record — lives in the core component, so this
// screen only supplies the session inputs (acting key + current database) and the
// "Run as" selector. Held state lives in console_model_, so a future CLI renders
// the same session against the same GovernedConsole.
void App::draw_console() {
    ImGui::SeparatorText("Console (REPL) · governed");
    ImGui::TextDisabled(
        "Run SQL or simulated-Python against this node, governed: every statement "
        "is capability-scoped against the acting key and recorded in the audit "
        "trail; simulated-Python lowers to an execution graph and must pass the "
        "correctness gate before it runs. Unqualified SQL names use the selected "
        "database (%s).",
        selected_db_.empty() ? "none" : selected_db_.c_str());

    // "Run as" — the acting key the session is capability-scoped against. The
    // default is an unauthenticated local session ("" key id): statements run + are
    // audited with no enforcement, so the basic path is frictionless. Selecting a
    // key enforces its capabilities (a key lacking the right blocks the statement,
    // which is still audited). Keys are listed straight off the node's principals
    // (the same source as the Admin panel), re-read each frame so a freshly issued
    // key appears and a dropped one can't leave a dangling selection.
    struct KeyChoice {
        std::string id;     // "" = unauthenticated local
        std::string label;  // selector display
    };
    std::vector<KeyChoice> choices;
    choices.push_back({"", "(unauthenticated local)"});
    bool selection_live = console_session_key_.empty();
    for (const auto& principal : node_.principals()) {
        for (const auto& key : principal->keys()) {
            const std::string short_key = key->id().substr(0, key->id().find('-'));
            std::string label = principal->name() + " · key " + short_key;
            if (key->id() == console_session_key_) selection_live = true;
            choices.push_back({key->id(), std::move(label)});
        }
    }
    // A previously selected key that has since been dropped falls back to local.
    if (!selection_live) console_session_key_.clear();

    std::string current_label = "(unauthenticated local)";
    for (const KeyChoice& choice : choices) {
        if (choice.id == console_session_key_) current_label = choice.label;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    if (ImGui::BeginCombo("Run as", current_label.c_str())) {
        for (const KeyChoice& choice : choices) {
            const bool selected = choice.id == console_session_key_;
            if (ImGui::Selectable(choice.label.c_str(), selected)) {
                console_session_key_ = choice.id;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Feed the session inputs into the governed runner, then forward the run.
    governed_console_->set_session_key(console_session_key_);
    governed_console_->set_database(selected_db_);
    const repl::RunHandler run =
        [this](repl::Frontend frontend,
               const std::string& source) -> repl::Result {
        return governed_console_->run(frontend, source);
    };

    console_->draw(*console_model_, run, mono_font_);
}

void App::draw_large_objects() {
    ImGui::SeparatorText("Files");
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
    banner(Tone::Error, large_object_error_);
    ImGui::Separator();

    const std::vector<zetta::data::LargeObject> objects = db->large_objects().list();
    if (objects.empty()) {
        // The next action is the Store file… button directly above (#631).
        empty_state("No files yet",
                    "Store a file above, or drop one on the window; it syncs "
                    "to the other nodes in the background.",
                    "");
    }
    for (const zetta::data::LargeObject& lo : objects) {
        ImGui::PushID(lo.id.c_str());
        if (ImGui::SmallButton("x")) {
            if (db->large_objects().unlink(lo.id)) {
                large_object_error_.clear();
            } else {
                large_object_error_ = "could not remove '" + lo.name + "'";
            }
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s  (%s)", lo.name.c_str(),
                           zetta::util::format_bytes(lo.size).c_str());
        // The metadata replicates immediately; the body transfers separately,
        // so a freshly-replicated object may not have its bytes here yet.
        if (!lo.body_present) {
            ImGui::SameLine();
            ImGui::TextColored(tone_color(Tone::Warn), "(syncing)");
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
    }
    ImGui::EndChild();
    // The staging/validation result for the dropped file — the n8n schema's
    // reason verbatim when it fails validation.
    if (!new_project_artifact_path_.empty()) {
        banner(new_project_artifact_valid_ ? Tone::Ok : Tone::Error,
               new_project_artifact_status_);
    }

    ImGui::Spacing();
    ImGui::Separator();
    banner(Tone::Error, new_project_error_);

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
                const zetta::pipeline::N8nImportResult imported = import_n8n_workflow_file(
                    *project, new_project_artifact_path_);
                summary += imported.ok()
                               ? " + n8n artifact (" + new_project_artifact_name_ +
                                     ")"
                               : " (artifact import failed: " + imported.error +
                                     ")";
            }
            selected_project_ = project->name();
            // Surface the outcome where it landed (#632): banner in the
            // Projects column and jump there (the Added-dataset precedent),
            // so a create started from Home/File menu is still visible.
            project_error_.clear();
            project_status_ = summary;
            navigate_to(View::Explorer, project->name());
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
