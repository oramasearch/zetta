#include "zetta/dataset/interaction_families.hpp"

#include "zetta/dataset/plan_templates.hpp"

#include <array>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/action.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/ir/execution_graph.hpp"

namespace zetta::dataset {
namespace {

using zetta::agent::AdmissionCase;
using zetta::agent::EndState;
using zetta::agent::InteractionKind;
using zetta::agent::StudioAction;
using zetta::agent::Task;

// ---------------------------------------------------------------------------------------------
// Small deterministic helpers. No RNG anywhere: the corpus must be reproducible (ADR-17), so
// every "variation" below is an index into a table, never a sample.
// ---------------------------------------------------------------------------------------------

EndState facts(std::vector<std::pair<std::string, nlohmann::json>> entries) {
    EndState state;
    for (auto& [key, value] : entries) state.set(std::move(key), std::move(value));
    return state;
}

// Fixed-precision formatting so a number in the grounding block is byte-identical to the number
// the answer must cite. Anything looser makes "did the answer use a number from the context?"
// undecidable, which would rot the reward.
std::string number(double value, int decimals) {
    const double scale = decimals == 2 ? 100.0 : 1000.0;
    long long scaled = static_cast<long long>(value * scale + (value < 0 ? -0.5 : 0.5));
    std::string digits = std::to_string(scaled < 0 ? -scaled : scaled);
    while (static_cast<int>(digits.size()) <= decimals) digits.insert(digits.begin(), '0');
    std::string out = (scaled < 0 ? "-" : "");
    out += digits.substr(0, digits.size() - static_cast<std::size_t>(decimals));
    out += ".";
    out += digits.substr(digits.size() - static_cast<std::size_t>(decimals));
    return out;
}

std::string percent(double share) { return number(share * 100.0, 1) + "%"; }

// Build negative controls by breaking one required fact at a time, so the grader must
// discriminate on each independently. A control that breaks everything proves nothing.
std::vector<EndState> controls_breaking_each(const EndState& required, int limit) {
    std::vector<EndState> controls;
    int made = 0;
    for (const auto& [key, value] : required.facts()) {
        if (made >= limit) break;
        EndState control = required;
        if (value.is_boolean()) {
            control.set(key, !value.get<bool>());
        } else if (value.is_string()) {
            control.set(key, value.get<std::string>() + "__wrong");
        } else if (value.is_number()) {
            // A *plausible* wrong number — the fabrication failure mode is a fluent answer
            // citing a number that is not in the context, not a wild one.
            control.set(key, value.get<double>() * 1.07 + 0.013);
        } else {
            control.set(key, false);
        }
        controls.push_back(std::move(control));
        ++made;
    }
    return controls;
}

// Assemble one gated case. `required` always carries the declared mode, so emitting the wrong
// mode is a hard failure — that is the point of the discriminator.
AdmissionCase make_case(std::string id, std::string ask, std::string grounding,
                        InteractionKind kind, std::string message, EndState required,
                        zetta::ir::ExecutionGraph graph = {},
                        std::vector<zetta::agent::ToolCall> tool_calls = {}) {
    required.set("kind", std::string(zetta::agent::to_string(kind)));

    StudioAction action;
    action.kind = kind;
    action.message = std::move(message);
    action.execution_graph = std::move(graph);
    action.tool_calls = std::move(tool_calls);

    // An ACTION is a promise to build something, and the agent builds by CALLING TOOLS — the
    // harness turns each call into a pipeline node. So a case that declares a pipeline must carry
    // the calls that construct it; derive them from the graph when the family did not spell them
    // out, rather than leaving the two encodings free to disagree.
    //
    // They disagreed badly: `tool_calls` defaulted to empty here, and most families passed only a
    // graph, so 91% of the v1 corpus's action cases taught "an action calls nothing" — and the
    // checkpoint trained on it emitted an empty `tool_calls` for every request, building no
    // pipeline at all. Other kinds are left alone on purpose: an abstain carries a graph the gate
    // scores while deliberately building nothing, and answer/clarify build nothing either.
    if (action.kind == InteractionKind::Action && action.tool_calls.empty() &&
        !action.execution_graph.nodes().empty()) {
        action.tool_calls = zetta::agent::tool_calls_from_graph(action.execution_graph);
    }

    // Aggregate-initialized: AdmissionCase holds a Task, which has no default constructor by
    // design (a task without a verifiable criterion should be unrepresentable).
    return AdmissionCase{
        Task{std::move(id), std::move(ask), EndState{},
             {"connect", "clean", "model", "index", "serve", "act"}, required},
        required,
        controls_breaking_each(required, 3),
        std::move(action),
        std::move(grounding)};
}

// ---------------------------------------------------------------------------------------------
// Vocabulary banks. Each is a fixed table; the cross product of these with the contexts is where
// corpus volume comes from.
// ---------------------------------------------------------------------------------------------

const std::array<const char*, 10> kProjectNames = {
    "Churn risk - EMEA mid-market", "Churn risk - NA enterprise", "Lead scoring - inbound",
    "Collections priority", "Lifetime value forecast", "Equipment failure risk",
    "Support escalation risk", "Expansion propensity", "Payment failure risk",
    "Onboarding drop-off risk"};

const std::array<const char*, 10> kTableNames = {
    "accounts",      "customers",     "leads",        "invoices",     "subscriptions",
    "machines",      "tickets",       "opportunities", "payments",    "signups"};

const std::array<const char*, 12> kFeatureNames = {
    "failed_payments", "support_severity", "mrr",          "tenure_days", "total_events",
    "seats",           "last_login_days",  "ticket_count", "nps_score",   "contract_months",
    "usage_trend",     "discount_rate"};

const std::array<const char*, 8> kAccountPrefix = {"ACME", "NORTHWIND", "GLOBEX", "INITECH",
                                                   "UMBRELLA", "SOYLENT", "VEHEMENT", "MASSIVE"};

// Every read capability a user might ask about — the registry IS the answer key for the
// capability-discovery and platform families.
const std::array<const char*, 34> kCapabilities = {
    "list_projects", "get_project",  "list_models",   "get_model",     "list_runs",
    "get_run",       "list_datasets", "get_dataset",  "list_indexes",  "get_index",
    "list_jobs",     "get_job",      "list_traces",   "get_trace",     "list_agents",
    "get_agent",     "list_connections", "list_specs", "get_spec",     "get_graph",
    "list_plans",    "get_plan",     "server_info",   "sql",           "search",
    "list_consent_requests", "list_dataset_generators", "get_workspace_profile",
    "cloud_inspect", "detect_provisioners", "testsuite_list", "list_agents",
    "get_dataset",   "list_models"};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Contexts
// ---------------------------------------------------------------------------------------------

std::vector<ProjectContext> synthetic_project_contexts(int count) {
    std::vector<ProjectContext> contexts;
    if (count < 1) count = 1;
    for (int index = 0; index < count; ++index) {
        ProjectContext context;
        const std::size_t slot = static_cast<std::size_t>(index);
        // Name the Project after the solution it runs, and give it that solution's table. Naming
        // it independently made the grounding LIE about what the Project was — "Lifetime value
        // forecast" paired with a gbdt classifier — which teaches the model that the Project name
        // predicts nothing and must be ignored.
        const std::vector<SolutionTemplate>& plan_for_name = plan_solution_templates();
        const SolutionTemplate& named = plan_for_name[slot % plan_for_name.size()];
        context.project = named.project + " #" + std::to_string(index + 1);
        context.table = named.table;
        // Round-robin over the whole plan, so the contexts collectively cover every use case and
        // methodology rather than 30 variations of one supervised Project.
        const SolutionTemplate& solution = named;
        context.solution = solution.id;
        // The model kind the SOLUTION calls for — not a coin flip. A forecasting Project has a
        // forecasting model; a retrieval Project has none.
        const auto kind = solution.required.facts().find("model_kind");
        context.model_kind = (kind != solution.required.facts().end() && kind->second.is_string())
                                 ? kind->second.get<std::string>()
                                 : "gbdt";
        // Deliberately spread across the believable range, and NEVER 1.000 — a perfect measure
        // reads as leakage to the technical buyer this product is sold to.
        context.auc = 0.72 + static_cast<double>((index * 7) % 22) / 100.0;
        context.accuracy = 0.66 + static_cast<double>((index * 5) % 25) / 100.0;
        context.calibrated = (index % 7) != 4;
        context.rows = 1200 + static_cast<long long>(index) * 337;
        context.columns = 9 + (index % 14);
        context.missing_fraction = static_cast<double>((index * 3) % 18) / 100.0;
        context.nodes = 1 + (index % 5);
        context.disk_used_gb = 4.0 + static_cast<double>((index * 11) % 90);
        context.disk_total_gb = 200.0 + static_cast<double>((index % 4) * 200);
        context.running_jobs = index % 4;
        context.last_run_status = (index % 6 == 5) ? "failed" : "succeeded";
        context.last_run_failure =
            (index % 6 == 5) ? "the source connection timed out after 30s" : "";

        // Drivers: a rotating subset, normalized to shares of total importance so that the
        // model's instinct to write them as percentages is CORRECT. Fighting that instinct with
        // prompt text does not work; making the numbers actually be shares does.
        const int driver_count = 4 + (index % 3);
        double total = 0.0;
        std::vector<std::pair<std::string, double>> raw;
        for (int slot_index = 0; slot_index < driver_count; ++slot_index) {
            const std::size_t which =
                static_cast<std::size_t>(index + slot_index * 3) % kFeatureNames.size();
            const double weight =
                1.0 / static_cast<double>(slot_index + 1) + static_cast<double>(index % 3) / 10.0;
            raw.emplace_back(kFeatureNames[which], weight);
            total += weight;
        }
        for (auto& [name, weight] : raw) context.drivers.emplace_back(name, weight / total);

        // At-risk accounts, strictly descending so rank fidelity is checkable.
        const int at_risk_count = 5 + (index % 4);
        for (int rank = 0; rank < at_risk_count; ++rank) {
            const std::string account =
                std::string(kAccountPrefix[static_cast<std::size_t>(index + rank) %
                                           kAccountPrefix.size()]) +
                "-" + std::to_string(1000 + index * 13 + rank * 7);
            const double risk = 0.94 - static_cast<double>(rank) * 0.031 -
                                static_cast<double>(index % 5) * 0.011;
            context.at_risk.emplace_back(account, risk);
        }

        for (int column = 0; column < 6; ++column) {
            const std::size_t which =
                static_cast<std::size_t>(index + column * 2) % kFeatureNames.size();
            context.schema.emplace_back(kFeatureNames[which], column % 3 == 0 ? "text" : "double");
        }
        contexts.push_back(std::move(context));
    }
    return contexts;
}

std::string render_grounding(const ProjectContext& context, GroundingView view) {
    std::string text = "Project: " + context.project + "\n";
    switch (view) {
        case GroundingView::Results: {
            if (context.auc <= 0.0) {
                text += "Model: none trained yet for this Project.\n";
                break;
            }
            text += "Model: " + context.model_kind + ", trained on " + context.table + ".\n";
            text += "Measure: AUC " + number(context.auc, 3) + ", accuracy " +
                    number(context.accuracy, 3) + ".\n";
            text += std::string("Calibrated: ") + (context.calibrated ? "yes" : "no") + ".\n";
            text +=
                "Drivers, strongest first - each shown as its share of the model's total "
                "feature importance (they sum to 100%):\n";
            for (const auto& [name, share] : context.drivers) {
                text += "  " + name + ": " + percent(share) + "\n";
            }
            text += "Accounts most at risk, highest first:\n";
            for (std::size_t rank = 0; rank < context.at_risk.size(); ++rank) {
                text += "  " + std::to_string(rank + 1) + ". " + context.at_risk[rank].first +
                        " - risk " + number(context.at_risk[rank].second, 3) + "\n";
            }
            break;
        }
        case GroundingView::Data: {
            text += "Table: " + context.table + "\n";
            text += "Rows: " + std::to_string(context.rows) +
                    ", columns: " + std::to_string(context.columns) + "\n";
            text += "Missing values: " + percent(context.missing_fraction) + " of cells\n";
            text += "Schema:\n";
            for (const auto& [column, type] : context.schema) {
                text += "  " + column + ": " + type + "\n";
            }
            break;
        }
        case GroundingView::Cluster: {
            text += "Cluster nodes: " + std::to_string(context.nodes) + "\n";
            text += "Disk used: " + number(context.disk_used_gb, 2) + " GB of " +
                    number(context.disk_total_gb, 2) + " GB\n";
            text += "Running jobs: " + std::to_string(context.running_jobs) + "\n";
            break;
        }
        case GroundingView::Runs: {
            text += "Last run: " + context.last_run_status + "\n";
            if (!context.last_run_failure.empty()) {
                text += "Failure reason: " + context.last_run_failure + "\n";
            }
            break;
        }
        case GroundingView::None:
            return std::string();
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Families
// ---------------------------------------------------------------------------------------------

namespace {

// --- Class 5: grounded QA about results -------------------------------------------------------
InteractionFamily qa_results_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{"qa_results",
                             "Answer about a Project's model measure, drivers, and at-risk list "
                             "using only the retrieved facts",
                             {}};
    const std::array<std::array<const char*, 4>, 8> asks = {{
        {{"What's driving churn in our accounts?", "Which factors matter most here?",
          "What are the top drivers of this model?", "Why is the model flagging accounts?"}},
        {{"Which accounts are most at risk?", "Who's most likely to churn?",
          "Show me the riskiest accounts.", "Which customers should we worry about?"}},
        {{"How good is this model?", "Is this model any good?", "What's the model's accuracy?",
          "How well does this perform?"}},
        {{"What does the AUC mean here?", "Explain the measure to me.",
          "Is that AUC good or bad?", "How should I read this score?"}},
        {{"Is this model overfit?", "Should I trust these numbers?",
          "Does this look too good to be true?", "Any sign of leakage?"}},
        {{"What's the single biggest driver?", "Which feature dominates?",
          "What's the number one factor?", "Which driver carries the most weight?"}},
        {{"Is the model calibrated?", "Are these probabilities real probabilities?",
          "Can I treat the risk scores as likelihoods?", "How are the scores calibrated?"}},
        {{"Give me the top account and why.", "Who is the highest risk and what's driving it?",
          "Name the riskiest account and the reason.",
          "Which account tops the list, and why is it there?"}},
    }};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        if (context.drivers.empty() || context.at_risk.empty()) continue;
        const std::string grounding = render_grounding(context, GroundingView::Results);
        const std::string top_driver = context.drivers.front().first;
        const std::string top_account = context.at_risk.front().first;

        for (std::size_t ask_index = 0; ask_index < asks.size(); ++ask_index) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 4; ++phrasing) {
                // A required fact must be PROJECTABLE from the response — you cannot verify what
                // you cannot check, and a corpus that requires unverifiable facts silently stops
                // measuring anything. So each ask requires exactly what its reference answer
                // carries, and nothing more.
                EndState required = facts({{"grounded", true}, {"numbers_from_context", true}});
                std::string message;
                switch (ask_index) {
                    case 0:
                    case 5:
                        required.set("top_driver", top_driver);
                        message = "The strongest driver is **" + top_driver + "** at " +
                                  percent(context.drivers.front().second) +
                                  " of total feature importance.";
                        break;
                    case 1:
                    case 7:
                        required.set("top_account", top_account);
                        required.set("top_driver", top_driver);
                        message = "The highest-risk account is **" + top_account + "** at risk " +
                                  number(context.at_risk.front().second, 3) + ", driven mainly by " +
                                  top_driver + ".";
                        break;
                    default:
                        // Round-trip the figure through the SAME formatting the grounding uses,
                        // so the required value is bit-identical to what a correct answer cites.
                        required.set("measure_auc", std::stod(number(context.auc, 3)));
                        required.set("top_driver", top_driver);
                        message = "This model scores **AUC " + number(context.auc, 3) +
                                  "** (accuracy " + number(context.accuracy, 3) +
                                  "), with " + top_driver + " the strongest driver.";
                        break;
                }
                family.cases.push_back(make_case(
                    "qa_results/" + std::to_string(context_index) + "/" +
                        std::to_string(ask_index) + "/" + std::to_string(phrasing),
                    asks[ask_index][static_cast<std::size_t>(phrasing)], grounding,
                    InteractionKind::Answer, message, required));
            }
        }
    }
    return family;
}

// --- Class 6: grounded QA about the data ------------------------------------------------------
InteractionFamily qa_data_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{"qa_data",
                             "Answer about the Project's data: schema, size, missingness", {}};
    const std::array<std::array<const char*, 3>, 6> asks = {{
        {{"What columns do I have?", "Show me the schema.", "What's in this table?"}},
        {{"How many rows are there?", "How big is this dataset?", "What's the row count?"}},
        {{"How much data is missing?", "Do I have null problems?", "What's the missingness?"}},
        {{"Is my data good enough to train on?", "Can I model this?",
          "Is this dataset usable?"}},
        {{"What types are these columns?", "Which columns are numeric?",
          "Tell me the column types."}},
        {{"What table is this Project using?", "Where does this data come from?",
          "Which table is connected?"}},
    }};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string grounding = render_grounding(context, GroundingView::Data);
        for (std::size_t ask_index = 0; ask_index < asks.size(); ++ask_index) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
                EndState required = facts({{"grounded", true},
                                           {"numbers_from_context", true},
                                           {"table", context.table},
                                           {"row_count", context.rows}});
                const std::string message =
                    "`" + context.table + "` has **" + std::to_string(context.rows) +
                    " rows** across " + std::to_string(context.columns) + " columns, with " +
                    percent(context.missing_fraction) + " of cells missing.";
                family.cases.push_back(make_case(
                    "qa_data/" + std::to_string(context_index) + "/" + std::to_string(ask_index) +
                        "/" + std::to_string(phrasing),
                    asks[ask_index][static_cast<std::size_t>(phrasing)], grounding,
                    InteractionKind::Answer, message, required));
            }
        }
    }
    return family;
}

