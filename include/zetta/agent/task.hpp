// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"

// A task (a "game"): one enterprise tool-calling problem the agent trains on and
// is graded against (#555; docs/agent/README.md). It is deliberately readable —
// you can look at a task and tell what success is (feat-12 step 2) — and it is
// *verifiable*: its success criterion is a required EndState a deterministic
// grader checks, not an opinion. A task with no required facts is NOT trainable
// (there is nothing to verify) — the constraint #554 places on any suggested task
// before it can enter the curriculum.
namespace zetta::agent {

class Task {
public:
    Task(std::string id, std::string description, EndState initial_state,
         std::vector<std::string> tool_names, EndState required)
        : id_(std::move(id)),
          description_(std::move(description)),
          initial_state_(std::move(initial_state)),
          tool_names_(std::move(tool_names)),
          required_(std::move(required)) {}

    const std::string& id() const noexcept { return id_; }
    const std::string& description() const noexcept { return description_; }

    // The world before the agent acts.
    const EndState& initial_state() const noexcept { return initial_state_; }

    // The tools available to the agent in this task. Their effects are wired in
    // the sandbox; here the task declares which tools are in scope.
    const std::vector<std::string>& tool_names() const noexcept {
        return tool_names_;
    }

    // The success criterion: the facts a correct agent must produce.
    const EndState& required() const noexcept { return required_; }

    // The deterministic grader for this task's success criterion.
    EndStateGrader grader() const { return EndStateGrader{required_}; }

    // Is this a task the agent can actually be trained on? Only if it has a
    // verifiable success criterion — at least one required fact to check. A task
    // with nothing to verify cannot enter the curriculum (#554).
    bool is_trainable() const noexcept { return !required_.empty(); }

private:
    std::string id_;
    std::string description_;
    EndState initial_state_;
    std::vector<std::string> tool_names_;
    EndState required_;
};

}  // namespace zetta::agent
