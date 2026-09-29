#include "zetta/ml/solution.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/methodology.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kIssueBase =
    "https://github.com/oramasearch/zetta/issues/";

std::string issue(int number) {
    return std::string(kIssueBase) + std::to_string(number);
}

std::vector<SolutionCapabilityLevel>
levels(std::string first, std::string second, std::string third,
       std::string fourth) {
    return {
        {1, "CL-1", std::move(first)},
        {2, "CL-2", std::move(second)},
        {3, "CL-3", std::move(third)},
        {4, "CL-4", std::move(fourth)},
    };
}

Solution band_a(
    std::string id, std::string name, std::string promise,
    std::string use_case, std::vector<SolutionMethodology> composition,
    std::vector<std::string> pipeline_order,
    std::vector<std::string> action_patterns,
    std::vector<std::string> latency_classes,
    std::vector<SolutionAutonomyDefault> autonomy_defaults,
    std::vector<SolutionDataRole> required_data_roles,
    std::string decision_policy_template, std::string measurement_design,
    std::vector<std::string> guardrails,
    std::vector<SolutionCapabilityLevel> capability_levels,
    std::string promotion_condition = {}) {
    Solution solution;
    solution.id = std::move(id);
    solution.name = std::move(name);
    solution.promise = std::move(promise);
    solution.use_case = std::move(use_case);
    solution.composition = std::move(composition);
    solution.pipeline_order = std::move(pipeline_order);
    solution.action_patterns = std::move(action_patterns);
    solution.latency_classes = std::move(latency_classes);
    solution.autonomy_defaults = std::move(autonomy_defaults);
    solution.required_data_roles = std::move(required_data_roles);
    solution.decision_policy_template = std::move(decision_policy_template);
    solution.measurement_design = std::move(measurement_design);
    solution.guardrails = std::move(guardrails);
    solution.capability_levels = std::move(capability_levels);
    solution.promotion_condition = std::move(promotion_condition);
    solution.templates = zetta::solutions::solution_templates(solution.id);
    solution.runnable = true;
    for (const SolutionMethodology& entry : solution.composition) {
        const Methodology* method = find_methodology(entry.id);
        if (method == nullptr || !method->runnable || !method->build_shape || !method->run) {
            solution.missing_components.push_back(
                {entry.id, method == nullptr ? entry.id : method->name, issue(984)});
            if (entry.minimum_level == 1) {
                solution.runnable = false;
            }
        }
    }
    solution.status = "catalogue";
    solution.issue = issue(solution.id == "S-1" ? 1110
                           : solution.id == "S-2" ? 1111
                           : solution.id == "S-3" ? 1112
                           : solution.id == "S-4" ? 1113
                           : solution.id == "S-5" ? 1114
                           : solution.id == "S-6" ? 1115
                           : solution.id == "S-7" ? 1116
                           : solution.id == "S-8" ? 1117
                           : solution.id == "S-9" ? 1118
                           : solution.id == "S-10" ? 1119
                           : solution.id == "S-11" ? 1120
                           : solution.id == "S-12" ? 1121
                           : solution.id == "B-1" ? 1175
                           : solution.id == "B-2" ? 1176
                           : solution.id == "B-3" ? 1177
                           : solution.id == "B-4" ? 1178
                           : solution.id == "B-5" ? 1179
                                                   : 1102);
    return solution;
}

Solution sketch(std::string id, std::string name, std::string promise,
                std::string use_case,
                std::vector<SolutionMethodology> composition,
                std::vector<SolutionDataRole> required_data_roles,
                std::string promotion_condition) {
    Solution solution;
    solution.id = std::move(id);
    solution.name = std::move(name);
    solution.promise = std::move(promise);
    solution.use_case = std::move(use_case);
    solution.composition = std::move(composition);
    solution.required_data_roles = std::move(required_data_roles);
    solution.status = "sketch";
    solution.issue = issue(1102);
    solution.promotion_condition = std::move(promotion_condition);
    return solution;
}

Solution band_b(
    std::string id, std::string name, std::string promise,
    std::string use_case, std::vector<SolutionMethodology> composition,
    std::vector<std::string> pipeline_order,
    std::vector<std::string> action_patterns,
    std::vector<std::string> latency_classes,
    std::vector<SolutionAutonomyDefault> autonomy_defaults,
    std::vector<SolutionDataRole> required_data_roles,
    std::string decision_policy_template, std::string measurement_design,
    std::vector<std::string> guardrails,
    std::vector<SolutionCapabilityLevel> capability_levels,
    int issue_number, std::string promotion_condition) {
    Solution solution = band_a(
        std::move(id), std::move(name), std::move(promise),
        std::move(use_case), std::move(composition),
        std::move(pipeline_order), std::move(action_patterns),
        std::move(latency_classes), std::move(autonomy_defaults),
        std::move(required_data_roles),
        std::move(decision_policy_template),
        std::move(measurement_design), std::move(guardrails),
        std::move(capability_levels));
    solution.issue = issue(issue_number);
    solution.promotion_condition = std::move(promotion_condition);
    return solution;
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

nlohmann::json source_hints(const nlohmann::json& sources,
                            std::string_view methodology) {
    nlohmann::json hints = nlohmann::json::object();
    if (!sources.is_object()) {
        return hints;
    }
    for (const auto& [key, value] : sources.items()) {
        if (key != "common" && key != "correctness" &&
            !key.starts_with("M-")) {
            hints[key] = value;
        }
    }
    if (const auto common = sources.find("common");
        common != sources.end() && common->is_object()) {
        hints.update(*common);
    }
    if (const auto specific = sources.find(std::string(methodology));
        specific != sources.end() && specific->is_object()) {
        hints.update(*specific);
    }
    return hints;
}

std::vector<std::string> roots(const ir::ExecutionGraph& graph) {
    std::vector<std::string> result;
    for (const auto& node : graph.nodes()) {
        if (graph.edges_into(node->id).empty()) {
            result.push_back(node->id);
        }
    }
    return result;
}

std::vector<std::string> terminals(const ir::ExecutionGraph& graph) {
    std::vector<std::string> result;
    for (const auto& node : graph.nodes()) {
        if (graph.edges_from(node->id).empty()) {
            result.push_back(node->id);
        }
    }
    return result;
}

std::vector<std::string>
copy_stage(ir::ExecutionGraph& destination, const ir::ExecutionGraph& source,
           int stage, std::string_view methodology) {
    const std::string prefix = "solution_stage_" + std::to_string(stage) + "_";
    for (const auto& node : source.nodes()) {
        nlohmann::json attributes = node->attributes.is_object()
                                        ? node->attributes
                                        : nlohmann::json::object();
        attributes["methodology"] = methodology;
        attributes["solution_stage"] = stage;
        attributes["preview"] = false;
        destination.add_node(node->kind, node->name, std::move(attributes),
                             prefix + node->id);
    }
    for (const ir::Edge& edge : source.edges()) {
        destination.add_edge(prefix + edge.from, prefix + edge.to, edge.kind);
    }
    std::vector<std::string> copied_roots;
    for (const std::string& root : roots(source)) {
        copied_roots.push_back(prefix + root);
    }
    return copied_roots;
}

nlohmann::json composition_json(const Solution& solution) {
    nlohmann::json result = nlohmann::json::array();
    for (const SolutionMethodology& methodology : solution.composition) {
        result.push_back({{"methodology", methodology.id},
                          {"role", methodology.role},
                          {"minimum_level", methodology.minimum_level}});
    }
    return result;
}

} // namespace

