#include "zetta/agent/sandbox.hpp"

#include <utility>

namespace zetta::agent {

void Sandbox::register_tool(std::string name, ToolEffect effect) {
    tools_.insert_or_assign(std::move(name), std::move(effect));
}

bool Sandbox::has_tool(const std::string& name) const {
    return tools_.contains(name);
}

ApplyOutcome Sandbox::apply(const ToolCall& call) {
    const auto tool = tools_.find(call.tool);
    if (tool == tools_.end()) {
        // Unknown tool: reject and leave the world untouched. A rollout must not
        // be able to hallucinate a tool and have the world silently agree.
        return ApplyOutcome{ApplyStatus::UnknownTool,
                            "no tool named '" + call.tool + "'"};
    }
    ToolResult result = tool->second(world_, call.arguments);
    if (!result.ok) {
        return ApplyOutcome{ApplyStatus::ToolFailed, std::move(result.reason), {}};
    }
    return ApplyOutcome{ApplyStatus::Ok, {}, std::move(result.data)};
}

}  // namespace zetta::agent
