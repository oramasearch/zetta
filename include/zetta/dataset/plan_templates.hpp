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
    // What a Project running this solution is CALLED, and the table it works over.
    //
    // These must match the solution, because the grounding shows them and the model reads them. A
    // corpus that labelled a Project "Lifetime value forecast" while requiring a gbdt classifier,
    // or "Churn risk" while requiring regression, is not merely unhelpful — it teaches the model to
    // IGNORE the Project name, since the name predicts nothing. That is exactly what happened: a
    // checkpoint trained on such a corpus got `model_kind` wrong on 23 of 100 canary cases.
    std::string project;               // "Lead scoring - inbound"
    std::string table;                 // "leads"
};

// Fill in `project` and `table` for every template (see the fields above). Separate from the
// template list only to keep the correctness facts and the naming readable side by side.
void label_solution_templates(std::vector<SolutionTemplate>& templates);

// The plan's subjects as templates (grows toward the full checklist — see
// docs/agent/training-coverage-checklist.md). Deterministic; no LLM in the product path.
std::vector<SolutionTemplate> plan_solution_templates();

// Build an admission-gated AdmissionCase from a template: reference = the required facts;
// negative controls each break a distinct required fact (so the grader must discriminate).
zetta::agent::AdmissionCase case_from_template(const SolutionTemplate& solution_template);

}  // namespace zetta::dataset