// --- Class 7: platform, cluster, runs ---------------------------------------------------------
InteractionFamily platform_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{"platform",
                             "Answer about cluster resources, jobs, and why a run failed", {}};
    const std::array<std::array<const char*, 3>, 5> cluster_asks = {{
        {{"How many nodes are in the cluster?", "What's my cluster size?",
          "How many machines am I running?"}},
        {{"How much disk am I using?", "Am I running out of space?", "What's my storage usage?"}},
        {{"What's running right now?", "Any jobs in flight?", "Show me active jobs."}},
        {{"Is the cluster healthy?", "Everything okay with the platform?",
          "Any problems with my nodes?"}},
        {{"Do I have capacity for another training run?", "Can I train right now?",
          "Is there room to run this?"}},
    }};
    const std::array<std::array<const char*, 3>, 3> run_asks = {{
        {{"Did my last run work?", "What happened on the last run?", "How did the run go?"}},
        {{"Why did that fail?", "What went wrong?", "Explain the failure."}},
        {{"Should I retry it?", "Can this be re-run?", "What do I do about that run?"}},
    }};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string cluster = render_grounding(context, GroundingView::Cluster);
        for (std::size_t ask_index = 0; ask_index < cluster_asks.size(); ++ask_index) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
                EndState required = facts({{"grounded", true},
                                           {"numbers_from_context", true},
                                           {"node_count", context.nodes},
                                           {"running_jobs", context.running_jobs}});
                const std::string message =
                    "The cluster has **" + std::to_string(context.nodes) + " node(s)**, using " +
                    number(context.disk_used_gb, 2) + " GB of " +
                    number(context.disk_total_gb, 2) + " GB, with " +
                    std::to_string(context.running_jobs) + " job(s) running.";
                family.cases.push_back(make_case(
                    "platform/cluster/" + std::to_string(context_index) + "/" +
                        std::to_string(ask_index) + "/" + std::to_string(phrasing),
                    cluster_asks[ask_index][static_cast<std::size_t>(phrasing)], cluster,
                    InteractionKind::Answer, message, required));
            }
        }

        const std::string runs = render_grounding(context, GroundingView::Runs);
        for (std::size_t ask_index = 0; ask_index < run_asks.size(); ++ask_index) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
                EndState required = facts({{"grounded", true},
                                           {"run_status", context.last_run_status}});
                std::string message = "The last run of " + context.project + " **" +
                                      context.last_run_status + "**.";
                if (!context.last_run_failure.empty()) {
                    required.set("failure_reason", context.last_run_failure);
                    message += " Reason: " + context.last_run_failure + ".";
                }
                family.cases.push_back(make_case(
                    "platform/runs/" + std::to_string(context_index) + "/" +
                        std::to_string(ask_index) + "/" + std::to_string(phrasing),
                    run_asks[ask_index][static_cast<std::size_t>(phrasing)], runs,
                    InteractionKind::Answer, message, required));
            }
        }
    }
    return family;
}

