#pragma once

#include <string>
#include <vector>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/interaction.hpp"

// The interaction corpus (#918) — the training data for every kind of turn a user actually has
// with Orama Studio, not just "intent → pipeline".
//
// The agent is harness code with a model attached, so the corpus's job is to be the recorded
// input/output of the EXACT interactions the harness runs. Each family below names one, and
// every case carries: the context the harness would have retrieved (`grounding`), the ask, the
// reference response envelope, the facts a correct response must satisfy, and negative controls
// that must FAIL. Discrimination is the product, not the pass.
//
// **Where the volume comes from.** Not hand-listing thousands of examples: the families are a
// cross product of (subjects × contexts × phrasings × difficulty). The important multiplier is
// CONTEXT VARIATION in the grounded-QA families — hold the question fixed and vary the retrieved
// facts, and the example becomes unanswerable without reading them. That is the anti-fabrication
// signal, and it is free to generate. Deterministic throughout (no RNG in the product path).
namespace zetta::dataset {

// A synthetic Project's state — what deterministic retrieval would hand the model before an ask.
// One of these renders into several different grounding views (results, data, cluster, …), so a
// single context feeds many families.
struct ProjectContext {
    std::string project;                                    // "Churn risk — EMEA mid-market"
    // Which solution template this Project IS (a plan id: "S-2", "M-10", "UC-20", …).
    //
    // Every Project in the corpus is a real use case with a real, production-grade pipeline, not a
    // generic one. Without this every family answered from the same supervised skeleton, so 92% of
    // action cases taught two pipeline shapes and the trained agent emitted one pipeline for churn,
    // lead scoring and collections alike. Carrying the solution means a feature request on a
    // forecasting Project produces the FORECASTING pipeline — backtested, no calibration — and on
    // a retrieval Project the indexed one.
    std::string solution;                                   // "S-2", "M-10", … (a plan template id)
    std::string table;                                      // the primary table name
    std::string model_kind;                                 // "gbdt", "regression", …
    double auc = 0.0;                                       // 0 when the Project has no model yet
    double accuracy = 0.0;
    bool calibrated = true;
    long long rows = 0;
    long long columns = 0;
    double missing_fraction = 0.0;
    std::vector<std::pair<std::string, double>> drivers;    // feature → share of importance
    std::vector<std::pair<std::string, double>> at_risk;    // account → risk, rank order
    std::vector<std::pair<std::string, std::string>> schema; // column → type
    int nodes = 1;                                          // cluster state
    double disk_used_gb = 0.0;
    double disk_total_gb = 0.0;
    int running_jobs = 0;
    std::string last_run_status;                            // "succeeded" / "failed"
    std::string last_run_failure;                           // reason when failed
};

// Which slice of a Project's state the harness retrieved for this ask. The same context renders
// differently depending on what was asked, exactly as the capability registry would return.
enum class GroundingView {
    Results,   // model measure + ranked drivers + at-risk list
    Data,      // schema, row/column counts, missingness
    Cluster,   // nodes, disk, running jobs
    Runs,      // last run status / failure reason
    None,      // no retrieval (build-from-intent asks)
};

// Deterministic synthetic contexts — `count` distinct Projects with genuinely different measures,
// drivers, accounts and shapes, so a model cannot answer any of them from memory.
std::vector<ProjectContext> synthetic_project_contexts(int count);

// Render one view of a context as the facts block the harness would supply.
std::string render_grounding(const ProjectContext& context, GroundingView view);

// One named family of interactions and the gated cases it produced.
struct InteractionFamily {
    std::string name;                                 // "qa_results", "feature_engineering", …
    std::string description;                          // one line, for the coverage report
    std::vector<zetta::agent::AdmissionCase> cases;
};

// Build every family over an explicit context set. The generator uses this overload to split
// contexts into a training set and a held-out canary, so the canary is a set of Projects whose
// numbers the model has never seen — a real memorization check for the grounded-QA families.
std::vector<InteractionFamily> interaction_families(const std::vector<ProjectContext>& contexts,
                                                    int phrasings);

// Build every family over `contexts_per_family` synthetic contexts. `contexts_per_family` is the
// main lever on corpus size (the context-variation multiplier); `phrasings` scales paraphrase
// coverage per ask.
std::vector<InteractionFamily> interaction_families(int contexts_per_family, int phrasings);

}  // namespace zetta::dataset
