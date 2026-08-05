#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/tool_call.hpp"

// The sandbox: the world an agent acts in during a rollout (#555;
// docs/agent/README.md). It holds the current EndState and a set of registered
// tools; applying a ToolCall runs that tool's effect against the world and reports
// what happened. An unknown tool, or a tool that fails, is REPORTED — never
// silently ignored — because a rollout that pretended a bad call succeeded would
// corrupt the reward. A tool's effect is an injected callable, so tests wire
// deterministic effects with no external system.
namespace zetta::agent {

// The result of running one tool's effect: did it succeed, if not why, and any
// value the tool produced. `data` is what a READ tool answers with — the agent
// needs the rows/schema back, not just "ok" — and it rides into the
// <tool_result> the next generation round reads (#904).
struct ToolResult {
    bool ok = false;
    std::string reason;   // why it failed; empty on success
    nlohmann::json data;  // what the tool returned; null when it produced no value

    static ToolResult success() { return ToolResult{true, {}, nlohmann::json()}; }
    static ToolResult success(nlohmann::json value) {
        return ToolResult{true, {}, std::move(value)};
    }
    static ToolResult failure(std::string why) {
        return ToolResult{false, std::move(why), nlohmann::json()};
    }
};

// A tool's effect: given the current world and the call's arguments, mutate the
// world and report success/failure. Injected per tool so it stays deterministic
// and fakeable in tests.
using ToolEffect =
    std::function<ToolResult(EndState& world, const nlohmann::json& arguments)>;

// What applying a ToolCall did.
enum class ApplyStatus {
    Ok,           // the tool ran and succeeded
    UnknownTool,  // no tool by that name is registered
    ToolFailed,   // the tool ran and reported failure
};

struct ApplyOutcome {
    ApplyStatus status = ApplyStatus::UnknownTool;
    std::string reason;   // detail for UnknownTool / ToolFailed; empty on Ok
    nlohmann::json data;  // what a read tool returned; null when none (#904)

    bool ok() const noexcept { return status == ApplyStatus::Ok; }
};

class Sandbox {
public:
    Sandbox() = default;
    explicit Sandbox(EndState initial) : world_(std::move(initial)) {}

    // Register a tool by name. Re-registering a name replaces its effect.
    void register_tool(std::string name, ToolEffect effect);

    // Is a tool registered under `name`?
    bool has_tool(const std::string& name) const;

    // Apply one tool call to the world. Unknown tool -> UnknownTool with the
    // world left unchanged; the tool reported failure -> ToolFailed; otherwise Ok.
    ApplyOutcome apply(const ToolCall& call);

    // The current world-state.
    const EndState& state() const noexcept { return world_; }

private:
    EndState world_;
    std::map<std::string, ToolEffect> tools_;
};

}  // namespace zetta::agent