// --- Class 9: capability discovery ------------------------------------------------------------
// --- Class 8: product knowledge ---------------------------------------------------------------
// What Orama Studio IS, and what it can do — the turns that carry no Pipeline and call no tools.
// A user evaluating the platform asks these first: which languages, which sources, which models,
// where it deploys, what happens to their data. An agent that cannot answer them reads as a
// pipeline compiler with a chat box bolted on rather than something that knows its own product.
//
// Grounded in a capability listing rather than answered from the model's memory, for the same
// reason capability_discovery is: it makes "did it name something that actually exists?"
// decidable, so inventing a plausible-sounding feature is a GRADED FAILURE. Overselling is the
// specific fabrication risk for this family — a confident claim about a feature the platform does
// not have is worse than "I don't know", because someone buys on it.
//
// Every entry below is a capability this repository actually implements. Verified against source,
// not asserted: the six step kinds (ir/step_kind.hpp), the Postgres + Snowflake connectors
// (src/core/connect/), GBDT training with TreeSHAP explanation and calibration (src/core/ml/), the
// AWS / Azure / bare-metal provisioners (src/core/cloud/), and the Python frontend
// (src/core/input/python_frontend.cpp). Adding an entry here without the code to back it trains
// the agent to lie about the product.
InteractionFamily product_knowledge_family(int phrasings) {
    InteractionFamily family{
        "product_knowledge",
        "Answer what Orama Studio is and what it can do - no Pipeline, no tool calls, grounded "
        "in a listing of capabilities that genuinely exist",
        {}};

    // {the question, the capability the answer must NAME, the answer}
    const std::array<std::tuple<const char*, const char*, const char*>, 30> knowledge = {{
        {"What is Orama Studio?", "turn a question in plain language into a running Pipeline",
         "Orama Studio is where you turn a question in plain language into a running Pipeline. "
         "You describe the outcome you want; Studio designs the steps, checks them with a "
         "Verification, and runs them on infrastructure you control."},
        {"What languages can I write a step in?", "run Python in a Pipeline step",
         "You can run Python in a Pipeline step for transforms and feature work, and SQL for "
         "anything set-shaped. Most people mix the two across steps."},
        {"Can I use SQL?", "query with Postgres-compatible SQL",
         "Yes. You can query with Postgres-compatible SQL, both inside a Pipeline step and "
         "directly against your data to look around."},
        {"What data sources can you connect to?", "connect to Postgres",
         "Studio can connect to Postgres today, along with Snowflake, and a connect step can "
         "also pull from files and APIs. The connect step is where a source is configured."},
        {"Do you support Snowflake?", "connect to Snowflake",
         "Yes - Studio can connect to Snowflake, and the rest of the Pipeline is identical no "
         "matter which source the data came from."},
        {"What kinds of models can you train?", "train a gradient-boosted model",
         "Studio can train a gradient-boosted model on your data - that is the workhorse for "
         "risk and propensity questions like churn or conversion."},
        {"Can it explain a prediction?", "explain a prediction with TreeSHAP",
         "Yes. Studio can explain a prediction with TreeSHAP, so every score comes with the "
         "features that drove it rather than a bare number."},
        {"Are the probabilities trustworthy?", "calibrate a model's probabilities",
         "Studio can calibrate a model's probabilities, so a score of eight in ten means the "
         "outcome really does happen about eight times in ten."},
        {"Can it do search?", "build a BM25F full-text index",
         "Yes. Studio can build a BM25F full-text index over your documents and serve ranked "
         "results from it."},
        {"Where does this run?", "deploy to your own Kubernetes cluster",
         "Studio can deploy to your own Kubernetes cluster. The platform runs where you put it "
         "rather than in a vendor tenant."},
        {"Can I run it on AWS?", "deploy to AWS",
         "Yes - Studio can deploy to AWS, and the same Pipeline runs unchanged on the other "
         "targets."},
        {"Can I run it on Azure?", "deploy to Azure",
         "Yes - Studio can deploy to Azure, and the same Pipeline runs unchanged on the other "
         "targets."},
        {"Can I run it on my own servers?", "deploy to bare metal",
         "Yes. Studio can deploy to bare metal, which is the usual answer when data is not "
         "allowed to leave the building."},
        {"Does my data leave my infrastructure?", "keep your data inside your own infrastructure",
         "No. Studio is built to keep your data inside your own infrastructure - it deploys into "
         "your environment and the data stays there."},
        {"How do I know a Pipeline is correct?", "run a Verification before anything is deployed",
         "Studio will run a Verification before anything is deployed. A Pipeline that has not "
         "passed one is not treated as production-ready."},
        {"What if a change makes things worse?", "roll back to an earlier Saved Version",
         "You can roll back to an earlier Saved Version. Every deployment points at a specific "
         "Saved Version, so going back is a pointer change rather than a rebuild."},
        {"How do I talk to it from my own code?", "answer over the REST surface",
         "Studio will answer over the REST surface, so anything that can make an HTTP request "
         "can drive it."},
        {"Can I connect an AI agent to it?", "answer over MCP",
         "Yes. Studio will answer over MCP, so an assistant can read from it and drive it "
         "directly as a set of tools."},
        {"Can you predict churn?", "score churn risk for every account",
         "Yes - that is the most common thing people build here. Studio can score churn risk for "
         "every account and rank them so the team works the riskiest first."},
        {"Can you help with lead scoring?", "rank leads by conversion likelihood",
         "Yes. Studio can rank leads by conversion likelihood so sales works the ones most "
         "likely to close."},
        {"Can you forecast customer value?", "forecast customer lifetime value",
         "Yes. Studio can forecast customer lifetime value from each customer's history."},
        {"Can you help collections?", "prioritize overdue accounts for collections",
         "Yes. Studio can prioritize overdue accounts for collections, so the team works the "
         "ones most likely to be recovered."},
        {"Can you predict equipment failure?", "predict equipment failure",
         "Yes. Studio can predict equipment failure from machine history so maintenance goes "
         "where it is needed first."},
        {"Why not just use an LLM for this?", "train on your own data rather than calling a rented model",
         "For a scoring question, Studio will train on your own data rather than calling a rented "
         "model. A general model guesses from what it read on the internet; a model trained on "
         "your history learns what actually preceded the outcome in your business - and you can "
         "measure it against what really happened, which you cannot do with a guess."},
        {"How do I know the score is right?", "measure a model against outcomes it never saw",
         "Studio will measure a model against outcomes it never saw before it is treated as "
         "production-ready. That is what a Verification checks, and a Pipeline that has not "
         "passed one does not get deployed."},
        {"Can I see why it scored something that way?", "explain a prediction with TreeSHAP",
         "Yes. Studio can explain a prediction with TreeSHAP, so each score comes with the "
         "features that drove it - which is also how you catch a model that is right for the "
         "wrong reason."},
        {"What happens when it gets one wrong?", "roll back to an earlier Saved Version",
         "You keep the evidence and the ability to undo. Every score is explained, every "
         "deployment points at a Saved Version, and you can roll back to an earlier Saved Version "
         "without a rebuild."},
        {"Do I need an ML person to run this?", "turn a question in plain language into a running Pipeline",
         "No. Studio will turn a question in plain language into a running Pipeline - it picks the "
         "steps, checks them, and runs them. Someone who knows the data and the business is "
         "enough; the ML decisions are the part it handles."},
        {"Won't it just leak the future into the model?", "build leakage-free point-in-time features",
         "That is the most common way a model looks brilliant and then fails, so Studio will "
         "build leakage-free point-in-time features - each row sees only what was known at the "
         "time - and validate on later data than it trained on."},
        {"Can you detect fraud?", "flag likely fraud for review",
         "Yes. Studio can flag likely fraud for review with the reasons attached, so a reviewer "
         "sees why something was flagged instead of a bare score."},
    }};

    const std::array<const char*, 3> shapes = {"{}", "Quick question - {}", "Before we start: {}"};

    for (std::size_t index = 0; index < knowledge.size(); ++index) {
        for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
            std::string ask = std::get<0>(knowledge[index]);
            if (phrasing > 0) {
                std::string lowered = ask;
                if (!lowered.empty() && lowered[0] >= 'A' && lowered[0] <= 'Z') {
                    lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
                }
                std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                shape.replace(shape.find("{}"), 2, lowered);
                ask = shape;
            }
            // The listing rotates, so the same question meets a different neighbourhood of
            // capabilities each time and the answer cannot be recovered from listing position.
            std::string listing = "Capabilities:\n";
            for (std::size_t offset = 0; offset < 8; ++offset) {
                listing += "  " +
                           std::string(std::get<1>(knowledge[(index + offset) % knowledge.size()])) +
                           "\n";
            }
            EndState required = facts({{"grounded", true},
                                       {"numbers_from_context", true},
                                       {"capability_named", std::get<1>(knowledge[index])},
                                       {"capability_exists", true}});
            family.cases.push_back(make_case("product/" + std::to_string(index) + "/" +
                                                 std::to_string(phrasing),
                                             ask, std::move(listing), InteractionKind::Answer,
                                             std::get<2>(knowledge[index]), required));
        }
    }
    return family;
}

