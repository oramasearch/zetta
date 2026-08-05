#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/agent/agent_store.hpp"
#include "zetta/agent/model_cache.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cfloat>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zetta::gui {
namespace {

// Split a comma-separated tool list into trimmed, non-empty names.
std::vector<std::string> split_tools(const std::string& text) {
    std::vector<std::string> tools;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const std::size_t begin = item.find_first_not_of(" \t\r\n");
        const std::size_t end = item.find_last_not_of(" \t\r\n");
        if (begin != std::string::npos) tools.push_back(item.substr(begin, end - begin + 1));
    }
    return tools;
}

void clear_add_agent_form(std::string& name, std::string& model, std::string& description,
                          std::string& prompt, std::string& tools) {
    name.clear();
    model.clear();
    description.clear();
    prompt.clear();
    tools.clear();
}

}  // namespace

// The inline "New agent" affordance, collapsed by default so the list stays the focus.
// Creates a replicated agent:<id> via AgentStore (a uuid + ISO timestamps stamped here),
// mirroring the create_agent capability — the same record the MCP/REST surface produces.
void App::draw_add_agent_form() {
    if (!ImGui::CollapsingHeader("New agent")) return;
    ImGui::TextDisabled(
        "Create a served agent: a model (a local checkpoint dir, or a HuggingFace repo id "
        "you have pulled with `zetta pull`) plus an optional system prompt and the tools it "
        "may call.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##aa_name", "required, e.g. Base agent", &add_agent_name_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##aa_model", "required, e.g. data/checkpoints/base-agent-0.6b",
                             &add_agent_model_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Description");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##aa_desc", "optional", &add_agent_description_);
    ImGui::Spacing();
    ImGui::TextUnformatted("System prompt");
    ImGui::InputTextMultiline("##aa_prompt", &add_agent_system_prompt_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::Spacing();
    ImGui::TextUnformatted("Tools");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##aa_tools",
                             "comma-separated, e.g. connect, clean, model, index, serve, act",
                             &add_agent_tools_);

    ImGui::Spacing();
    ImGui::Separator();
    banner(Tone::Error, add_agent_error_);

    if (primary_button("Create agent")) {
        add_agent_error_.clear();
        add_agent_status_.clear();
        if (node_.replication() == nullptr) {
            add_agent_error_ = "agents require a replicated node";
        } else {
            zetta::agent::AgentRecord record;
            record.id = zetta::util::new_uuid();
            record.created_at = zetta::util::now_iso8601();
            record.updated_at = record.created_at;
            record.name = add_agent_name_;
            record.model = add_agent_model_;
            record.description = add_agent_description_;
            record.system_prompt = add_agent_system_prompt_;
            record.tools = split_tools(add_agent_tools_);
            std::string error;
            zetta::agent::AgentStore store(*node_.replication());
            if (const std::optional<zetta::agent::AgentRecord> stored =
                    store.put(record, &error)) {
                selected_agent_ = stored->id;
                add_agent_status_ = "Created agent '" + stored->name + "'";
                clear_add_agent_form(add_agent_name_, add_agent_model_, add_agent_description_,
                                     add_agent_system_prompt_, add_agent_tools_);
            } else {
                add_agent_error_ = error;  // the engine's reason, verbatim
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        clear_add_agent_form(add_agent_name_, add_agent_model_, add_agent_description_,
                             add_agent_system_prompt_, add_agent_tools_);
        add_agent_error_.clear();
    }
}

// Edit an existing agent in the detail pane (#849). The working copy lives in edit_agent_*
// (seeded when Edit is pressed, in draw_agents); Save writes it back through AgentStore::put —
// the same record the update_agent capability produces, so the GUI + MCP/REST + web agree.
// The model isn't reloaded on a prompt change: respond() applies the prompt per turn, so the
// edit takes effect on the very next chat message.
void App::draw_edit_agent_form(const zetta::agent::AgentRecord& agent) {
    ImGui::TextDisabled("Edit agent");
    ImGui::Spacing();
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ea_name", "required", &edit_agent_name_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ea_model", "a checkpoint dir, or a pulled HuggingFace repo id",
                             &edit_agent_model_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Description");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ea_desc", "optional", &edit_agent_description_);
    ImGui::Spacing();
    ImGui::TextUnformatted("System prompt");
    ImGui::InputTextMultiline("##ea_prompt", &edit_agent_system_prompt_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f));
    ImGui::Spacing();
    ImGui::TextUnformatted("Tools");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ea_tools", "comma-separated, e.g. connect, clean, model, serve",
                             &edit_agent_tools_);

    ImGui::Spacing();
    ImGui::Separator();
    banner(Tone::Error, edit_agent_error_);

    if (primary_button("Save changes")) {
        edit_agent_error_.clear();
        if (node_.replication() == nullptr) {
            edit_agent_error_ = "agents require a replicated node";
        } else if (edit_agent_name_.empty()) {
            edit_agent_error_ = "name is required";
        } else if (edit_agent_model_.empty()) {
            edit_agent_error_ = "model is required";
        } else {
            zetta::agent::AgentRecord updated = agent;  // preserves id + created_at
            updated.name = edit_agent_name_;
            updated.model = edit_agent_model_;
            updated.description = edit_agent_description_;
            updated.system_prompt = edit_agent_system_prompt_;
            updated.tools = split_tools(edit_agent_tools_);
            updated.updated_at = zetta::util::now_iso8601();
            std::string error;
            if (zetta::agent::AgentStore(*node_.replication()).put(updated, &error)) {
                add_agent_status_ = "Updated agent '" + updated.name + "'";
                edit_agent_id_.clear();  // back to the read-only detail
            } else {
                edit_agent_error_ = error;  // the engine's reason, verbatim
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        edit_agent_id_.clear();
        edit_agent_error_.clear();
    }
}

// The Agents section (#849): the served agents you can chat with. A thin, read-only viewer
// over the replicated agent:<id> records (AgentStore over the CRDT — the same source the
// MCP/REST list_agents reads), plus the inline "New agent" affordance. Held by id and
// re-resolved each frame, like the Models panel.
void App::draw_agents() {
    page_header("Agents",
                "Served agents you can chat with: a model, a system prompt, and tools");

    std::vector<zetta::agent::AgentRecord> agents;
    if (node_.replication() != nullptr) {
        agents = zetta::agent::AgentStore(*node_.replication()).list();
    }
    std::optional<std::string> pending = take_pending_selection(View::Agents);

    if (agents.empty()) {
        draw_add_agent_form();
        banner(Tone::Ok, add_agent_status_);
        empty_state("No agents yet",
                    "Create a served agent from a model checkpoint, then chat with it over "
                    "the API/MCP. Pull open weights with `zetta pull` if you need a model first.",
                    nullptr);
        return;
    }
    ImGui::TextDisabled("%zu agent(s)", agents.size());
    draw_add_agent_form();
    banner(Tone::Ok, add_agent_status_);

    // Model cache (#849): how many checkpoints stay resident (default 1) so a warm agent
    // doesn't reload the multi-GB weights per chat; plus what's currently loaded.
    {
        zetta::agent::ModelCache& cache = zetta::agent::ModelCache::instance();
        int capacity = static_cast<int>(cache.capacity());
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Model cache \xC2\xB7 keep");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt("##model_capacity", &capacity)) {
            cache.set_capacity(capacity < 1 ? 1 : static_cast<std::size_t>(capacity));
        }
        ImGui::SameLine();
        const std::vector<std::string> resident = cache.loaded();
        ImGui::TextDisabled("model(s) resident \xC2\xB7 %zu loaded", resident.size());
        if (!resident.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Unload all")) cache.unload_all();
        }
    }

    // Agents have no lifecycle status (muted dot); each row's title is the name (or the
    // UUID's first segment when unnamed) with the model as meta.
    std::vector<MasterRow> rows;
    rows.reserve(agents.size());
    for (const zetta::agent::AgentRecord& agent : agents) {
        // The row meta is a SHORT label — the model's last path segment (or repo name),
        // never the full checkpoint path (which would overrun the row).
        const std::size_t slash = agent.model.find_last_of('/');
        const std::string model_label =
            slash == std::string::npos ? agent.model : agent.model.substr(slash + 1);
        rows.push_back({.id = agent.id,
                        .title = agent.name.empty() ? entity_short_id(agent.id) : agent.name,
                        .meta = model_label,
                        .id_title = agent.name.empty()});
    }

    master_detail(
        "agents", rows, selected_agent_, std::move(pending), "(no agents)",
        "Select an agent to view its model, prompt, and tools.",
        [&](const MasterRow& row) {
            const zetta::agent::AgentRecord* agent = nullptr;
            for (const zetta::agent::AgentRecord& candidate : agents) {
                if (candidate.id == row.id) {
                    agent = &candidate;
                    break;
                }
            }
            if (agent == nullptr) return;

            // Editing this agent replaces the read-only detail with the edit form.
            if (edit_agent_id_ == agent->id) {
                draw_edit_agent_form(*agent);
                return;
            }

            if (!agent->description.empty()) {
                ImGui::TextWrapped("%s", agent->description.c_str());
                ImGui::Spacing();
            }
            mono_line(kInkQuiet, "model:    ", agent->model);
            mono_line(kInkQuiet, "id:       ", agent->id);
            if (!agent->created_at.empty()) {
                mono_line(kInkQuiet, "created:  ", agent->created_at);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Tools");
            if (agent->tools.empty()) {
                ImGui::TextDisabled("(no tools)");
            } else {
                std::string tools;
                for (const std::string& tool : agent->tools) {
                    if (!tools.empty()) tools += "  ·  ";
                    tools += tool;
                }
                if (g_mono_font) ImGui::PushFont(g_mono_font);
                ImGui::TextColored(kInkSecondary, "%s", tools.c_str());
                if (g_mono_font) ImGui::PopFont();
            }

            if (!agent->system_prompt.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("System prompt");
                ImGui::TextWrapped("%s", agent->system_prompt.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled(
                "Talk to this agent in the Chat view (left nav) or over the API/MCP `chat` "
                "capability.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Open chat")) {
                chat_agent_ = row.id;  // preselect this agent in the Chat view
                navigate_to(View::Chat);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit")) {
                // Seed the working copy from the record, then the detail pane swaps to the
                // edit form (the branch at the top of this lambda).
                edit_agent_id_ = agent->id;
                edit_agent_name_ = agent->name;
                edit_agent_model_ = agent->model;
                edit_agent_description_ = agent->description;
                edit_agent_system_prompt_ = agent->system_prompt;
                edit_agent_tools_.clear();
                for (const std::string& tool : agent->tools) {
                    if (!edit_agent_tools_.empty()) edit_agent_tools_ += ", ";
                    edit_agent_tools_ += tool;
                }
                edit_agent_error_.clear();
                add_agent_status_.clear();
            }
            ImGui::Spacing();
            // Load / unload this agent's model in the cache. Loading runs OFF the UI thread
            // (a detached worker — it only touches the process-wide cache), so the frame never
            // freezes; the button flips to "Unload" once the weights are resident.
            zetta::agent::ModelCache& cache = zetta::agent::ModelCache::instance();
            if (cache.is_loaded(agent->model) && model_loading_path_ == agent->model) {
                model_loading_path_.clear();
            }
            if (model_loading_path_ == agent->model) {
                ImGui::TextDisabled("loading model\xE2\x80\xA6");
            } else if (cache.is_loaded(agent->model)) {
                if (ImGui::Button("Unload model")) cache.unload(agent->model);
            } else if (ImGui::Button("Load model")) {
                model_loading_path_ = agent->model;
                std::thread([path = agent->model]() {
                    std::string error;
                    zetta::agent::ModelCache::instance().load(path, &error);
                }).detach();
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete agent") && node_.replication() != nullptr) {
                zetta::agent::AgentStore(*node_.replication()).remove(row.id);
                selected_agent_.clear();
            }
        });
}

}  // namespace zetta::gui
