#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/agent/action.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/task.hpp"

// The admission gate (#555, #248; docs/agent/README.md). Before a task corpus is
// used to train or evaluate the agent, it must EARN admission: every task's
// reference (known-correct) end-state must pass its grader, every negative control
// must fail it, and the corpus must carry at least one negative control — a corpus
// where nothing can fail is measuring nothing (feat-12 step 3). This is the same
// discipline blender-rl enforces with its admission gate, and it is what keeps a
// high eval score honest.
namespace zetta::agent {

// One task's admission evidence.
struct AdmissionCase {
    Task task;
    EndState reference;                       // must PASS the task's grader
    std::vector<EndState> negative_controls;  // each must FAIL the task's grader
    // The reference solution in the agent's ACTION shape (#873): {message, tool_calls,
    // execution_graph}. Present once the corpus is reshaped from facts to actions (the
    // generator fills it); the SFT render uses it as the training target when set. The
    // `reference` facts above stay as the gate's checks on the run pipeline.
    std::optional<StudioAction> reference_action;
    // The context the HARNESS would have retrieved before this ask (#918) — a model's measure
    // and ranked drivers, an at-risk list, a schema, cluster state. Rendered into the prompt by
    // `render_interaction_prompt`, the same function serving uses, so a training example is
    // byte-identical to the served turn. Empty for asks that need no retrieval (build a
    // pipeline from intent). This is where grounded-QA volume comes from: hold the question
    // fixed and vary this, and the example becomes unanswerable without reading it.
    std::string grounding;
};

enum class AdmissionFailureKind {
    UntrainableTask,          // the task has no verifiable criterion
    ReferenceRejected,        // the reference solution did not pass its grader
    NegativeControlAccepted,  // a negative control passed (the grader caught nothing)
    NoNegativeControls,       // the corpus carries no negative controls at all
};

std::string_view to_string(AdmissionFailureKind kind);

struct AdmissionFailure {
    AdmissionFailureKind kind;
    std::string task_id;  // which task (empty for corpus-wide failures)
    std::string detail;
};

struct AdmissionResult {
    bool admitted = false;
    std::vector<AdmissionFailure> failures;
};

// Admit `corpus` only if: every task is trainable, every reference passes its
// grader, every negative control fails it, and there is at least one negative
// control across the whole corpus. Returns every failure found (not just the
// first), so a corpus can be fixed in one pass.
AdmissionResult admit_corpus(const std::vector<AdmissionCase>& corpus);

}  // namespace zetta::agent