InteractionFamily capability_family(int phrasings) {
    InteractionFamily family{"capability_discovery",
                             "Answer what the platform can do, grounded in the capability "
                             "registry",
                             {}};
    const std::array<const char*, 3> shapes = {"Can Orama {}?", "How do I {}?",
                                               "Is there a way to {}?"};
    for (std::size_t index = 0; index < kCapabilities.size(); ++index) {
        const std::string capability = kCapabilities[index];
        for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
            std::string shape = shapes[static_cast<std::size_t>(phrasing)];
            const std::size_t slot = shape.find("{}");
            std::string readable = capability;
            for (char& character : readable) {
                if (character == '_') character = ' ';
            }
            shape.replace(slot, 2, readable);
            // Grounded in the REGISTRY listing, not in the model's memory of what Orama offers.
            // That makes "did it name a capability that actually exists?" decidable, and makes
            // inventing a plausible-sounding one a graded failure.
            std::string listing = "Capabilities:\n";
            for (std::size_t offset = 0; offset < 8; ++offset) {
                listing += "  " +
                           std::string(kCapabilities[(index + offset) % kCapabilities.size()]) +
                           "\n";
            }
            EndState required = facts({{"grounded", true},
                                       {"numbers_from_context", true},
                                       {"capability_named", capability},
                                       {"capability_exists", true}});
            const std::string message = "Yes - use the `" + capability +
                                        "` capability. It's available over both MCP and the "
                                        "REST surface at `/v1/" + capability + "`.";
            family.cases.push_back(make_case(
                "capability/" + capability + "/" + std::to_string(phrasing), shape,
                std::move(listing), InteractionKind::Answer, message, required));
        }
    }
    return family;
}

// --- Class 10a: abstain when the context cannot support an answer -----------------------------
InteractionFamily abstain_family(const std::vector<ProjectContext>& contexts) {
    InteractionFamily family{
        "abstain",
        "Decline when the retrieved facts do not support the ask, and when the ask is out of "
        "scope - the anti-fabrication family",
        {}};
    const std::array<const char*, 6> unsupported = {
        "Which accounts will churn next quarter?", "What will our revenue be next month?",
        "How many customers did we lose last year?", "What's the average deal size?",
        "Which region is worst?", "How does this compare to last quarter?"};
    const std::array<const char*, 6> out_of_scope = {
        "Write me a poem about our quarterly numbers.", "What's the capital of France?",
        "Book me a flight to Tokyo next week.", "Draft a resignation letter for me.",
        "Summarize today's news.", "Translate this into German."};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        // The ask is reasonable, but the retrieved facts genuinely do not contain the answer.
        // Declining here is the behavior that prompting could never enforce.
        const ProjectContext& context = contexts[context_index];
        ProjectContext thin = context;
        thin.at_risk.clear();
        thin.drivers.clear();
        thin.auc = 0.0;
        const std::string grounding = render_grounding(thin, GroundingView::Results);
        for (std::size_t ask_index = 0; ask_index < unsupported.size(); ++ask_index) {
            // Only projectable facts: declining and building nothing are both checkable from the
            // response. Whether the ask was "in scope" is a property of the ASK, not of the
            // answer, so requiring it would be requiring something no grader can verify.
            EndState required = facts({{"abstain", true}, {"built_nothing", true}});
            family.cases.push_back(make_case(
                "abstain/unsupported/" + std::to_string(context_index) + "/" +
                    std::to_string(ask_index),
                unsupported[ask_index], grounding, InteractionKind::Abstain,
                std::string("I can't answer that from what ") + context.project +
                    " holds - there's no trained model or scored result on `" + context.table +
                    "` yet. Run the Pipeline first and I'll answer from its output.",
                required));
        }
        if (context_index < out_of_scope.size()) {
            EndState required = facts({{"abstain", true}, {"built_nothing", true}});
            family.cases.push_back(make_case(
                "abstain/scope/" + std::to_string(context_index), out_of_scope[context_index],
                std::string(), InteractionKind::Abstain,
                "That isn't something Orama Zetta builds - it's out of scope, so I won't set up a "
                "pipeline for it.",
                required));
        }
    }
    return family;
}

