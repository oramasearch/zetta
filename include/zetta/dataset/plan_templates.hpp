#pragma once

#include <string>
#include <vector>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"

// The native encoding of the solutions-discovery plan's subjects as gated task templates (#878).
// Each template is a plan subject (a solution S-n / method M-n / use case) reduced to the facts
// its correct pipeline must satisfy — mined from the plan's per-method graphs + per-solution
// "what the gate checks" sections. This is how the base-agent generator grows to full-plan
// coverage as DATA, not per-subject code: build_solution_graph turns a template's facts into a
// real pipeline, and case_from_template makes it an admission-gated AdmissionCase (reference
// passes, discriminating negative controls fail).
namespace zetta::dataset {

// One plan subject as a gated task template.
struct SolutionTemplate {
    std::string id;                    // the plan id, e.g. "S-2", "M-10"
    std::string description;           // the plain-language ask (the task description / intent)
    zetta::agent::EndState required;   // the facts the correct pipeline must satisfy
};

// The plan's subjects as templates (grows toward the full checklist — see
// docs/agent/training-coverage-checklist.md). Deterministic; no LLM in the product path.
std::vector<SolutionTemplate> plan_solution_templates();

// Build an admission-gated AdmissionCase from a template: reference = the required facts;
// negative controls each break a distinct required fact (so the grader must discriminate).
zetta::agent::AdmissionCase case_from_template(const SolutionTemplate& solution_template);

}  // namespace zetta::dataset