const std::vector<Solution>& solutions() {
    static const std::vector<Solution> table = {
        band_a(
            "S-1", "Churn & Retention",
            "Find who may leave, explain why, and route the right retention work.",
            "Identify preventable churn early enough for a governed intervention.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-5", "attachable", 2},
             {"M-6", "attachable", 3}, {"M-8", "attachable", 4}},
            {"M-2", "M-1", "M-3", "M-5", "M-6", "M-8"},
            {"CRM tasks with owner, SLA, and top-3 reason codes",
             "capped retention journey triggers", "driver report publication"},
            {"L1 CRM tasks", "L1 journey triggers", "batch driver report"},
            {{"score and create CRM tasks", "A2"},
             {"trigger capped retention journeys", "A2"},
             {"concessions and playbook changes", "A1"},
             {"threshold tuning inside approved bounds", "A3"}},
            {{"entity", true, "customer, account, or household"},
             {"label", true, "churn event and outcome window"},
             {"time", true, "feature cutoff and label window"},
             {"feature", true, "point-in-time behavior, service, and billing drivers"},
             {"value", true, "customer value or an explicit value tier"},
             {"treatment", false, "retention action and no-contact cell"},
             {"owner", false, "account owner for CRM task routing"}},
            "Risk and treatment tiers with eligibility, contact caps, concession approvals, and a kill switch.",
            "Permanent holdout with randomized treatment cells and incremental retention read-back.",
            {"complaints", "discount budget", "contact fatigue", "action-execution rate"},
            levels("propensity, reasons, segments, holdout and report",
                   "add survival timing", "add uplift targeting",
                   "add treatment arbitration")),
        band_a(
            "S-2", "Lead & Opportunity Scoring",
            "Prioritize the leads and opportunities most worth timely sales attention.",
            "Concentrate finite sales capacity while preserving exploration and coverage.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-6", "attachable", 3}},
            {"M-2", "M-1", "M-3", "M-6"},
            {"CRM lead tasks with owners and SLAs", "capacity-aware sales and nurture queues"},
            {"L1 CRM queue writes", "optional L2 scoring read-back"},
            {{"queue writes and nurture assignment", "A2"}, {"priority overrides", "A1"},
             {"band and SLA policy changes", "A1"},
             {"exploration rate inside bounds", "A3"}},
            {{"entity", true, "lead, contact, account, and opportunity hierarchy"},
             {"label", true, "stage conversion in a signed window"},
             {"time", true, "point-in-time CRM field history"},
             {"feature", true, "pre-qualification activity and firmographic signals"},
             {"owner", true, "eligible sales owner or queue"},
             {"territory", true, "routing territory and eligibility"},
             {"capacity", true, "rep capacity available at decision time"},
             {"value", false, "opportunity value for level-4 routing evidence"},
             {"treatment", false, "worked, routed, nurtured, or no-contact action"}},
            "Score bands crossed with territory, rep capacity, SLAs, coverage caps, and an exploration quota.",
            "Randomized routing at matched capacity with a protected exploration slice.",
            {"SLA miss rate", "capacity violations", "routing coverage", "action-execution rate"},
            levels("scoring with queues and SLAs", "add opportunity risk",
                   "add worked-lead incrementality", "add value-weighted routing")),
        band_a(
            "S-3", "Customer Lifetime Value",
            "Predict customer value and govern audience and budget choices around it.",
            "Allocate acquisition, service, and retention spend against a stated future-value horizon rather than recent revenue.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-5", "attachable", 2},
             {"M-6", "attachable", 3}},
            {"M-2", "M-1", "M-3", "M-5", "M-6"},
            {"audience sync", "budget allocation", "audience and budget report"},
            {"L1 audience sync", "L1 allocation report"},
            {{"audience sync", "A2"}, {"budget allocation changes", "A1"},
             {"threshold tuning inside bounds", "A3"}},
            {{"entity", true, "stable customer or account counted once"},
             {"label", true, "net future value over the declared horizon"},
             {"time", true, "cohort and point-in-time observation cutoff"},
             {"feature", true, "pre-cutoff value, engagement, cost, and tenure inputs"},
             {"tenure", true, "customer age used to censor immature outcomes"},
             {"margin", true, "margin evidence for the policy floor"},
             {"consent", true, "audience consent and channel eligibility"},
             {"treatment", false, "campaign, service, or budget intervention"},
             {"owner", false, "audience or budget owner for routed actions"}},
            "Predicted-value bands with consent and channel eligibility, a margin floor, frequency and lifetime caps, a budget ceiling, and owner approval for budget changes.",
            "Permanent holdout in every value band with net value measured over the same stated horizon and compared against treated entities.",
            {"margin violations", "consent violations", "discount fairness",
             "budget utilization", "cohort calibration"},
            levels("value regression, segments, audience sync, holdout and report",
                   "add censoring-aware survival horizon",
                   "add causal audience and budget allocation",
                   "add governed cross-channel budget policy")),
        band_a(
            "S-4", "Collections Prioritization",
            "Prioritize fair, compliant collections work by expected net recovery.",
            "Match treatment and queue order to recoverability, cost, and customer protections.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-6", "attachable", 3}},
            {"M-2", "M-1", "M-3", "M-6"},
            {"policy-engine treatment selection", "digital treatment delivery",
             "dialer queue tasks"},
            {"L1 digital treatments", "L1 dialer queue tasks"},
            {{"digital treatments and dialer queueing", "A2"}, {"settlements", "A1"},
             {"thresholds inside approved bands", "A3"}},
            {{"entity", true, "account, customer, or household"},
             {"label", true, "point-in-time promise-to-pay and net recovery outcome"},
             {"time", true, "promise, delinquency, and contact-history cutoff"},
             {"feature", true, "point-in-time balance, aging, payment, and contact signals"},
             {"eligibility", true, "regulatory contact eligibility and permitted contact hours"},
             {"vulnerability", true, "explicit vulnerable-customer protection evidence"},
             {"contact", true, "prior contact counts and timestamps"},
             {"treatment", false, "contact channel, digital treatment, and offer"},
             {"owner", false, "collections team or queue owner"}},
            "Recovery tiers with contact-time rules, frequency caps, vulnerability protection, settlement approvals, and suppression.",
            "Champion/challenger treatment cells measured on net recovery after cost.",
            {"contact limits", "complaints", "vulnerable-customer contacts", "fairness", "net recovery"},
            levels("queue prioritization", "add treatment policy",
                   "add uplift selection", "add bounded digital treatment")),
        band_a(
            "S-5", "Demand Forecasting",
            "Forecast demand with honest uncertainty and surface the exceptions planners need to act on.",
            "Replace point forecasts and manual overrides with measured forecast value added.",
            {{"M-4", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-5", "attachable", 3}},
            {"M-2", "M-4", "M-3", "M-5"}, {"planner report", "exception queues"},
            {"L1", "L2"},
            {{"forecast publication", "A2"}, {"planning decisions", "A0"},
             {"exception thresholds", "A3"}},
            {{"time", true, "ordered demand history"}, {"label", true, "demand quantity"},
             {"group", false, "item, location, and hierarchy"},
             {"event", false, "promotions, stockouts, and calendar effects"}},
            "Forecast exceptions by hierarchy with override reasons, freshness limits, and planner ownership.",
            "Rolling forecast-value-added comparison against seasonal-naive and current-plan baselines.",
            {"bias", "service level", "stockout censoring", "override audit", "interval coverage"},
            levels("forecast and planner report", "add fresh exception queues",
                   "add event timing", "add scenario planning")),
        band_a(
            "S-6", "Replenishment & Inventory Action",
            "Turn demand signals into bounded replenishment decisions with an auditable plan.",
            "Balance availability, working capital, capacity, and spend constraints.",
            {{"M-4", "primary", 1}, {"M-16", "foundation", 1},
             {"M-5", "attachable", 3}},
            {"M-4", "M-16", "M-5"},
            {"draft purchase and transfer orders", "owned tasks in ERP",
             "bounded closed-loop release"},
            {"L1 ERP tasks", "bounded batch release"},
            {{"draft purchase or transfer orders", "A0"}, {"release inside policy", "A2"},
             {"policy changes", "A1"}},
            {{"entity", true, "item and location replenishment key"},
             {"time", true, "demand and inventory history"},
             {"label", true, "demand quantity"},
             {"variable", true, "order and transfer decisions"},
             {"constraint", true, "capacity, lead time, budget, and service bounds"},
             {"cost", true, "unit, order, and working-capital cost"},
             {"owner", false, "planner or buyer accountable for the ERP task"}},
            "Order proposals constrained by service targets, lead times, capacity, spend caps, and segregation of duties.",
            "Protected holdout inside a phased release, with acceptance, edit distance, service, and inventory outcomes.",
            {"spend cap", "service level", "inventory exposure", "order volatility", "segregation of duties"},
            levels("forecast plus draft plan", "add ERP tasks",
                   "add time-to-stockout", "add bounded automatic release")),
        band_a(
            "S-7", "KPI Anomaly & Driver Analysis",
            "Detect meaningful KPI changes, explain likely drivers, and carry decisions into a governed forum.",
            "Reduce alert noise while preserving the evidence and decision log behind each escalation.",
            {{"M-7", "primary", 1}, {"M-1", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-4", "attachable", 3}},
            {"M-7", "M-1", "M-3", "M-4"},
            {"decision-forum report with anomaly evidence",
             "durable forum decision log"},
            {"L1 decision-forum webhook", "L2 batch report read-back"},
            {{"detection and report drafting", "A2"},
             {"executive escalation", "A1"},
             {"business decision", "A0"},
             {"alert-budget tuning inside approved bounds", "A3"}},
            {{"feature", true, "KPI measures and candidate drivers"},
             {"time", true, "ordered KPI and reporting periods"},
             {"entity", true, "observation, business unit, or segment key"},
             {"owner", true, "decision-forum owner"},
             {"permission", true, "sensitive-driver access evidence"},
             {"label", false, "confirmed anomaly for evaluation only"}},
            "Exact alert budgets and severity tiers with owner routing, sensitive-driver access, weekly caps, executive approval, and a kill switch.",
            "Phased decision-quality tracking: adoption, closure, and confirmed-alert precision are descriptive until a module-specific estimator ships.",
            {"alert volume", "false escalation rate",
             "sensitive-driver access violations", "causal-claim violations",
             "decision closure rate", "action-execution rate"},
            levels("seasonal anomaly report and protected forum routing",
                   "add candidate-driver forum and decision log",
                   "add rolling-origin forecast context",
                   "add cross-module executive review")),
        band_a(
            "S-8", "Predictive Maintenance",
            "Detect asset risk early and route evidence-backed maintenance work before failure.",
            "Trade planned intervention against failure risk without exceeding safe operating authority.",
            {{"M-5", "primary", 1}, {"M-1", "foundation", 1},
             {"M-2", "foundation", 1}, {"M-7", "attachable", 3}},
            {"M-2", "M-1", "M-5", "M-7"},
            {"CMMS work orders with asset, owner, SLA, risk, and time-to-failure",
             "governed inspection and scheduling tasks"},
            {"L3 sensor detection", "L1 CMMS tasks"},
            {{"draft work orders", "A0"},
             {"inspection work orders for approved asset classes", "A2"},
             {"scheduled work orders", "A1"},
             {"threshold tuning inside approved bounds", "A3"}},
            {{"entity", true, "stable asset identity and hierarchy"},
             {"time", true, "sensor, failure, and maintenance chronology"},
             {"label", true, "observed failure event indicator"},
             {"duration", true, "time from prediction cutoff to failure or censoring"},
             {"feature_time", true, "point-in-time sensor observation cutoff"},
             {"feature", true, "condition, usage, and environment signals"},
             {"asset_class", true, "approved class and safety policy scope"},
             {"owner", true, "maintenance team or CMMS queue"},
             {"eligibility", true, "maintenance window, safety, and capacity evidence"}},
            "Failure-risk tiers crossed with asset-class approval, maintenance windows, safety exclusions, work-order caps, approval rules, and a kill switch.",
            "Permanent holdout within each risk tier, measuring failure avoidance after work-order assignment.",
            {"unsafe work orders", "false work orders", "missed failures",
             "sensor drift", "maintenance capacity", "action-execution rate"},
            levels("failure risk, time-to-failure, holdout, and CMMS work orders",
                   "add asset-class maintenance-window policy",
                   "add sensor anomaly context",
                   "add approval-gated scheduling")),
        band_a(
            "S-9", "Automated Outbound",
            "Select incremental audiences and run governed outbound journeys without exhausting customers.",
            "Coordinate audience, channel, pacing, and message policy while measuring response against protected holdouts.",
            {{"M-1", "primary", 1}, {"M-3", "foundation", 1},
             {"M-2", "foundation", 1}, {"M-6", "attachable", 2},
             {"M-8", "attachable", 3}, {"M-10", "attachable", 4}},
            {"M-2", "M-1", "M-3", "M-6", "M-8", "M-10"},
            {"capped outbound journey triggers", "copy-review tasks"},
            {"L1 journey triggers", "L1 copy-review tasks"},
            {{"send approved journeys inside caps", "A2"}, {"audiences and copy", "A1"},
             {"pacing inside bounds", "A3"}},
            {{"entity", true, "resolved customer or account"},
             {"label", true, "response observed in a fully closed window"},
             {"time", true, "eligibility, contact history, and response cutoff"},
             {"feature", true, "point-in-time engagement and audience attributes"},
             {"consent", true, "current consent for the selected channel"},
             {"suppression", true, "suppression and unsubscribe state at decision time"},
             {"contact", true, "lifetime and rolling-window contact counts"},
             {"channel", true, "eligible channel and journey routing evidence"},
             {"treatment", false, "journey, channel, and no-contact arm for uplift levels"}},
            "Journey eligibility, consent and suppression, contact caps, channel budgets, approvals, and no-contact arms.",
            "Send/hold holdouts per cell with incremental response and downstream value measurement.",
            {"unsubscribe rate", "complaint rate", "consent violations",
             "contact frequency", "action-execution rate"},
            levels("response propensity, audiences, capped journeys and holdout",
                   "add positive-uplift audience selection",
                   "add eligible journey arbitration",
                   "add governed, grounded copy assistance")),
        band_a(
            "S-10", "Fraud & Financial-Crime Decisioning",
            "Make bounded request-time fraud decisions and route evidence-rich cases for review.",
            "Reduce loss and false declines together while keeping every decision explainable.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-12", "foundation", 1}, {"M-5", "attachable", 3}},
            {"M-2", "M-12", "M-1", "M-5"},
            {"request-time authorization decisions", "case-management tasks"},
            {"L4 authorization decision", "L1 case triage"},
            {{"approve, challenge, and review decisions", "A2"},
             {"blocks, emergency rules, and promotions", "A1"},
             {"thresholds inside approved bands", "A3"}},
            {{"entity", true, "transaction or authorization request"},
             {"label", true, "matured fraud, dispute, or case disposition"},
             {"time", true, "request-time and label-maturation chronology"},
             {"feature", true, "request-time transaction, device, and behavioral signals"},
             {"group", true, "account, device, merchant, and counterparty edges"},
             {"propensity", true, "logged authorization or exploration propensity"},
             {"owner", true, "case queue or investigation owner"}},
            "Approve, challenge, review, and block bands with friction budgets, fallback rules, approvals, and expiry.",
            "Continuous champion/challenger with a protected exploration slice and below-the-line audits.",
            {"fraud loss", "approval rate", "challenge rate", "review load", "fairness"},
            levels("post-event review priority", "add request-time bands",
                   "add graph and account-takeover context", "add financial-crime case assistance")),
        band_a(
            "S-11", "Next-Best-Action",
            "Choose the best eligible action, including no action, consistently across channels.",
            "Arbitrate competing offers for long-term value inside shared customer constraints.",
            {{"M-8", "primary", 1}, {"M-1", "foundation", 1},
             {"M-6", "foundation", 1}, {"M-2", "foundation", 1},
             {"M-15", "foundation", 1}},
            {"M-2", "M-1", "M-6", "M-8", "M-15"},
            {"request-time decision API across channels",
             "approval task for high-stakes actions"},
            {"L4 request-time decisions", "L1 approval tasks"},
            {{"arbitration inside policy", "A2"},
             {"action catalog and high-stakes offers", "A1"},
             {"exploration inside bounds", "A3"}},
            {{"entity", true, "one customer identity across channels"},
             {"treatment", true, "eligible actions and no-action arm"},
             {"label", true, "per-decision and long-horizon outcomes"},
             {"time", true, "decision chronology and outcome maturity"},
             {"feature", true, "decision-time context available in every channel"},
             {"consent", true, "channel consent and suppression evidence"},
             {"contact", true, "cross-channel contact counts and caps"},
             {"propensity", true, "actual logged action-selection probability"},
             {"channel", true, "channel and delivery destination"}},
            "Eligibility, consent, cross-channel caps, budgets, margin floors, business overrides, and exploration floor.",
            "Global holdout plus uplift cells, per-arm propensity logging, off-policy evaluation, and long-horizon value.",
            {"consent violations", "contact-cap violations", "missing propensity logs",
             "margin floor", "action-execution rate"},
            levels("single-channel batch lists", "real-time single channel",
                   "omnichannel arbitration", "add long-horizon objectives")),
        band_a(
            "S-12", "Claims Triage & Fraud Referral",
            "Put each claim on the right path and refer suspicious cases with traceable evidence.",
            "Reduce cycle time and leakage while keeping denial decisions with people.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-11", "foundation", 1}, {"M-3", "attachable", 3}},
            {"M-2", "M-11", "M-1", "M-3"}, {"path assignment", "referral tasks"},
            {"L3", "L1"},
            {{"payment-direction paths", "A2"}, {"denial direction", "A0"},
             {"referral thresholds inside bounds", "A3"}},
            {{"entity", true, "claimant, provider, shop, policy, and claim"},
             {"label", true, "claim development, referral, and audit outcome"},
             {"document", true, "first-notice documents and evidence"},
             {"time", true, "claim-state cutoff and development chronology"}},
            "Path allowlists, amount caps, coverage and identity checks, SIU capacity, catastrophe overrides, and appeal paths.",
            "Leakage audits, random referral samples, referral hit rate, and phased comparison by line.",
            {"no automated denial", "overpayment", "false referral", "complaints", "field extraction accuracy"},
            levels("SIU queue ranking", "add triage with human release",
                   "add instant pay inside allowlists", "add photo and subrogation assistance")),
        band_a(
            "B-1", "Dynamic Pricing & Discount Governance",
            "Set prices or discounts inside explicit margin and authority bands.",
            "Govern price changes despite policy-confounded historical demand.",
            {{"M-4", "primary", 1}, {"M-6", "foundation", 1},
             {"M-16", "foundation", 1}},
            {"M-4", "M-6", "M-16"},
            {"pricing recommendations", "pricing review tasks",
             "controlled price experiments", "approved price publication"},
            {"L1 pricing recommendation", "L1 review and experiment task"},
            {{"recommendations", "A0"},
             {"review tasks and experiments inside authority bands", "A2"},
             {"price changes above the automatic authority band", "A1"}},
            {{"entity", true, "product, customer, and market"},
             {"treatment", true, "randomized price or discount offered"},
             {"label", true, "demand, margin, and customer outcome"},
             {"time", true, "offer, competitor observation, and outcome chronology"},
             {"price", true, "current and recommended prices at decision time"},
             {"margin", true, "gross margin and configured margin floor"},
             {"authority", true, "automatic and hard price-change bands"}},
            "Eligibility, margin floor, automatic and hard authority bands, approvals, frequency caps, and kill switch.",
            "Randomized or otherwise identifiable price variation with per-tier holdouts and net-margin measurement.",
            {"margin-floor violations", "authority-band violations",
             "unapproved price changes", "price-feature leakage",
             "action-execution rate"},
            levels("forecast and governed recommendation",
                   "add human pricing review tasks",
                   "add controlled price experiments",
                   "add approved price publication inside authority bands"),
            "Promote when a target account has identifiable price variation or permits controlled price experiments."),
        band_a(
            "B-2", "Employee Attrition (aggregate-first)",
            "Explain aggregate workforce risks without flagging individuals to managers.",
            "Support organizational interventions with small-group and proxy protections.",
            {{"M-1", "primary", 1}, {"M-7", "foundation", 1}},
            {"M-1", "M-7"},
            {"people-leader aggregate risk report",
             "approval-gated organizational intervention review"},
            {"L1 people-leader report", "batch organizational review"},
            {{"publish aggregate workforce risk", "A2"},
             {"organizational intervention", "A1"},
             {"individual employment decision", "A0"},
             {"aggregate threshold tuning inside approved bounds", "A3"}},
            {{"group", true, "team, role, or tenure band with a stable aggregate key"},
             {"group_size", true, "cohort count proving the configured minimum size"},
             {"label", true, "aggregate attrition outcome"},
             {"time", true, "workforce snapshot and outcome window"},
             {"feature", true, "aggregate workforce measures and drivers"},
             {"owner", true, "authorized people leader receiving aggregate output"},
             {"approval", true, "recorded legal or works-council approval"}},
            "Aggregate risk tiers with minimum group size, people-leader-only delivery, review approvals, report caps, and a kill switch.",
            "Permanent group holdout measuring aggregate attrition change over a fixed outcome window.",
            {"small-group outputs", "individual-score outputs",
             "fairness-review failures", "approval failures",
             "action-execution rate"},
            levels("aggregate team risk and drivers",
                   "add role-band comparison",
                   "add tenure-band anomaly context",
                   "add approval-gated intervention review"),
            "Promote with a sufficiently large workforce, a named people leader, and legal or works-council approval."),
        band_a(
            "B-3", "Ticket Triage & Service Automation",
            "Route service work with explicit abstention and measured resolution quality.",
            "Reduce misroutes and handling effort without mistaking containment for resolution.",
            {{"M-10", "primary", 1}, {"M-19", "foundation", 1}},
            {"M-10", "M-19"},
            {"customer-taxonomy queue tasks", "grounded draft-reply tasks",
             "approval-gated ticket closure"},
            {"L1 service queue tasks", "L1 draft-review tasks"},
            {{"route tickets inside the customer taxonomy", "A2"},
             {"draft replies for review", "A2"},
             {"close tickets", "A1"},
             {"confidence tuning inside approved bounds", "A3"}},
            {{"document", true, "ticket text and interaction history"},
             {"label", true, "routing and resolved outcome"},
             {"entity", true, "stable ticket identifier"},
             {"taxonomy", true, "customer-owned queue taxonomy and version"},
             {"owner", true, "service queue owner"},
             {"resolution", true, "resolution-quality outcome"},
             {"feature", true, "numeric signals for label-efficient review"}},
            "Confidence tiers with explicit abstention, customer-owned queue eligibility, handling caps, reply review, approval-gated closure, and a kill switch.",
            "Permanent holdout by confidence tier with resolution quality measured after routing and reply assistance.",
            {"misroute rate", "resolution quality", "abstention rate",
             "new ticket-type drift", "unapproved automatic closure",
             "action-execution rate"},
            levels("confidence routing with abstention and holdout",
                   "add grounded draft replies",
                   "add label-efficient taxonomy review",
                   "add bounded service automation"),
            "Promote when ticket volume is high enough and the service organization owns or funds its taxonomy."),
        band_a(
            "B-4", "Document & Workflow Automation",
            "Extract and validate documents while routing uncertain fields to review.",
            "Automate high-volume document lanes with deterministic checks and audit samples.",
            {{"M-11", "primary", 1}},
            {"M-11"},
            {"field-level human review tasks",
             "validated structured-data workflow handoff"},
            {"L1 review tasks", "L1 workflow handoff"},
            {{"route uncertain fields to human review", "A2"},
             {"handoff validated structured output", "A2"},
             {"high-impact workflow release", "A1"},
             {"confidence thresholds inside approved bounds", "A3"}},
            {{"document", true, "source document or page image"},
             {"field", true, "declared fields and a confidence threshold for each"},
             {"label", true, "verified fields and workflow outcome"},
             {"entity", true, "stable document, case, customer, or transaction key"},
             {"time", true, "document arrival and workflow-outcome cutoff"},
             {"owner", true, "human-review queue or workflow owner"},
             {"eligibility", true, "workflow eligibility and suppression evidence"}},
            "Per-field confidence thresholds, mandatory human review for uncertain fields, workflow eligibility, frequency caps, approval for high-impact release, and a kill switch.",
            "Permanent holdout by validation tier, measuring completed workflows over the declared outcome window.",
            {"unvalidated workflow actions", "review queue misses",
             "field extraction accuracy", "field validation rate",
             "action-execution rate"},
            levels("extract fields, retain evidence, and route review tasks",
                   "add owned validation and release tasks",
                   "add validated structured-data workflow handoff",
                   "add approval-gated high-impact workflow release"),
            "Promote for a high-volume document lane or repeated demand from multiple deployed modules."),
        band_a(
            "B-5", "Supplier Risk & Procurement Intelligence",
            "Prioritize supplier risks and procurement actions with evidence.",
            "Reduce disruption exposure despite sparse, selected supplier outcomes.",
            {{"M-1", "primary", 1}, {"M-4", "foundation", 1}},
            {"M-4", "M-1"},
            {"supplier watch and procurement review tasks",
             "approval-bounded mitigation tasks"},
            {"L1 procurement tasks", "batch supplier-risk read-back"},
            {{"publish evidence-backed supplier flags", "A2"},
             {"route procurement review work", "A2"},
             {"approve supplier mitigation spend", "A1"},
             {"tune risk thresholds inside approved bounds", "A3"}},
            {{"entity", true, "supplier, part, site, and contract"},
             {"label", true, "disruption or performance outcome"},
             {"time", true, "orders, deliveries, external publications, and risk signals"},
             {"feature", true, "pre-event delivery, quality, concentration, and external-risk evidence"},
             {"owner", true, "buyer or procurement team accountable for review"},
             {"evidence", true, "source, publication cutoff, reason codes, and forecast context"},
             {"cost", false, "mitigation cost used for budget and approval limits"}},
            "Risk tiers crossed with supplier eligibility, evidence completeness, task frequency, mitigation budget, approvals, and a kill switch.",
            "Permanent holdout in every risk tier, measuring disruption avoidance after procurement action.",
            {"missing flag evidence", "stale external data",
             "single-source exposure", "mitigation budget",
             "action-execution rate"},
             levels("supplier risk flags with source and forecast evidence",
                    "add owned procurement review tasks",
                    "add approval-bounded mitigation tasks",
                    "add bounded procurement action routing"),
             "Promote with concentrated manufacturing demand and reusable external-data support across several modules."),
        band_b(
            "B-6", "Healthcare Operations (no-show & denials)",
            "Prioritize assistive scheduling and revenue-cycle work with equity controls.",
            "Recover operational capacity without turning predictions into access penalties.",
            {{"M-1", "primary", 1}}, {"M-1"},
            {"assistive scheduling tasks", "revenue-cycle review tasks"},
            {"L1 owned work queues", "L2 batch operational read-back"},
            {{"scheduling assistance and reminders", "A2"},
             {"revenue-cycle review tasks", "A2"},
             {"expanded assistance or policy exceptions", "A1"},
             {"care access and clinical decisions", "A0"}},
            {{"entity", true, "patient, appointment, claim, or operational case"},
             {"label", true, "attendance or claim-denial outcome"},
             {"time", true, "scheduling and claim chronology"},
             {"feature", true, "minimum-necessary operational predictors"},
             {"workflow", true, "scheduling or revenue-cycle work type"},
             {"protected_group", true, "evaluation-only equity cohorts"},
             {"privacy", true, "approved privacy posture and minimum-necessary review"},
             {"owner", true, "care coordination or revenue-cycle owner"}},
            "Operational-risk tiers with assistive-only actions, eligibility, weekly and lifetime caps, privacy and equity evidence, approval for exceptions, and a kill switch.",
            "Permanent holdout in every risk tier with operational resolution measured across protected groups; care access is never an outcome or action.",
            {"care-denial actions", "equity rate gap", "PHI scope violations",
             "privacy posture failures", "action-execution rate"},
            levels("risk scoring, protected holdout, and owned assistive tasks",
                   "add scheduling and revenue-cycle work queues",
                   "add protected-group operating review",
                   "add bounded cross-workflow orchestration"),
            1180,
            "Promote with a provider design customer that owns revenue-cycle operations and has an agreed privacy posture."),
        band_b(
            "B-7", "Credit Underwriting & Limit Management",
            "Support credit policy with examiner-readable evidence and adverse-action discipline.",
            "Balance loss, approval, and fairness under regulated model-risk controls.",
            {{"M-1", "primary", 1}, {"M-17", "foundation", 1}},
            {"M-1", "M-17"},
            {"underwriter review tasks", "credit and limit decisions with evidence"},
            {"L1 underwriting tasks", "batch decision and validation read-back"},
            {{"credit approval inside approved caps", "A2"},
             {"underwriter review routing", "A2"},
             {"limit increases", "A1"},
             {"declines and adverse limit changes", "A0"}},
            {{"entity", true, "applicant, account, and facility"},
             {"label", true, "matured repayment outcome"},
             {"time", true, "application cutoff and performance window"},
             {"feature", true, "verified point-in-time application and bureau inputs"},
             {"group", true, "portfolio group used for uncertainty validation"},
             {"eligibility", true, "identity, income, product, and policy eligibility"},
             {"protected_cohort", true, "evaluation-only fair-lending cohorts"},
             {"reason", true, "ranked adverse-action reason codes"},
             {"owner", true, "accountable underwriter or credit team"}},
            "Risk and limit tiers with verified eligibility, amount caps, A1 approvals, A0 adverse decisions, and a kill switch.",
            "Permanent holdout measured on performing-account outcomes, with disjoint M-17 coverage validation.",
            {"automated adverse decisions", "missing adverse-action reasons",
             "model-risk validation", "fairness review", "approval-rate gap",
             "action-execution rate"},
            levels("credit risk, reasons, uncertainty, holdout and review tasks",
                   "add governed limit review", "add uncertainty-aware escalation",
                   "add portfolio limit management"),
            1181,
            "Promote with a lender whose model-risk team co-signs and exercises the full validation pipeline."),
        band_b(
            "B-8", "IT & Security Operations",
            "Prioritize operational and security risks with graduated, bounded authority.",
            "Reduce alert load and exposure while constraining automated remediation.",
            {{"M-7", "primary", 1}, {"M-12", "foundation", 1}},
            {"M-12", "M-7"},
            {"owned investigation recommendations",
             "approval-gated containment tasks",
             "bounded containment actions"},
            {"L1 investigation queue", "L1 containment action"},
            {{"investigation recommendations", "A0"},
             {"containment approval", "A1"},
             {"single-asset containment inside an approved playbook", "A2"},
             {"policy and playbook changes", "A1"}},
            {{"entity", true, "asset, identity, service, and dependency"},
             {"time", true, "event, relationship, and telemetry chronology"},
             {"feature", true, "numeric telemetry, edge weight, and risk evidence"},
             {"group", true, "dependency or identity relationship destination"},
             {"owner", true, "operations or security response owner"},
             {"eligibility", true, "approved playbook and containment scope evidence"},
             {"label", false, "confirmed incident or operational impact"}},
            "Severity tiers with an exact alert budget, A0 investigation, A1 approval, single-asset A2 containment, explicit eligibility, and a kill switch.",
            "Permanent holdout in every severity tier with incident avoidance measured after the response window.",
            {"alert volume", "containment scope", "unapproved containment",
             "kill-switch bypass", "action-execution rate"},
            levels("prioritized alerts and A0 investigation recommendations",
                   "add A1 containment approval tasks",
                   "add single-asset A2 containment inside an approved playbook",
                   "add dependency-aware bounded containment with the same authority ceiling"),
            1182,
            "Promote as an attach-on after multiple modules prove the shared governed-operations pattern."),
    };
    return table;
}