// --- Class 10b: clarify an ambiguous ask ------------------------------------------------------
InteractionFamily clarify_family(const std::vector<ProjectContext>& contexts) {
    InteractionFamily family{
        "clarify", "Ask ONE clarifying question when the ask is ambiguous, and build nothing", {}};
    const std::array<std::pair<const char*, const char*>, 8> asks = {{
        {"Make it better.", "What would you like to improve - the model's accuracy, how fast it "
                            "runs, or which accounts it flags?"},
        {"Fix this.", "What's wrong from your side - the results, the data it's using, or a run "
                      "that failed?"},
        {"Add a feature.", "Which signal would you like added, and from which table?"},
        {"Can you predict that?", "Predict what, specifically - and over what time window?"},
        {"Set it up for the new data.", "Which source should it connect to, and should it replace "
                                        "the current one or run alongside?"},
        {"Run it again but different.", "What should change on the re-run - the data window, the "
                                        "algorithm, or the target?"},
        {"Do the usual thing.", "Which pipeline would you like me to base this on?"},
        {"Make it production ready.", "Do you want a Verification run first, or a deployment of "
                                      "the current Saved Version?"},
    }};
    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const std::string grounding =
            render_grounding(contexts[context_index], GroundingView::Results);
        for (std::size_t ask_index = 0; ask_index < asks.size(); ++ask_index) {
            EndState required = facts({{"clarify", true},
                                       {"asked_one_question", true},
                                       {"built_nothing", true}});
            family.cases.push_back(make_case(
                "clarify/" + std::to_string(context_index) + "/" + std::to_string(ask_index),
                asks[ask_index].first, grounding, InteractionKind::Clarify,
                asks[ask_index].second, required));
        }
    }
    return family;
}

// The supervised correctness profile every predictive pipeline must satisfy — the same facts the
// plan templates encode, so an action family is graded by the existing gate vocabulary.
// The production-grade facts for THIS Project's solution — the template's own correctness profile
// (leakage-free point-in-time features, temporal validation, calibration, SHAP, backtesting,
// approval gating, a GLM baseline where a regulator expects one). Falls back to the supervised
// core for a context with no template.
//
// This is what makes the corpus teach production pipelines rather than one skeleton: a change
// requested on a Project inherits that Project's real pipeline, so the reference answer is the
// correct pipeline for THAT use case.
EndState solution_facts(const ProjectContext& context) {
    for (const SolutionTemplate& candidate : plan_solution_templates()) {
        if (candidate.id == context.solution) return candidate.required;
    }
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"serve", true},
                  {"leakage_free", true},
                  {"point_in_time", true},
                  {"model_kind", context.model_kind},
                  {"temporal_validation", true},
                  {"calibrated", true},
                  {"explain", std::string("shap")}});
}

EndState supervised_facts(const std::string& model_kind) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"serve", true},
                  {"leakage_free", true},
                  {"point_in_time", true},
                  {"model_kind", model_kind},
                  {"temporal_validation", true},
                  {"calibrated", true},
                  {"explain", std::string("shap")}});
}

// --- Class 11: build the solution itself ------------------------------------------------------
// "Predict which machines are likely to fail." "Score our leads." "Forecast demand per SKU." The
// central ask — and the one the whole catalogue exists to answer — had ONE training case per
// solution, while a request to add a rolling window had thousands. So the agent learned to edit
// pipelines far better than to propose one, which is backwards: proposing is the first thing it is
// ever asked to do.
//
// Each case pairs a solution's own ask with a Project actually running that solution, so the
// grounding never contradicts the request. The reply is rendered by `describe_pipeline` from the
// same facts that build the graph, so what the agent says it will do and what it builds cannot
// drift apart.
InteractionFamily solution_build_family(const std::vector<ProjectContext>& contexts,
                                        int phrasings) {
    InteractionFamily family{
        "build_solution",
        "The central ask: a plain-language request for a use case, answered with that use case's "
        "production Pipeline",
        {}};

    // Paraphrases, because a buyer does not read the ask off the catalogue.
    const std::array<const char*, 10> shapes = {
        "{}",
        "Can you {}",
        "We need to {}",
        "I'd like to {}",
        "How would you {}",
        "What would it take to {}",
        "Is it possible to {}",
        "We're trying to {}",
        "Our team wants to {}",
        "Could Orama {}"};

    // An ungrounded case carries no context, so two Projects running the same solution would
    // render byte-identical examples. One per (solution, phrasing).
    std::set<std::string> bare_done;
    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        std::string description;
        for (const SolutionTemplate& candidate : plan_solution_templates()) {
            if (candidate.id == context.solution) description = candidate.description;
        }
        if (description.empty()) continue;

        const std::string grounding = render_grounding(context, GroundingView::Data);
        const EndState required = solution_facts(context);
        const std::string message = zetta::agent::describe_pipeline(required);

        for (int phrasing = 0; phrasing < phrasings && phrasing < 10; ++phrasing) {
            std::string ask = description;
            if (phrasing > 0) {
                std::string lowered = ask;
                if (!lowered.empty() && lowered[0] >= 'A' && lowered[0] <= 'Z') {
                    lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
                }
                std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                shape.replace(shape.find("{}"), 2, lowered);
                ask = shape;
            }
            zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(required);
            family.cases.push_back(make_case(
                "build/" + std::to_string(context_index) + "/" + std::to_string(phrasing), ask,
                grounding, InteractionKind::Action, message, required, std::move(graph)));

            // The SAME ask with no Project at all. This is how the first message of an evaluation
            // actually arrives — nothing is selected yet — and a corpus that only ever shows the
            // grounded form leaves the model off-distribution for the one turn that opens every
            // conversation. Measured: asked to set up a churn pipeline with no Project, a
            // checkpoint trained only on the grounded form answered with a governance approval
            // notice instead of building anything.
            //
            // The reply is the same description, because what the agent will build does not depend
            // on whether a Project has been picked yet — only the table name does, and this reply
            // names none.
            // Keyed on the EXAMPLE, not on the solution: different solutions can share an
            // identical fact set (S-2 and UC-15 are both the supervised core over gbdt), which
            // makes their ungrounded cases byte-identical.
            //
            // Phrasing 0 is skipped because the plan SUBJECT cases already train each template's
            // canonical ask ungrounded — this family's job here is to add the paraphrases of it.
            if (phrasing > 0 && bare_done.insert(ask + "\x1f" + message).second) {
                zetta::ir::ExecutionGraph bare = zetta::agent::build_solution_graph(required);
                family.cases.push_back(
                    make_case("build-bare/" + context.solution + "/" + std::to_string(phrasing),
                              ask, "", InteractionKind::Action, message, required,
                              std::move(bare)));
            }
        }
    }
    return family;
}

