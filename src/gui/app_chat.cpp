#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"

#include "zetta/agent/agent_store.hpp"
#include "zetta/agent/chat.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zetta::gui {

// A chat turn running OFF the UI thread: the worker loads the agent's model + generates
// (seconds), writes the response, and sets `done`; poll_chat_op() applies it on the UI
// thread (the #624 spawn/poll pattern, with a std::thread like the test worker). The done
// flag + response are shared_ptrs so the worker never references the op that owns the thread.
struct ChatOp {
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<zetta::agent::ChatResponse> response =
        std::make_shared<zetta::agent::ChatResponse>();
    ~ChatOp() {
        if (worker.joinable()) worker.join();
    }
};

void App::start_chat_op(const std::string& agent_id, const std::string& message) {
    auto op = std::make_shared<ChatOp>();
    std::shared_ptr<std::atomic<bool>> done = op->done;
    std::shared_ptr<zetta::agent::ChatResponse> response = op->response;
    // The ReplicationManager is mutex-guarded (AgentStore reads its snapshot) and the model
    // load + generation are pure, so this is safe off the main thread.
    zetta::cluster::ReplicationManager* replication = node_.replication();
    op->worker = std::thread([done, response, replication, agent_id, message]() {
        if (replication == nullptr) {
            response->error = "agents require a replicated node";
        } else if (std::optional<zetta::agent::AgentRecord> agent =
                       zetta::agent::AgentStore(*replication).get(agent_id)) {
            *response = zetta::agent::respond(*agent, message);
        } else {
            response->error = "agent '" + agent_id + "' not found";
        }
        done->store(true, std::memory_order_release);
    });
    chat_op_ = std::move(op);
}

void App::poll_chat_op() {
    if (!chat_op_ || !chat_op_->done->load(std::memory_order_acquire)) return;
    const zetta::agent::ChatResponse& response = *chat_op_->response;
    if (response.ok) {
        std::string tools;
        for (const zetta::agent::ChatToolInvocation& invocation : response.result.invocations) {
            if (!tools.empty()) tools += "  ·  ";
            const char* status = invocation.status == zetta::agent::ApplyStatus::Ok ? "ok"
                                 : invocation.status == zetta::agent::ApplyStatus::UnknownTool
                                     ? "unknown"
                                     : "failed";
            tools += invocation.call.tool + "(" + status + ")";
        }
        chat_history_.push_back({/*from_user=*/false, response.result.reply, std::move(tools)});
    } else {
        chat_error_ = response.error;  // the engine's reason, verbatim
    }
    chat_op_.reset();  // ~ChatOp joins the finished worker
}

// The Chat view (#849): pick an agent and talk to it. The turn (model load + generation +
// tool-calling) runs off the UI thread, so the frame never blocks; the reply lands via
// poll_chat_op(). A pure view over the live agent list + the transcript.
void App::draw_chat() {
    page_header("Chat", "Talk to an agent — it proposes a pipeline and calls tools");

    std::vector<zetta::agent::AgentRecord> agents;
    if (node_.replication() != nullptr) {
        agents = zetta::agent::AgentStore(*node_.replication()).list();
    }
    if (agents.empty()) {
        if (empty_state("No agents yet",
                        "Create a served agent in the Agents section, then come back here to "
                        "chat with it.",
                        "Go to Agents")) {
            navigate_to(View::Agents);
        }
        return;
    }

    // Default / validate the selected agent against the live list.
    bool valid = false;
    for (const zetta::agent::AgentRecord& agent : agents) {
        if (agent.id == chat_agent_) {
            valid = true;
            break;
        }
    }
    if (!valid) chat_agent_ = agents.front().id;

    const auto label_of = [&agents](const std::string& id) -> std::string {
        for (const zetta::agent::AgentRecord& agent : agents) {
            if (agent.id == id) {
                return agent.name.empty() ? entity_short_id(agent.id) : agent.name;
            }
        }
        return id;
    };

    // The agent selector.
    ImGui::TextUnformatted("Agent");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("##chat_agent", label_of(chat_agent_).c_str())) {
        for (const zetta::agent::AgentRecord& agent : agents) {
            const bool selected = agent.id == chat_agent_;
            const std::string label =
                agent.name.empty() ? entity_short_id(agent.id) : agent.name;
            if (ImGui::Selectable(label.c_str(), selected) && chat_agent_ != agent.id) {
                chat_agent_ = agent.id;  // switching agents starts a fresh transcript
                chat_history_.clear();
                chat_error_.clear();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear chat")) {
        chat_history_.clear();
        chat_error_.clear();
    }
    ImGui::Separator();

    // The transcript (scrolls; the input row is pinned below).
    const float input_height = ImGui::GetFrameHeightWithSpacing() + 12.0f;
    ImGui::BeginChild("##chat_transcript", ImVec2(0.0f, -input_height), true);
    for (const ChatMessage& message : chat_history_) {
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::TextColored(message.from_user ? kInkQuiet : kInkPrimary,
                           message.from_user ? "you" : "agent");
        if (g_mono_font) ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(message.text.c_str());
        ImGui::PopTextWrapPos();
        if (!message.tools.empty()) {
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kInkQuiet, "tools: %s", message.tools.c_str());
            if (g_mono_font) ImGui::PopFont();
        }
        ImGui::Spacing();
    }
    if (chat_op_) ImGui::TextDisabled("thinking…");
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    banner(Tone::Error, chat_error_);

    // The input row. Disabled while a turn is running (one turn at a time).
    const bool busy = chat_op_ != nullptr;
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-96.0f);
    const bool entered = ImGui::InputTextWithHint(
        "##chat_input", "Ask the agent to build something…", &chat_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool clicked = ImGui::Button("Send", ImVec2(88.0f, 0.0f));
    ImGui::EndDisabled();

    if ((clicked || entered) && !busy) {
        const std::size_t begin = chat_input_.find_first_not_of(" \t\r\n");
        if (begin != std::string::npos) {
            const std::string message = chat_input_.substr(begin);
            chat_history_.push_back({/*from_user=*/true, message, {}});
            chat_error_.clear();
            start_chat_op(chat_agent_, message);
            chat_input_.clear();
        }
    }
}

}  // namespace zetta::gui
