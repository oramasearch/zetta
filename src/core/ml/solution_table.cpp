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
    std::vector<SolutionCapabilityLevel> capability_levels) {
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
    solution.issue = issue(solution.id == "S-1" ? 1110 : 1102);
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
            {"M-2", "M-1", "M-3", "M-6"}, {"tasks and queues with SLAs"},
            {"L1", "optional L2"},
            {{"queue writes and nurture assignment", "A2"}, {"band and SLA changes", "A1"},
             {"exploration rate inside bounds", "A3"}},
            {{"entity", true, "lead, contact, account, and opportunity hierarchy"},
             {"label", true, "stage conversion in a signed window"},
             {"time", true, "point-in-time CRM field history"},
             {"treatment", false, "worked, routed, or nurtured action"}},
            "Score bands crossed with territory, rep capacity, SLAs, coverage caps, and an exploration quota.",
            "Randomized routing at matched capacity with a protected exploration slice.",
            {"response SLA", "new-segment coverage", "rep cherry-picking", "speed to lead"},
            levels("scoring with queues and SLAs", "add opportunity risk",
                   "add worked-lead incrementality", "add value-weighted routing")),
        band_a(
            "S-3", "Customer Lifetime Value",
            "Forecast customer value and govern audience and budget choices around it.",
            "Allocate acquisition, service, and retention spend against future value rather than recent revenue.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-5", "attachable", 2},
             {"M-6", "attachable", 3}},
            {"M-2", "M-1", "M-3", "M-5", "M-6"},
            {"policy engine", "audience and budget report"}, {"L1"},
            {{"audience sync", "A2"}, {"budget and discount changes", "A1"},
             {"threshold tuning inside bounds", "A3"}},
            {{"entity", true, "customer or account"}, {"label", true, "future value components"},
             {"time", true, "cohort and observation cutoff"},
             {"treatment", false, "campaign or service intervention"}},
            "Value bands with channel eligibility, budget ceilings, margin floors, consent, and discount approvals.",
            "Geographic or audience holdouts with cohort calibration and incremental value read-back.",
            {"margin", "consent", "discount fairness", "budget adherence", "cohort calibration"},
            levels("value scoring and reporting", "add survival horizon",
                   "add causal allocation", "add cross-channel budget policy")),
        band_a(
            "S-4", "Collections Prioritization",
            "Prioritize fair, compliant collections work by expected net recovery.",
            "Match treatment and queue order to recoverability, cost, and customer protections.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-3", "foundation", 1}, {"M-6", "attachable", 3}},
            {"M-2", "M-1", "M-3", "M-6"}, {"policy engine", "dialer queues"},
            {"L1"},
            {{"digital treatments and queueing", "A2"}, {"settlements", "A1"},
             {"thresholds inside approved bands", "A3"}},
            {{"entity", true, "account, customer, or household"},
             {"label", true, "payment and recovery outcome"},
             {"time", true, "delinquency and contact history cutoff"},
             {"treatment", false, "contact channel and offer"}},
            "Recovery tiers with contact-time rules, frequency caps, hardship eligibility, settlement approvals, and suppression.",
            "Champion/challenger treatment cells measured on net recovery after cost.",
            {"contact limits", "complaints", "hardship protections", "fairness", "net recovery"},
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
            {"M-4", "M-16", "M-5"}, {"tasks in ERP", "bounded closed loop"}, {"L1"},
            {{"draft purchase or transfer orders", "A0"}, {"release inside policy", "A2"},
             {"policy changes", "A1"}},
            {{"time", true, "demand and inventory history"}, {"label", true, "demand quantity"},
             {"variable", true, "order and transfer decisions"},
             {"constraint", true, "capacity, lead time, budget, and service bounds"}},
            "Order proposals constrained by service targets, lead times, capacity, spend caps, and segregation of duties.",
            "Phased release comparison with acceptance rate, edit distance, service, and inventory outcomes.",
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
            {"report to decision forum", "decision log"}, {"L1", "L2"},
            {{"detection and report drafting", "A2"}, {"business decision", "A0"},
             {"alert-budget tuning", "A3"}},
            {{"feature", true, "KPI measures and candidate drivers"},
             {"time", true, "event and reporting periods"}, {"entity", false, "business unit or segment"},
             {"label", false, "confirmed anomaly for evaluation only"}},
            "Alert budgets, severity tiers, owner routing, sensitive-driver access, and documented decision outcomes.",
            "Decision adoption and forecast-honesty tracking with confirmed-alert precision.",
            {"alert volume", "false escalation", "sensitive-driver access", "causal caveats", "decision closure"},
            levels("anomaly report", "add driver forum and log",
                   "add forecast context", "add cross-module executive view")),
        band_a(
            "S-8", "Predictive Maintenance",
            "Detect asset risk early and route evidence-backed maintenance work before failure.",
            "Trade planned intervention against failure risk without exceeding safe operating authority.",
            {{"M-5", "primary", 1}, {"M-1", "foundation", 1},
             {"M-2", "foundation", 1}, {"M-7", "attachable", 3}},
            {"M-2", "M-1", "M-5", "M-7"}, {"work orders in maintenance system"},
            {"L3 detect", "L1"},
            {{"draft work orders", "A0"}, {"approved asset classes", "A2"},
             {"threshold tuning", "A3"}},
            {{"entity", true, "asset identity and hierarchy"}, {"time", true, "sensor and maintenance chronology"},
             {"label", true, "failure or intervention outcome"}, {"feature", true, "condition and usage signals"}},
            "Risk tiers by asset class with maintenance windows, safety boundaries, approval rules, and kill switches.",
            "Phased rollout by asset class with confirmed-alert precision and avoided-failure evidence.",
            {"functional safety", "false work orders", "missed failures", "OT security", "maintenance capacity"},
            levels("risk and work-order drafts", "add survival timing",
                   "add anomaly detection", "add bounded automatic scheduling")),
        band_a(
            "S-9", "Automated Outbound",
            "Choose governed outbound journeys and preserve a no-contact option for every audience.",
            "Coordinate targeting, channel, pacing, and message policy without exhausting customers.",
            {{"M-1", "primary", 1}, {"M-3", "foundation", 1},
             {"M-2", "foundation", 1}, {"M-6", "attachable", 2},
             {"M-8", "attachable", 3}, {"M-10", "attachable", 4}},
            {"M-2", "M-1", "M-3", "M-6", "M-8", "M-10"},
            {"policy engine for journeys within caps"}, {"L1"},
            {{"send inside caps", "A2"}, {"audiences and copy", "A1"},
             {"pacing inside bounds", "A3"}},
            {{"entity", true, "resolved customer or account"}, {"treatment", true, "journey, channel, and no-contact arm"},
             {"label", true, "response and downstream value"}, {"time", true, "eligibility and contact history"}},
            "Journey eligibility, consent and suppression, contact caps, channel budgets, approvals, and no-contact arms.",
            "Send/hold holdouts per cell with incremental response and downstream value measurement.",
            {"unsubscribe", "complaints", "frequency cap", "consent", "margin"},
            levels("governed outbound cells", "add uplift selection",
                   "add action arbitration", "add grounded language assistance")),
        band_a(
            "S-10", "Fraud & Financial-Crime Decisioning",
            "Make bounded request-time fraud decisions and route evidence-rich cases for review.",
            "Reduce loss and false declines together while keeping every decision explainable.",
            {{"M-1", "primary", 1}, {"M-2", "foundation", 1},
             {"M-12", "foundation", 1}, {"M-5", "attachable", 3}},
            {"M-2", "M-12", "M-1", "M-5"},
            {"request-time authorization decision", "case tasks"}, {"L4", "L1"},
            {{"in-band decisions", "A2"}, {"emergency rules and promotions", "A1"},
             {"thresholds inside approved bands", "A3"}},
            {{"entity", true, "account, device, merchant, and counterparty graph"},
             {"label", true, "matured fraud, dispute, or case disposition"},
             {"time", true, "request-time and label-maturation chronology"},
             {"group", true, "relationship edges and investigation groups"}},
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
            {"M-2", "M-1", "M-6", "M-8", "M-15"}, {"decision API across channels"}, {"L4"},
            {{"arbitration inside policy", "A2"}, {"action catalog and high-stakes offers", "A1"},
             {"exploration inside bounds", "A3"}},
            {{"entity", true, "one customer identity across channels"},
             {"treatment", true, "eligible actions and no-action arm"},
             {"label", true, "per-decision and long-horizon outcomes"},
             {"time", true, "decision chronology and propensity log"}},
            "Eligibility, consent, cross-channel caps, budgets, margin floors, business overrides, and exploration floor.",
            "Global holdout plus per-arm propensity logging, off-policy evaluation, and long-horizon value.",
            {"complaints", "unsubscribe", "margin", "offer fairness", "exploration floor"},
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
        sketch("B-1", "Dynamic Pricing & Discount Governance",
               "Set prices or discounts inside explicit margin and authority bands.",
               "Govern price changes despite policy-confounded historical demand.",
               {{"M-4", "primary", 1}, {"M-6", "foundation", 1}, {"M-16", "foundation", 1}},
               {{"entity", true, "product, customer, and market"},
                {"treatment", true, "price or discount offered"},
                {"label", true, "demand, margin, and customer outcome"}},
               "Promote when a target account has identifiable price variation or permits controlled price experiments."),
        sketch("B-2", "Employee Attrition (aggregate-first)",
               "Explain aggregate workforce risks without flagging individuals to managers.",
               "Support organizational interventions with small-group and proxy protections.",
               {{"M-1", "primary", 1}, {"M-7", "foundation", 1}},
               {{"group", true, "protected organizational cohort"},
                {"label", true, "aggregate attrition outcome"},
                {"time", true, "workforce snapshot and outcome window"}},
               "Promote with a sufficiently large workforce, a named people leader, and legal or works-council approval."),
        sketch("B-3", "Ticket Triage & Service Automation",
               "Route service work with explicit abstention and measured resolution quality.",
               "Reduce misroutes and handling effort without mistaking containment for resolution.",
               {{"M-10", "primary", 1}, {"M-19", "foundation", 1}},
               {{"document", true, "ticket text and interaction history"},
                {"label", true, "routing and resolved outcome"},
                {"entity", false, "customer, service, and product"}},
               "Promote when ticket volume is high enough and the service organization owns or funds its taxonomy."),
        sketch("B-4", "Document & Workflow Automation",
               "Extract and validate documents while routing uncertain fields to review.",
               "Automate high-volume document lanes with deterministic checks and audit samples.",
               {{"M-11", "primary", 1}},
               {{"document", true, "source document and page images"},
                {"label", true, "verified fields and workflow outcome"},
                {"entity", false, "case, customer, or transaction"}},
               "Promote for a high-volume document lane or repeated demand from multiple deployed modules."),
        sketch("B-5", "Supplier Risk & Procurement Intelligence",
               "Prioritize supplier risks and procurement actions with evidence.",
               "Reduce disruption exposure despite sparse, selected supplier outcomes.",
               {{"M-1", "primary", 1}, {"M-4", "foundation", 1}},
               {{"entity", true, "supplier, part, site, and contract"},
                {"label", true, "disruption or performance outcome"},
                {"time", true, "orders, deliveries, and risk signals"}},
               "Promote with concentrated manufacturing demand and reusable external-data support across several modules."),
        sketch("B-6", "Healthcare Operations (no-show & denials)",
               "Prioritize assistive scheduling and revenue-cycle work with equity controls.",
               "Recover operational capacity without turning predictions into access penalties.",
               {{"M-1", "primary", 1}},
               {{"entity", true, "patient, appointment, or claim"},
                {"label", true, "attendance or denial outcome"},
                {"time", true, "scheduling and claim chronology"}},
               "Promote with a provider design customer that owns revenue-cycle operations and has an agreed privacy posture."),
        sketch("B-7", "Credit Underwriting & Limit Management",
               "Support credit policy with examiner-readable evidence and adverse-action discipline.",
               "Balance loss, approval, and fairness under regulated model-risk controls.",
               {{"M-1", "primary", 1}, {"M-17", "foundation", 1}},
               {{"entity", true, "applicant, account, and facility"},
                {"label", true, "matured repayment outcome"},
                {"time", true, "application cutoff and performance window"}},
               "Promote with a lender whose model-risk team co-signs and exercises the full validation pipeline."),
        sketch("B-8", "IT & Security Operations",
               "Prioritize operational and security risks with graduated, bounded authority.",
               "Reduce alert load and exposure while constraining automated remediation.",
               {{"M-7", "primary", 1}, {"M-12", "foundation", 1}},
               {{"entity", true, "asset, identity, service, and dependency"},
                {"time", true, "event and telemetry chronology"},
                {"label", false, "confirmed incident or operational impact"}},
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
    for (const auto& node : graph.nodes()) {
        if (!node->attributes.is_object() ||
            node->attributes.value("solution_stage", 0) != stage) {
            continue;
        }
        result.add_node(node->kind, node->name, node->attributes, node->id);
    }
    for (const ir::Edge& edge : graph.edges()) {
        if (result.has_node(edge.from) && result.has_node(edge.to)) {
            result.add_edge(edge.from, edge.to, edge.kind);
        }
    }
    return result;
}

} // namespace zetta::ml