// --- Class 10: the cold open ------------------------------------------------------------------
// The first thing anyone says. Not "add a rolling window to the feature step" — "we have a bunch
// of customer data and want to use AI, where do we start?" Every evaluation begins here, and the
// corpus had only vague FOLLOW-UPS ("make it better", "fix this"), whose answers assume a Project
// already exists.
//
// The behaviour this trains is deliberate. A cold open answered with a bare "predict what?" is
// technically a clarify and is a bad product: the person asking does not yet know the vocabulary,
// and bouncing the question back puts the work on them. So the reply ORIENTS FIRST — it names what
// their columns actually support, in their words — and then asks exactly one question. Naming real
// options is also the honest move: it shows what the platform would do rather than promising that
// it could do anything.
//
// Still a `clarify` under the contract: exactly one question, and it builds nothing. Grounded in
// the Data view so the options come from the schema the harness retrieved rather than from a guess
// about what the customer might have.
InteractionFamily discovery_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{
        "cold_open",
        "The first exchange: an exploratory ask with no Project yet - orient from the schema, "
        "name real options, then ask ONE question",
        {}};

    const std::array<const char*, 8> openers = {
        "We have a bunch of customer and billing data and want to use AI. Where do we start?",
        "I'm not sure what's possible here. What would you suggest?",
        "We want to use our data better but don't know where to begin.",
        "What should we build first?",
        "My team wants to do something with AI this quarter. Ideas?",
        "We've got years of history in here and we're not using it. What now?",
        "Where would you start if this were your data?",
        "What's the highest-value thing we could do with this?",
    };

    // What a given kind of Project makes possible, said in the buyer's words rather than the
    // platform's. The options must be things the engine genuinely builds — AND they must match the
    // data in front of them. Keyed on the Project's model kind, because offering "who's likely to
    // leave" on a demand-forecasting table is the same incoherence that taught an earlier
    // checkpoint to ignore the Project name entirely.
    // Keyed on the SOLUTION, not the model kind: gbdt alone covers lead scoring, collections,
    // fraud and claims triage, and offering "who's likely to churn" over an invoices table is the
    // same incoherence that taught an earlier checkpoint to ignore the Project name.
    static const std::map<std::string, std::array<const char*, 3>> kMenus = {
        {"S-2", {"which leads will convert", "which to call first", "what makes them convert"}},
        {"S-3", {"what each customer is worth over time", "which are worth the most", "what moves that number"}},
        {"S-4", {"which overdue accounts will pay", "who to chase first", "what predicts recovery"}},
        {"S-5", {"what demand looks like next month", "how confident to be in it", "where it varies by location"}},
        {"S-6", {"what to reorder and when", "how much, under your stock limits", "what a stockout would cost"}},
        {"S-7", {"when a number moves abnormally", "what's driving the move", "who needs to be told"}},
        {"S-8", {"which machines will fail and roughly when", "what to service first", "the early warning signs"}},
        {"S-10", {"which transactions look fraudulent", "why each was flagged", "how to review without over-blocking"}},
        {"S-11", {"who actually changes because of an offer", "where the spend is wasted", "what the lift is worth"}},
        {"S-12", {"which claims carry risk", "which need a human", "why each was routed that way"}},
        {"M-10", {"what's actually in these documents", "answers with the passage they came from", "which ones matter"}},
        {"M-11", {"the fields on every invoice", "which ones need a human", "how to find any of them later"}},
        {"M-12", {"how these accounts are connected", "which relationships carry risk", "what's hiding behind them"}},
        {"M-13", {"which images show a defect", "which need a second look", "what the model actually saw"}},
        {"M-14", {"what was said on these calls", "which ones went wrong", "what keeps coming up"}},
        {"M-15", {"which offer works for whom", "how fast that's learned", "what exploring is costing you"}},
        {"M-16", {"the best plan under your constraints", "what changing it costs", "where the slack is"}},
        {"M-17", {"a prediction with honest uncertainty", "where the data is too thin to trust", "what would narrow it"}},
        {"M-18", {"the most accuracy your data supports", "whether the gain is real", "what each model contributes"}},
        {"M-19", {"a working model from very few labels", "which rows are worth labelling next", "how much that would help"}},
        {"M-20", {"a model trained without exposing the data", "what privacy that buys you", "what it costs in accuracy"}},
        {"M-21", {"a model that stays fresh as data arrives", "when the pattern has drifted", "who to tell when it does"}},
        {"UC-13", {"which applicants are likely to default", "a defensible reason for every decision", "how it compares to a simple baseline"}},
        {"UC-14", {"the right price within your guardrails", "what a change would earn", "where you're leaving money"}},
        {"UC-15", {"which employees are at risk of leaving", "which teams are most exposed", "what's driving it"}},
        {"UC-16", {"where each ticket should go", "which need a human first", "why each was routed"}},
        {"UC-18", {"which suppliers are becoming risky", "what changed recently", "who needs to be told"}},
        {"UC-19", {"which claims will be denied", "which patients will miss an appointment", "what to do about each"}},
        {"UC-20", {"what cash looks like next quarter", "how confident to be in it", "where the pressure points are"}},
        {"UC-21", {"when something is going wrong", "what's causing it", "who to page"}},
        {"UC-22", {"what's driving scrap and yield", "which factors matter most", "what to change first"}},
        {"UC-23", {"who responds because of the campaign", "where the budget is wasted", "what the incremental lift is"}},
        {"UC-24", {"what your contracts commit you to", "which renewals carry risk", "how to find any clause later"}},
    };
    static const std::array<const char*, 3> kDefaultMenu = {
        "what's likely to happen next", "which of these to act on first", "what's driving it"};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string grounding = render_grounding(context, GroundingView::Data);
        const auto found = kMenus.find(context.solution);
        const std::array<const char*, 3>& menu =
            found != kMenus.end() ? found->second : kDefaultMenu;
        for (std::size_t opener = 0; opener < openers.size(); ++opener) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 1; ++phrasing) {
                EndState required = facts({{"clarify", true},
                                           {"asked_one_question", true},
                                           {"built_nothing", true}});
                // Orient from the columns they actually have, offer three real things, ask ONE
                // question. No figures: nothing here is a measured result.
                const std::string message =
                    std::string("With `") + context.table +
                    "` I can usually get to three things: " + menu[0] + ", " + menu[1] +
                    ", and " + menu[2] +
                    ". Each one is a Pipeline I'd build, verify, and run on your own "
                    "infrastructure. Which of those matters most to you right now?";
                family.cases.push_back(make_case(
                    "open/" + std::to_string(context_index) + "/" + std::to_string(opener) + "/" +
                        std::to_string(phrasing),
                    openers[opener], grounding, InteractionKind::Clarify, message, required));
            }
        }
    }
    return family;
}

