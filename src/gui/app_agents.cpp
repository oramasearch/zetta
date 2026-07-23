#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/agent/agent_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cfloat>
#include <optional>
#include <sstream>
#include <string>
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

    // Agents have no lifecycle status (muted dot); each row's title is the name (or the
    // UUID's first segment when unnamed) with the model as meta.
    std::vector<MasterRow> rows;
    rows.reserve(agents.size());
    for (const zetta::agent::AgentRecord& agent : agents) {
        rows.push_back({.id = agent.id,
                        .title = agent.name.empty() ? entity_short_id(agent.id) : agent.name,
                        .meta = agent.model,
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
                "Chat with this agent over the API/MCP `chat` capability; an in-panel chat "
                "with a selector is coming.");
            ImGui::Spacing();
            if (ImGui::Button("Delete agent") && node_.replication() != nullptr) {
                zetta::agent::AgentStore(*node_.replication()).remove(row.id);
                selected_agent_.clear();
            }
        });
}

}  // namespace zetta::gui