const Solution* find_solution(std::string_view id) {
    const std::string wanted = lower(id);
    for (const Solution& solution : solutions()) {
        if (lower(solution.id) == wanted) {
            return &solution;
        }
    }
    return nullptr;
}

nlohmann::json solution_json(const Solution& solution) {
    nlohmann::json guardrails = nlohmann::json::array();
    for (const auto& guardrail : solution.templates.guardrails) {
        guardrails.push_back(zetta::solutions::to_json(guardrail));
    }
    nlohmann::json autonomy = nlohmann::json::array();
    for (const SolutionAutonomyDefault& entry : solution.autonomy_defaults) {
        autonomy.push_back({{"action_class", entry.action_class},
                            {"autonomy", entry.autonomy}});
    }
    nlohmann::json data_roles = nlohmann::json::array();
    for (const SolutionDataRole& role : solution.required_data_roles) {
        data_roles.push_back({{"role", role.role},
                              {"required", role.required},
                              {"description", role.description}});
    }
    nlohmann::json levels = nlohmann::json::array();
    for (const SolutionCapabilityLevel& level : solution.capability_levels) {
        levels.push_back({{"level", level.level},
                          {"name", level.name},
                          {"description", level.description}});
    }
    nlohmann::json missing = nlohmann::json::array();
    for (const MissingSolutionComponent& component : solution.missing_components) {
        missing.push_back({{"id", component.id},
                           {"name", component.name},
                           {"issue", component.issue}});
    }
    return {{"id", solution.id},
            {"name", solution.name},
            {"promise", solution.promise},
            {"use_case", solution.use_case},
            {"composition", composition_json(solution)},
            {"pipeline_order", solution.pipeline_order},
            {"action_patterns", solution.action_patterns},
            {"latency_classes", solution.latency_classes},
            {"autonomy_defaults", std::move(autonomy)},
            {"required_data_roles", std::move(data_roles)},
            {"decision_policy_template", solution.decision_policy_template},
            {"measurement_design", solution.measurement_design},
            {"policy_template", solution.templates.policy},
            {"experiment_template", zetta::solutions::to_json(solution.templates.experiment)},
            {"measurement", zetta::solutions::to_json(solution.templates.measurement)},
            {"minimal_template", solution.templates.minimal},
            {"guardrails", std::move(guardrails)},
            {"levels", std::move(levels)},
            {"missing_components", std::move(missing)},
            {"status", solution.status},
            {"runnable", solution.runnable},
            {"issue", solution.issue},
            {"promotion_condition", solution.promotion_condition}};
}