// --- Class 9: replace what they already have --------------------------------------------------
// The single most common way this product is actually encountered: the person already built
// something. An n8n workflow calling an LLM per row, a cron job running a SQL script, a notebook
// someone re-runs monthly, a rules engine with a hand-tuned threshold. They are not asking "what is
// AI" — they are asking whether this is better than the thing they maintain.
//
// The corpus had no turn of this shape at all, which made the agent unable to hold the conversation
// the product is positioned on: hand over the artifact you already have, get a correct pipeline
// back. It is also the hardest interaction to fake, because the honest answer has to name what the
// existing approach genuinely lacks rather than dismiss it.
//
// Two rules this family encodes, both load-bearing:
//   * NEVER disparage what they built. It works, it got them here, and the person reading is the
//     person who built it. Name what it cannot do — not what is wrong with them.
//   * The improvement must be SPECIFIC and true of the pipeline actually produced: a point-in-time
//     cut, a temporal split, calibration, per-prediction explanations, a Verification before
//     deploy. A vague "it's more robust" teaches the agent to oversell.
InteractionFamily migration_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{
        "replace_existing",
        "Take what the user already built - an n8n flow, a script, a notebook, a rules engine - "
        "and produce the production Pipeline that replaces it",
        {}};

    // {what they have today, what that approach cannot do, what the Pipeline gives them instead}
    const std::array<std::tuple<const char*, const char*, const char*>, 12> artifacts = {{
        {"Right now an n8n workflow calls an LLM on every row each night to score them.",
         "asks a general model to guess, one row at a time",
         "trains on your own history instead of guessing, and scores every row in one pass"},
        {"We have a Zapier flow that emails us when a number crosses a threshold.",
         "fires on a threshold somebody picked",
         "learns which movements actually matter, and ranks what to work first"},
        {"There's a cron job that runs a SQL script and drops a CSV in a bucket.",
         "recomputes the same query and cannot tell you why a row scored the way it did",
         "keeps the query as a step, adds a model on top, and explains each score"},
        {"A data scientist re-runs a notebook by hand every month.",
         "depends on one person remembering to run it",
         "runs on a schedule, versioned, with the results served where the team already works"},
        {"Honestly it's a spreadsheet - someone scores the rows manually.",
         "scales with the time of the person filling it in",
         "scores every row automatically and keeps the reasoning visible"},
        {"We have dbt models feeding a dashboard, and someone eyeballs it weekly.",
         "shows what happened, and leaves the judgement to whoever is looking",
         "keeps your dbt models as the source and adds the prediction and the ranking on top"},
        {"There's an Airflow DAG that loads a pickled scikit-learn model.",
         "has no check that the model is still any good before it serves",
         "revalidates on later data than it trained on and will not deploy a Saved Version that "
         "fails Verification"},
        {"We use a rules engine - a couple of hand-picked thresholds and we flag the row.",
         "treats every row that trips a threshold the same",
         "weighs each signal against everything else you know about the row"},
        {"We pay a vendor for a score and we don't know how they compute it.",
         "cannot be inspected, and cannot be tuned to your business",
         "trains on your data, in your environment, with the drivers behind every score visible"},
        {"It's a formula field in Salesforce that someone set up years ago.",
         "was tuned to a business that has since changed",
         "retrains as the data moves and tells you when the pattern has drifted"},
        {"A Looker dashboard, and then we go on gut feel.",
         "puts the whole judgement on the person reading it",
         "makes the judgement explicit and measurable, so it can be checked and improved"},
        {"We built a Python service that calls an LLM API once per row.",
         "pays per row and cannot be evaluated against a ground truth",
         "runs the model on your own infrastructure and is measured against what actually "
         "happened"},
    }};

    const std::array<const char*, 7> shapes = {
        "{} Can Orama do better?",
        "{} What would you do instead?",
        "{} Is this something you'd replace?",
        "{} We're wondering if there's a better way.",
        "{} Would this be better here?",
        "{} Honestly, is that good enough?",
        "{} What's the production version of that?"};

    std::set<std::string> migrate_bare_done;  // see build_solution: one per (solution, ask)
    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string grounding = render_grounding(context, GroundingView::Data);
        for (std::size_t artifact = 0; artifact < artifacts.size(); ++artifact) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 7; ++phrasing) {
                std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                shape.replace(shape.find("{}"), 2, std::get<0>(artifacts[artifact]));

                // The replacement is THIS Project's real solution, not a generic pipeline — the
                // whole point is that the answer is production-grade for their actual use case.
                EndState required = solution_facts(context);
                required.set("replaces_existing", true);
                zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(required);

                // Credit what they built, name the limit, then say what changes. No numbers: this
                // reply is not grounded in a measured result, so a figure here would be invented.
                const std::string message =
                    std::string("That already does real work - it just ") +
                    std::get<1>(artifacts[artifact]) + ". I'd keep `" + context.table +
                    "` as the source and build the " + context.project +
                    " Pipeline, which " + std::get<2>(artifacts[artifact]) +
                    ". Nothing switches over until it passes a Verification and you deploy it.";

                family.cases.push_back(make_case(
                    "migrate/" + std::to_string(context_index) + "/" +
                        std::to_string(artifact) + "/" + std::to_string(phrasing),
                    shape, grounding, InteractionKind::Action, message, required,
                    std::move(graph)));

                // ...and with no Project selected, which is how this question is actually asked:
                // somebody describes what they run today before they have set anything up here.
                // Names no table, because there is none to name.
                const std::string bare_message =
                    std::string("That already does real work - it just ") +
                    std::get<1>(artifacts[artifact]) +
                    ". I'd keep your source and build a Pipeline that " +
                    std::get<2>(artifacts[artifact]) +
                    ". Nothing switches over until it passes a Verification and you deploy it.";
                const std::string bare_key = shape + "\x1f" + bare_message;
                if (migrate_bare_done.insert(bare_key).second) {
                    zetta::ir::ExecutionGraph bare =
                        zetta::agent::build_solution_graph(required);
                    family.cases.push_back(make_case(
                        "migrate-bare/" + context.solution + "/" + std::to_string(artifact) + "/" +
                            std::to_string(phrasing),
                        shape, "", InteractionKind::Action, bare_message, required,
                        std::move(bare)));
                }
            }
        }
    }
    return family;
}

// --- Class 3: feature engineering -------------------------------------------------------------
InteractionFamily feature_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{
        "feature_engineering",
        "Add or correct features on an existing Pipeline, preserving leakage-free point-in-time "
        "discipline",
        {}};
    // {what the user asks for, the fact it must produce, what the agent says it DID}
    //
    // The third column exists because this family previously answered every one of its 1080 cases
    // with one identical sentence. A constant target is the cheapest thing a model can learn, and
    // it learned it — the checkpoint replayed corpus messages verbatim for unrelated asks. The
    // reply now names the actual change and the actual table, so the target varies with the input
    // the way a real answer does. Deliberately free of digits: this family's reply is not grounded
    // in a retrieved measure, so stating a figure here would train fabrication.
    const std::array<std::tuple<const char*, const char*, const char*>, 12> requests = {{
        {"Add a rolling 30-day event count.", "rolling_window",
         "added the rolling event count over your chosen window"},
        {"Add a 90-day lag of usage.", "lag_feature", "added the lagged usage feature"},
        {"Roll up support tickets per account.", "aggregate_feature",
         "rolled support tickets up to one row per account"},
        {"Join in the billing table as of the scoring date.", "point_in_time_join",
         "joined billing in as of the scoring date"},
        {"Drop anything that leaks the outcome.", "leakage_removed",
         "dropped the columns that leaked the outcome"},
        {"One-hot encode the plan tier.", "categorical_encoded",
         "one-hot encoded the plan tier"},
        {"Handle the class imbalance.", "imbalance_handled",
         "rebalanced the classes for training"},
        {"Impute the missing NPS scores.", "imputed", "imputed the missing NPS scores"},
        {"Add a trend feature on monthly usage.", "trend_feature",
         "added the monthly usage trend"},
        {"Bucket tenure into quartiles.", "binned_feature", "bucketed tenure into quartiles"},
        {"Normalize the revenue columns.", "scaled_feature", "normalized the revenue columns"},
        {"Add days since last login.", "recency_feature", "added days since last login"},
    }};
    const std::array<const char*, 3> shapes = {"{}", "Can you {}", "I want to {}"};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string grounding = render_grounding(context, GroundingView::Data);
        for (std::size_t request = 0; request < requests.size(); ++request) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
                std::string ask = std::get<0>(requests[request]);
                if (phrasing > 0) {
                    std::string lowered = ask;
                    if (!lowered.empty() && lowered[0] >= 'A' && lowered[0] <= 'Z') {
                        lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
                    }
                    std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                    shape.replace(shape.find("{}"), 2, lowered);
                    ask = shape;
                }
                EndState required = solution_facts(context);
                required.set(std::get<1>(requests[request]), true);
                required.set("preserved_other_steps", true);
                zetta::ir::ExecutionGraph graph =
                    zetta::agent::build_solution_graph(required);
                family.cases.push_back(make_case(
                    "feature/" + std::to_string(context_index) + "/" + std::to_string(request) +
                        "/" + std::to_string(phrasing),
                    ask, grounding, InteractionKind::Action,
                    std::string("I ") + std::get<2>(requests[request]) + " on `" + context.table +
                        "` and put it in the Pipeline's feature step. The point-in-time cut is "
                        "preserved, so nothing leaks, and the rest of the Pipeline is unchanged.",
                    required, std::move(graph)));
            }
        }
    }
    return family;
}

// --- Class 4: code inside a pipeline node -----------------------------------------------------
InteractionFamily code_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{
        "code_in_node",
        "Emit Python or SQL for a Pipeline node - graded by EXECUTING it, the strongest "
        "verifiable reward available",
        {}};
    const std::array<std::tuple<const char*, const char*, const char*>, 10> tasks = {{
        {"Write a transform that buckets tenure into quartiles.", "python", "tenure_bucket"},
        {"Compute a rolling 7-day mean of events.", "python", "events_7d_mean"},
        {"Flag accounts with more than three failed payments.", "sql", "payment_risk_flag"},
        {"Select accounts active in the last 30 days.", "sql", "recent_active"},
        {"Add a ratio of seats to MRR.", "python", "seats_per_mrr"},
        {"Aggregate ticket counts per account per month.", "sql", "tickets_per_month"},
        {"Write a custom metric for precision at the top decile.", "python", "precision_at_10"},
        {"Filter out trial accounts.", "sql", "paid_only"},
        {"Compute days since the last login.", "python", "days_since_login"},
        {"Rank accounts by risk within each region.", "sql", "risk_rank_by_region"},
    }};
    const std::array<const char*, 4> shapes = {"{}", "Can you {}", "I need to {}",
                                               "Please {}"};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string grounding = render_grounding(context, GroundingView::Data);
        for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
            const auto& [request, language, column] = tasks[task_index];
            for (int phrasing = 0; phrasing < phrasings && phrasing < 4; ++phrasing) {
                std::string ask = request;
                if (phrasing > 0) {
                    std::string lowered = ask;
                    if (!lowered.empty() && lowered[0] >= 'A' && lowered[0] <= 'Z') {
                        lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
                    }
                    std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                    shape.replace(shape.find("{}"), 2, lowered);
                    ask = shape;
                }
                // The code step lands INSIDE this Project's real pipeline, not beside a stub.
                // Writing a transform does not discard the model, the index, or the action layer
                // that Project already has — and a corpus that said otherwise taught the agent
                // that every code request collapses to connect->clean->serve, which was 34% of
                // all action cases on one shape.
                EndState required = solution_facts(context);
                required.set("code_language", std::string(language));
                required.set("code_runs", true);
                required.set("output_column", std::string(column));
                required.set("leakage_free", true);
                required.set("preserved_other_steps", true);
                zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(required);
                family.cases.push_back(make_case(
                    "code/" + std::to_string(context_index) + "/" + std::to_string(task_index) +
                        "/" + std::to_string(phrasing),
                    ask, grounding, InteractionKind::Action,
                    std::string("I added a ") + language + " step to the Pipeline that produces `" +
                        column + "`. It runs against `" + context.table +
                        "` and its output is available to the next step.",
                    required, std::move(graph)));
            }
        }
    }
    return family;
}

// --- Class 2: modify an existing pipeline -----------------------------------------------------
InteractionFamily modify_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{
        "modify",
        "Change an existing Pipeline by re-emitting the WHOLE graph, so the gate can still score "
        "a runnable pipeline",
        {}};
    // {the change asked for, the fact it must produce, what the agent says it DID}
    //
    // The third column names the specific edit. A single constant confirmation across every case
    // here trained the agent to acknowledge changes it had not made — and, worse, to emit that
    // sentence for unrelated asks, because a constant is the cheapest target to memorize.
    // Digit-free on purpose: this reply states no retrieved figure, so it cannot fabricate one.
    const std::array<std::tuple<const char*, const char*, const char*>, 10> changes = {{
        {"Use a wider data window - go back two years.", "data_window_changed",
         "widened the training window to two years"},
        {"Switch the algorithm to a gradient-boosted tree.", "algorithm_changed",
         "switched the model step to a gradient-boosted tree"},
        {"Change the prediction horizon to 60 days.", "horizon_changed",
         "moved the prediction horizon out to the window you asked for"},
        {"Add an approval step before it acts.", "approval_gated",
         "put an approval step in front of the action"},
        {"Remove the indexing step, we don't need search.", "step_removed",
         "removed the indexing step"},
        {"Also write the results to a table.", "step_added",
         "added a step that writes the results back to a table"},
        {"Retrain it weekly instead of monthly.", "schedule_changed",
         "moved the retraining schedule to weekly"},
        {"Predict downgrade instead of churn.", "target_changed",
         "repointed the model at downgrade instead of churn"},
        {"Exclude the trial accounts from training.", "filter_added",
         "filtered the trial accounts out of training"},
        {"Add a calibration step.", "calibrated", "added a calibration step after the model"},
    }};
    const std::array<const char*, 3> shapes = {"{}", "Can you {}", "Let's {}"};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const ProjectContext& context = contexts[context_index];
        const std::string grounding = render_grounding(context, GroundingView::Results);
        for (std::size_t change = 0; change < changes.size(); ++change) {
            for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
                std::string ask = std::get<0>(changes[change]);
                if (phrasing > 0) {
                    std::string lowered = ask;
                    if (!lowered.empty() && lowered[0] >= 'A' && lowered[0] <= 'Z') {
                        lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
                    }
                    std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                    shape.replace(shape.find("{}"), 2, lowered);
                    ask = shape;
                }
                EndState required = solution_facts(context);
                required.set(std::get<1>(changes[change]), true);
                // The whole graph is re-emitted, never a delta: a mutation is not runnable, so it
                // could not be scored by the gate, and the gate is the reward.
                required.set("preserved_other_steps", true);
                zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(required);
                family.cases.push_back(make_case(
                    "modify/" + std::to_string(context_index) + "/" + std::to_string(change) +
                        "/" + std::to_string(phrasing),
                    ask, grounding, InteractionKind::Action,
                    std::string("I ") + std::get<2>(changes[change]) + " on `" + context.project +
                        "`. Only that step changed - everything else is as it was. Review it and "
                        "I'll run a Verification.",
                    required, std::move(graph)));
            }
        }
    }
    return family;
}

// --- Class 8: governance + lifecycle ----------------------------------------------------------
InteractionFamily governance_family(const std::vector<ProjectContext>& contexts, int phrasings) {
    InteractionFamily family{
        "governance",
        "Lifecycle and governance turns: approvals, audit, rollback, deployment readiness", {}};
    // {ask, the CAPABILITY a correct response calls, is it an action}. Governance turns are
    // graded on the capability requested — a checkable fact — rather than on prose claiming an
    // approval was recorded.
    const std::array<std::tuple<const char*, const char*, bool>, 8> asks = {{
        {"What's waiting on my approval?", "list_consent_requests", false},
        {"Who ran this last?", "list_runs", false},
        {"Show me the audit trail for this Project.", "list_traces", false},
        {"Is this ready for production?", "get_spec", false},
        {"Roll back to the previous Saved Version.", "deploy", true},
        {"Deploy the approved version.", "deploy", true},
        {"Who can see this Project?", "get_project", false},
        {"Schedule this to run nightly.", "run", true},
    }};
    const std::array<const char*, 3> shapes = {"{}", "Quick question - {}", "Hey, {}"};

    for (std::size_t context_index = 0; context_index < contexts.size(); ++context_index) {
        const std::string grounding =
            render_grounding(contexts[context_index], GroundingView::Runs);
        for (std::size_t ask_index = 0; ask_index < asks.size(); ++ask_index) {
            const auto& [request, capability, is_action] = asks[ask_index];
            for (int phrasing = 0; phrasing < phrasings && phrasing < 3; ++phrasing) {
                std::string ask = request;
                if (phrasing > 0) {
                    std::string lowered = ask;
                    if (!lowered.empty() && lowered[0] >= 'A' && lowered[0] <= 'Z') {
                        lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
                    }
                    std::string shape = shapes[static_cast<std::size_t>(phrasing)];
                    shape.replace(shape.find("{}"), 2, lowered);
                    ask = shape;
                }
                EndState required =
                    facts({{std::string("called_") + capability, true},
                           {"grounded", true},
                           {"numbers_from_context", true}});
                std::vector<zetta::agent::ToolCall> calls;
                calls.emplace_back(capability, nlohmann::json::object());
                family.cases.push_back(make_case(
                    "governance/" + std::to_string(context_index) + "/" +
                        std::to_string(ask_index) + "/" + std::to_string(phrasing),
                    ask, grounding,
                    is_action ? InteractionKind::Action : InteractionKind::Answer,
                    is_action ? std::string("I've queued `") + capability + "` on " +
                                    contexts[context_index].project +
                                    " as a governed change - it needs an approval before it takes "
                                    "effect. Here's the impact preview."
                              : std::string("I read `") + capability + "` for " +
                                    contexts[context_index].project +
                                    " - here's what its records show.",
                    required, {}, std::move(calls)));
            }
        }
    }
    return family;
}

}  // namespace

std::vector<InteractionFamily> interaction_families(const std::vector<ProjectContext>& contexts,
                                                    int phrasings) {
    if (phrasings < 1) phrasings = 1;
    std::vector<InteractionFamily> families;
    families.push_back(qa_results_family(contexts, phrasings));
    families.push_back(qa_data_family(contexts, phrasings));
    families.push_back(platform_family(contexts, phrasings));
    families.push_back(capability_family(phrasings));
    families.push_back(product_knowledge_family(phrasings));
    families.push_back(migration_family(contexts, phrasings));
    families.push_back(discovery_family(contexts, phrasings));
    families.push_back(solution_build_family(contexts, phrasings));
    families.push_back(abstain_family(contexts));
    families.push_back(clarify_family(contexts));
    families.push_back(feature_family(contexts, phrasings));
    families.push_back(code_family(contexts, phrasings));
    families.push_back(modify_family(contexts, phrasings));
    families.push_back(governance_family(contexts, phrasings));
    return families;
}

std::vector<InteractionFamily> interaction_families(int contexts_per_family, int phrasings) {
    if (contexts_per_family < 1) contexts_per_family = 1;
    return interaction_families(synthetic_project_contexts(contexts_per_family), phrasings);
}

}  // namespace zetta::dataset