ComposedSolution compose_solution(const Solution& solution, int level,
                                  const nlohmann::json& sources) {
    ComposedSolution result;
    if (solution.status == "sketch") {
        result.error = solution.name + " (" + solution.id +
                       ") is a sketch and cannot be created. Promotion condition: " +
                       solution.promotion_condition + ". " + solution.issue;
        return result;
    }
    if (level < 1 || level > static_cast<int>(solution.capability_levels.size())) {
        result.error = "level must select one of " + solution.id +
                       "'s declared capability levels";
        return result;
    }
    std::vector<std::string> selected;
    for (const SolutionMethodology& entry : solution.composition) {
        if (entry.minimum_level <= level) {
            selected.push_back(entry.id);
        }
    }

    result.graph.add_node(
        ir::StepKind::Connect, "Solution execution manifest",
        {{"solution", solution.id},
         {"level", level},
         {"preview", false},
         {"methodologies", selected},
         {"sources", sources}},
        "solution_manifest");
    result.graph.add_node(
        ir::StepKind::Clean, "Read point-in-time snapshots",
        {{"foundation_component", "snapshots"},
         {"runnable", true},
         {"issue", issue(1103)}},
        "solution_snapshots");
    result.graph.add_edge("solution_manifest", "solution_snapshots",
                          ir::EdgeKind::Control);
    std::vector<std::string> previous = {"solution_snapshots"};
    int stage = 1;
    for (const std::string& methodology_id : solution.pipeline_order) {
        if (std::ranges::find(selected, methodology_id) == selected.end()) {
            continue;
        }
        const Methodology* methodology = find_methodology(methodology_id);
        if (methodology == nullptr || !methodology->runnable ||
            !methodology->build_shape || !methodology->run) {
            result.error = methodology == nullptr
                               ? "no methodology registered for " + methodology_id
                               : unsupported_methodology_message(*methodology);
            return result;
        }
        ShapeRequest request;
        request.solution = solution.id;
        request.hints = source_hints(sources, methodology_id);
        const ir::ExecutionGraph stage_graph = methodology->build_shape(request);
        if (stage_graph.empty()) {
            result.error = methodology->name + " (" + methodology->id +
                           ") produced an empty pipeline";
            return result;
        }
        const std::vector<std::string> stage_roots =
            copy_stage(result.graph, stage_graph, stage, methodology_id);
        for (const std::string& from : previous) {
            for (const std::string& to : stage_roots) {
                result.graph.add_edge(from, to, ir::EdgeKind::Control);
            }
        }
        previous.clear();
        const std::string prefix =
            "solution_stage_" + std::to_string(stage) + "_";
        for (const std::string& terminal : terminals(stage_graph)) {
            previous.push_back(prefix + terminal);
        }
        result.methodologies.push_back(methodology_id);
        ++stage;
    }

    const std::vector<MissingSolutionComponent> after_model = {
        {"policy", "Apply decision policy", issue(1104)},
        {"holdout", "Maintain holdout cells", issue(1105)},
        {"actions", "Dispatch governed actions", issue(1109)},
        {"ledger", "Record score, action, and outcome", issue(1107)},
        {"monitor", "Monitor drift and guardrails", issue(1106)},
        {"readout", "Publish measured-value readout", issue(1108)},
    };
    int component_index = 0;
    for (const MissingSolutionComponent& component : after_model) {
        const std::string node_id =
            "solution_foundation_" + std::to_string(component_index++);
        const ir::StepKind kind = component.id == "policy" ? ir::StepKind::Serve
                                  : component.id == "monitor" ? ir::StepKind::Clean
                                                               : ir::StepKind::Act;
        result.graph.add_node(
            kind, component.name,
            {{"foundation_component", component.id},
             {"runnable", true},
             {"issue", component.issue}},
            node_id);
        for (const std::string& from : previous) {
            result.graph.add_edge(from, node_id, ir::EdgeKind::Control);
        }
        previous = {node_id};
    }
    if (const auto manifest = result.graph.find_node("solution_manifest")) {
        manifest->attributes["methodologies"] = result.methodologies;
    }
    result.missing_components = solution.missing_components;
    return result;
}

ir::ExecutionGraph solution_stage_graph(const ir::ExecutionGraph& graph,
                                        int stage) {
    ir::ExecutionGraph result;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    const bool strips_composition_annotations =
        manifest &&
        manifest->attributes.value("solution", std::string{}) == "S-6";
    for (const auto& node : graph.nodes()) {
        if (!node->attributes.is_object() ||
            node->attributes.value("solution_stage", 0) != stage) {
            continue;
        }
        nlohmann::json attributes = node->attributes;
        attributes.erase("methodology");
        attributes.erase("solution_stage");
        attributes.erase("preview");
        if (strips_composition_annotations) {
            attributes.erase("source_kind");
            attributes.erase("snapshot_as_of");
            attributes.erase("solution_snapshot_as_of");
        }
        result.add_node(node->kind, node->name, std::move(attributes),
                        node->id);
    }
    for (const ir::Edge& edge : graph.edges()) {
        if (result.has_node(edge.from) && result.has_node(edge.to)) {
            result.add_edge(edge.from, edge.to, edge.kind);
        }
    }
    return result;
}

} // namespace zetta::ml
