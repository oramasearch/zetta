#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "zetta/api/background.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

namespace zetta::ml {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

bool m1_model_kind(std::string_view value) {
    const std::string normalized = lower(value);
    return normalized == "gbdt" || normalized == "gradient_boosting" ||
           normalized == "classifier" || normalized == "classification" ||
           normalized == "regression" || normalized == "score" ||
           normalized == "tabular";
}

bool m1_owns(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        if (const auto kind = node->attributes.find("model_kind");
            kind != node->attributes.end() && kind->is_string()) {
            return m1_model_kind(kind->get<std::string>());
        }
        if (const auto algorithm = node->attributes.find("algo");
            algorithm != node->attributes.end() && algorithm->is_string()) {
            return m1_model_kind(algorithm->get<std::string>());
        }
    }
    return zetta::usecases::churn::is_churn_graph(graph);
}

ir::ExecutionGraph build_m1_shape(const ShapeRequest& request) {
    ir::ExecutionGraph graph;
    const bool lead_scoring = request.solution == "S-2";
    const bool automated_outbound = request.solution == "S-9";
    const bool collections = request.solution == "S-4";
    const bool fraud_decisioning = request.solution == "S-10";
    const bool supplier_risk = request.solution == "B-5";
    const bool credit_underwriting = request.solution == "B-7";
    const bool healthcare_operations = request.solution == "B-6";

    if (request.solution == "B-2") {
        const std::string group_column =
            request.hints.value("group_column", "entity_id");
        const std::string group_size_column =
            request.hints.value("group_size_column", "group_size");
        graph.add_node(
            ir::StepKind::Connect, "Load aggregate workforce cohorts",
            nlohmann::json{
                {"source", request.hints.value("source", "people_analytics")},
                {"table", request.hints.value("table", "workforce_groups")},
                {"note", "one aggregate row per team, role, or tenure band"}},
            "load_workforce_groups");
        graph.add_node(
            ir::StepKind::Clean, "Build aggregate attrition features",
            nlohmann::json{
                {"group_column", group_column},
                {"group_size_column", group_size_column},
                {"aggregate_only", true},
                {"note", "retain cohort measures only; never publish employee rows"}},
            "build_group_features");
        graph.add_node(
            ir::StepKind::Model, "Train aggregate attrition-risk model",
            nlohmann::json{
                {"model_kind", "gbdt"},
                {"algo", "gradient_boosting"},
                {"target", request.hints.value("label_column", "attrition_label")},
                {"group_column", group_column},
                {"group_size_column", group_size_column},
                {"aggregate_only", true},
                {"note", "estimate cohort risk from aggregate workforce measures"}},
            "train_group_risk_model");
        graph.add_node(
            ir::StepKind::Index, "Index aggregate workforce risk",
            nlohmann::json{{"kind", "feature"}, {"aggregate_only", true}},
            "index_group_scores");
        graph.add_node(
            ir::StepKind::Serve, "Explain aggregate workforce drivers",
            nlohmann::json{
                {"aggregate_only", true},
                {"note", "expose cohort risk and drivers to the people leader"}},
            "serve_group_drivers");
        graph.add_node(
            ir::StepKind::Act, "Publish aggregate workforce risk",
            nlohmann::json{
                {"result_database", "workforce_results"},
                {"result_table_prefix", "aggregate_attrition_risk_"},
                {"entity_output_column", "group_key"},
                {"score_output_column", "attrition_risk"},
                {"group_column", group_column},
                {"group_size_column", group_size_column},
                {"aggregate_only", true},
                {"note", "publish one risk row per protected cohort"}},
            "publish_group_risk");
        graph.add_edge("load_workforce_groups", "build_group_features");
        graph.add_edge("build_group_features", "train_group_risk_model");
        graph.add_edge("train_group_risk_model", "index_group_scores");
        graph.add_edge("index_group_scores", "serve_group_drivers");
        graph.add_edge("serve_group_drivers", "publish_group_risk");
        return graph;
    }

    if (request.solution == "S-3") {
        const int value_horizon_days =
            request.hints.value("value_horizon_days", 365);
        graph.add_node(
            ir::StepKind::Connect, "Load customer value cohort",
            nlohmann::json{{"source", request.hints.value("source", "datasets")},
                           {"table", request.hints.value("table", "customers")},
                           {"note", "one point-in-time row per customer"}},
            "load_customers");
        graph.add_node(
            ir::StepKind::Clean, "Build lifetime value features",
            nlohmann::json{{"note", "pre-cutoff value, cost, engagement, and tenure inputs"}},
            "build_features");
        graph.add_node(
            ir::StepKind::Model, "Train lifetime value model",
            nlohmann::json{{"model_kind", "regression"},
                           {"algo", "gradient_boosting"},
                           {"objective", "squared_error"},
                           {"target", request.hints.value("label_column", "future_value")},
                           {"value_horizon_days", value_horizon_days},
                           {"note", "predict net customer value over the declared horizon"}},
            "train_churn_model");
        graph.add_node(
            ir::StepKind::Index, "Rank predicted lifetime value",
            nlohmann::json{{"kind", "feature"},
                           {"note", "order customers by predicted future value"}},
            "index_scores");
        graph.add_node(
            ir::StepKind::Serve, "Explain lifetime value predictions",
            nlohmann::json{{"note", "expose predicted value and top drivers"}},
            "serve_scores");
        graph.add_node(
            ir::StepKind::Act, "Write lifetime value predictions",
            nlohmann::json{{"output", "lifetime_value_predictions"},
                           {"note", "one predicted-value row per customer"}},
            "notify_retention");
        graph.add_edge("load_customers", "build_features");
        graph.add_edge("build_features", "train_churn_model");
        graph.add_edge("train_churn_model", "index_scores");
        graph.add_edge("index_scores", "serve_scores");
        graph.add_edge("serve_scores", "notify_retention");
        return graph;
    }

    if (request.solution == "S-12") {
        graph.add_node(
            ir::StepKind::Connect, "Load first notices of loss",
            nlohmann::json{{"source", request.hints.value("source", "claims")},
                           {"table", request.hints.value("table", "fnol")},
                           {"note", "one point-in-time row per claim"}},
            "load_claims");
        graph.add_node(
            ir::StepKind::Clean, "Build claim triage features",
            nlohmann::json{
                {"note", "pre-decision FNOL features only; exclude dispositions"}},
            "build_claim_features");
        graph.add_node(
            ir::StepKind::Model, "Train referral-risk model",
            nlohmann::json{{"model_kind", "gbdt"},
                           {"algo", "gradient_boosting"},
                           {"target", "referral_confirmed"},
                           {"note", "estimate review value, never claim denial"}},
            "train_referral_model");
        graph.add_node(
            ir::StepKind::Index, "Index claim referral scores",
            nlohmann::json{{"kind", "feature"}},
            "index_claim_scores");
        graph.add_node(
            ir::StepKind::Serve, "Serve claim scores and reasons",
            nlohmann::json{{"note", "expose per-claim score and evidence"}},
            "serve_claim_scores");
        graph.add_node(
            ir::StepKind::Act, "Publish claim referral scores",
            nlohmann::json{{"result_database", "claims_results"},
                           {"result_table_prefix", "referral_scores_"},
                           {"entity_output_column", "claim_id"},
                           {"score_output_column", "referral_score"},
                           {"note", "policy assigns paths after scoring"}},
            "publish_claim_scores");

        graph.add_edge("load_claims", "build_claim_features");
        graph.add_edge("build_claim_features", "train_referral_model");
        graph.add_edge("train_referral_model", "index_claim_scores");
        graph.add_edge("index_claim_scores", "serve_claim_scores");
        graph.add_edge("serve_claim_scores", "publish_claim_scores");
        return graph;
    }

    if (healthcare_operations) {
        graph.add_node(
            ir::StepKind::Connect, "Load healthcare operations cases",
            nlohmann::json{
                {"source", request.hints.value("source", "healthcare")},
                {"table", request.hints.value("table", "operations")},
                {"note", "minimum-necessary appointment and claim operations rows"}},
            "load_healthcare_operations");
        graph.add_node(
            ir::StepKind::Clean, "Build minimum-necessary operations features",
            nlohmann::json{
                {"note", "exclude direct identifiers, clinical decisions, and care-access outcomes"}},
            "build_healthcare_features");
        graph.add_node(
            ir::StepKind::Model, "Train healthcare operations risk model",
            nlohmann::json{
                {"model_kind", "gbdt"},
                {"algo", "gradient_boosting"},
                {"target", request.hints.value("label_column", "operational_outcome")},
                {"note", "rank assistive scheduling and revenue-cycle work only"}},
            "train_healthcare_operations_model");
        graph.add_node(
            ir::StepKind::Index, "Index healthcare operations scores",
            nlohmann::json{{"kind", "feature"}},
            "index_healthcare_operations_scores");
        graph.add_node(
            ir::StepKind::Serve, "Serve operations scores and reasons",
            nlohmann::json{
                {"note", "expose operational risk without changing care access"}},
            "serve_healthcare_operations_scores");
        graph.add_node(
            ir::StepKind::Act, "Publish healthcare operations risk scores",
            nlohmann::json{
                {"result_database", "healthcare_results"},
                {"result_table_prefix", "operation_risk_"},
                {"entity_output_column", "entity"},
                {"score_output_column", "operational_risk"},
                {"note", "policy selects assistive work after scoring"}},
            "publish_healthcare_operations_scores");
        graph.add_edge("load_healthcare_operations",
                       "build_healthcare_features");
        graph.add_edge("build_healthcare_features",
                       "train_healthcare_operations_model");
        graph.add_edge("train_healthcare_operations_model",
                       "index_healthcare_operations_scores");
        graph.add_edge("index_healthcare_operations_scores",
                       "serve_healthcare_operations_scores");
        graph.add_edge("serve_healthcare_operations_scores",
                       "publish_healthcare_operations_scores");
        return graph;
    }

    graph.add_node(
        ir::StepKind::Connect,
        credit_underwriting ? "Load credit applications"
        : fraud_decisioning ? "Load authorization requests"
        : supplier_risk ? "Load supplier and contract records"
        : lead_scoring ? "Load lead and opportunity records"
                         : collections ? "Load receivables"
                                       : automated_outbound ? "Load consented outbound candidates"
                             : "Load customer accounts",
        nlohmann::json{
            {"source", "warehouse"},
            {"table", credit_underwriting ? "credit_applications"
                         : fraud_decisioning ? "authorizations"
                         : supplier_risk ? "suppliers"
                         : lead_scoring ? "leads"
                         : collections ? "receivables"
                                       : "customers"},
            {"note", credit_underwriting ? "point-in-time applications awaiting a governed credit decision"
                         : fraud_decisioning ? "authorization requests to score before a response"
                         : supplier_risk ? "suppliers, parts, sites, and contracts at the decision cutoff"
                         : lead_scoring ? "open leads and opportunities to score for conversion"
                         : collections ? "eligible receivables to score for recovery priority"
                                       : automated_outbound
                         ? "resolved customers eligible for outbound audience selection"
                         : "active accounts to score for churn risk"}},
        "load_customers");
    graph.add_node(
        ir::StepKind::Connect,
        credit_underwriting ? "Load verified application and bureau evidence"
        : fraud_decisioning ? "Load request-time and graph signals"
        : supplier_risk ? "Load delivery and external risk evidence"
        : lead_scoring ? "Load pre-qualification engagement"
                         : collections ? "Load payment and contact history"
                                       : automated_outbound ? "Load pre-send engagement and contact history"
                             : "Load usage & billing activity",
        nlohmann::json{
            {"source", "warehouse"},
            {"table", credit_underwriting ? "credit_evidence"
                         : fraud_decisioning ? "authorization_signals"
                         : supplier_risk ? "supplier_risk_signals"
                         : lead_scoring ? "lead_events"
                         : collections ? "collections_events"
                                       : "events"},
            {"note", credit_underwriting ? "verified pre-decision income, exposure, and repayment signals"
                         : fraud_decisioning ? "request-time device, merchant, counterparty, and graph features"
                         : supplier_risk ? "point-in-time delivery, quality, concentration, and external publications"
                         : lead_scoring ? "point-in-time engagement signals per lead"
                         : collections ? "point-in-time payment, delinquency, and contact signals"
                                       : automated_outbound
                         ? "point-in-time engagement, consent, suppression, and contact evidence"
                         : "engagement + payment signals per account"}},
        "load_activity");
    graph.add_node(
        ir::StepKind::Clean,
        credit_underwriting ? "Build point-in-time credit features"
        : fraud_decisioning ? "Build request-time fraud features"
        : supplier_risk ? "Build supplier risk features"
        : lead_scoring ? "Build conversion features"
                         : collections ? "Build recovery features"
                                       : automated_outbound ? "Build outbound response features"
                             : "Build churn features",
        nlohmann::json{
            {"note", credit_underwriting
                         ? "exclude protected attributes and post-decision performance"
                     : fraud_decisioning
                         ? "training and request-time features must have exact parity"
                     : supplier_risk
                         ? "use pre-event supplier evidence and exclude post-disruption records"
                     : lead_scoring
                         ? "point-in-time features; exclude post-qualification fields"
                     : collections
                         ? "point-in-time features; exclude post-contact outcomes"
                     : automated_outbound
                         ? "point-in-time response features; keep policy evidence out of model features"
                         : "point-in-time features; keep out post-cutoff leakage"}},
        "build_features");
    graph.add_node(
        ir::StepKind::Model,
        credit_underwriting ? "Train credit-risk model"
        : fraud_decisioning ? "Train fraud-risk model"
        : supplier_risk ? "Train supplier disruption-risk model"
        : lead_scoring ? "Train conversion-propensity model"
                         : collections ? "Train recovery-priority model"
                                       : automated_outbound ? "Train outbound response-propensity model"
                             : "Train churn-risk model",
        nlohmann::json{{"model_kind", "gbdt"},
                       {"algo", "gradient_boosting"},
                       {"target", credit_underwriting ? "defaulted"
                         : fraud_decisioning ? "fraud_label"
                         : supplier_risk ? "disruption"
                         : lead_scoring ? "converted"
                         : collections ? "promise_to_pay"
                                       : automated_outbound ? "response"
                                                       : "churned"},
                       {"note", credit_underwriting ? "predict matured repayment risk for each applicant"
                         : fraud_decisioning ? "predict matured fraud risk for each authorization"
                         : supplier_risk ? "predict disruption risk for each supplier relationship"
                         : lead_scoring ? "predict probability each lead converts"
                         : collections ? "predict point-in-time recovery propensity"
                                       : automated_outbound
                                    ? "predict response propensity before incremental targeting"
                                    : "predict probability each account churns"}},
        "train_churn_model");
    graph.add_node(
        ir::StepKind::Index,
        credit_underwriting ? "Index credit risk scores"
        : fraud_decisioning ? "Index authorization scores"
        : supplier_risk ? "Index supplier risk scores"
        : lead_scoring ? "Index conversion scores"
                         : collections ? "Index recovery scores"
                                       : automated_outbound ? "Index outbound response scores"
                             : "Index churn scores",
        nlohmann::json{{"kind", "feature"},
                       {"note", "make scores fast to look up at serve time"}},
        "index_scores");
    graph.add_node(
        ir::StepKind::Serve,
        credit_underwriting ? "Serve credit scores and adverse-action reasons"
        : fraud_decisioning ? "Serve authorization scores and reasons"
        : supplier_risk ? "Serve supplier flags and evidence"
        : lead_scoring ? "Serve conversion scores and reasons"
                         : collections ? "Serve recovery scores and reasons"
                                       : automated_outbound ? "Serve outbound response scores and reasons"
                             : "Serve churn scores & reasons",
        nlohmann::json{
            {"note", credit_underwriting ? "expose per-applicant risk, uncertainty, and ranked reason codes"
                         : fraud_decisioning ? "expose per-request fraud score and graph evidence"
                         : supplier_risk ? "expose per-supplier risk, reasons, and external provenance"
                         : lead_scoring ? "expose per-lead conversion score and top drivers"
                         : collections ? "expose per-receivable recovery priority and drivers"
                                       : automated_outbound
                         ? "expose per-entity response propensity and top drivers"
                         : "expose per-account risk + top drivers"}},
        "serve_scores");
    graph.add_node(
        ir::StepKind::Act,
        credit_underwriting ? "Publish credit risk scores"
        : fraud_decisioning ? "Write fraud authorization scores"
        : supplier_risk ? "Publish supplier risk flags"
        : lead_scoring ? "Route sales queue tasks"
                         : collections ? "Prioritize collections treatments"
                                       : automated_outbound ? "outbound response scores"
                             : "Trigger retention outreach",
        nlohmann::json{
            {"note", credit_underwriting ? "publish examiner-readable scores before policy and human authority"
                         : fraud_decisioning ? "write request-time decisions and case-review candidates"
                         : supplier_risk ? "publish evidence-backed supplier risk flags for procurement"
                         : lead_scoring ? "route capacity-aware CRM tasks with SLAs"
                         : collections ? "publish the governed recovery-priority table"
                                       : automated_outbound
                         ? "publish the outbound-specific decision score artifact"
                         : "notify retention on high-risk accounts"},
             {"result_database", credit_underwriting ? "credit_results"
                  : fraud_decisioning ? "fraud_results"
                  : supplier_risk ? "supplier_risk_results"
                  : collections ? "collections_results"
                  : automated_outbound ? "solution_methodology_results" : "churn_results"},
             {"result_table_prefix", credit_underwriting ? "risk_scores_"
                  : fraud_decisioning ? "authorization_scores_"
                  : supplier_risk ? "risk_flags_"
                  : collections ? "recovery_priority_"
                  : automated_outbound ? "outbound_response_" : "at_risk_"},
             {"entity_output_column", credit_underwriting ? "applicant"
                  : fraud_decisioning ? "transaction"
                  : supplier_risk ? "supplier"
                  : collections ? "receivable"
                  : automated_outbound ? "entity" : "account"},
             {"score_output_column", credit_underwriting ? "credit_risk"
                  : fraud_decisioning ? "fraud_score"
                  : supplier_risk ? "supplier_risk_score"
                  : collections ? "recovery_score"
                  : automated_outbound ? "response_propensity" : "risk"},
             {"evidence_output_fields",
              supplier_risk
                  ? nlohmann::json{"reason_codes", "dominant_driver"}
                  : nlohmann::json::array()}},
        "notify_retention");

    graph.add_edge("load_customers", "build_features");
    graph.add_edge("load_activity", "build_features");
    graph.add_edge("build_features", "train_churn_model");
    graph.add_edge("train_churn_model", "index_scores");
    graph.add_edge("index_scores", "serve_scores");
    graph.add_edge("serve_scores", "notify_retention");
    return graph;
}

api::ApiResult run_m1(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-1 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding wired no background "
            "action owner");
    }

    zetta::usecases::churn::ChurnDataset dataset;
    zetta::usecases::churn::ChurnTableInput table;
    nlohmann::json dataset_notes = nlohmann::json::array();
    nlohmann::json dataset_contract;
    data::FeaturePlan feature_plan;
    // A landed table named by the pipeline is the default data source.
    if (!request.input.contains("dataset_path")) {
        for (const auto& source : request.graph->graph().nodes_of_kind(ir::StepKind::Connect)) {
            const std::string database = source->attributes.value("source", std::string{});
            const std::string name = source->attributes.value("table", std::string{});
            if (database.empty() || name.empty()) {
                continue;
            }
            if (database != "warehouse" || name != "customers") {
                request.input["dataset_path"] = database + "." + name;
                break;
            }
            for (const auto& candidate : context.node.datasets()) {
                if (candidate.database == database && candidate.name == name) {
                    request.input["dataset_path"] = database + "." + name;
                    break;
                }
            }
            break;
        }
    }
    if (const auto dataset_path = request.input.find("dataset_path");
        dataset_path != request.input.end()) {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind == zetta::data::DatasetPathKind::Table) {
            zetta::data::DatasetContract overrides;
            if (const auto given = request.input.find("dataset");
                given != request.input.end()) {
                std::string reason;
                const std::optional<zetta::data::DatasetContract> parsed =
                    zetta::data::dataset_contract_from_json(*given, &reason);
                if (!parsed) {
                    return api::ApiResult::failure("invalid_input", reason);
                }
                overrides = *parsed;
            }
            zetta::data::ResolvedDataset resolved =
                zetta::data::resolve_dataset(
                    context.node, path, overrides,
                    zetta::data::default_dataset_sampler(context.node,
                                                         context.cluster));
            if (!resolved.ok()) {
                return run_dataset_failure(request, "M-1", resolved);
            }
            feature_plan = plan_run_features(request, "M-1", resolved);
            if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
                return api::ApiResult::failure("invalid_input", error);
            }
            table.table_id = resolved.table_id;
            table.contract = resolved.contract;
            table.dataset_name = dataset_path->get<std::string>();
            table.notes = resolved.notes;
            // #1040: say that the table came from the pipeline's own connect step.
            if (const auto note = request.input.find("pipeline_note");
                note != request.input.end() && note->is_string()) {
                table.notes.insert(table.notes.begin(), note->get<std::string>());
            }
            dataset_notes = table.notes;
            dataset_contract = to_json(resolved.contract);
        } else {
            auto loaded =
                zetta::usecases::churn::load_churn_dataset(path.directory);
            if (!loaded.ok()) {
                return api::ApiResult::failure("engine_error", loaded.error);
            }
            dataset = std::move(loaded.dataset);
        }
    } else {
        // No dataset and no connect-step table (handle_run fills the path when
        // the pipeline names one, #1040): the built-in demo dataset.
        dataset = zetta::usecases::churn::demo_churn_dataset();
    }

    if (table.empty()) {
        const auto shards = zetta::usecases::churn::encode_churn_dataset(dataset);
        const auto spec = zetta::usecases::churn::churn_featurize_spec(shards.as_of);
        data::SampledDataset sampled;
        sampled.database = "churn";
        sampled.table = "accounts";
        sampled.rows = shards.accounts.size();
        sampled.sample = shards.accounts;
        sampled.columns = {"entity", "churned", "mrr", "seats", "tenure_days"};
        data::FeaturePlanOptions planning;
        planning.methodology = "M-1";
        planning.label_column = spec.label_column;
        planning.entity_column = spec.entity_column;
        for (const auto& feature : spec.account_features) {
            planning.native_entries.push_back({{feature}, {feature}, "numeric carry-through",
                "static account attribute used by the native churn featurizer"});
        }
        feature_plan = data::plan_features(data::analyze_sampled_dataset(sampled), planning);
        if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
            return api::ApiResult::failure("invalid_input", error);
        }
        for (const auto& feature : spec.event_features) {
            std::vector<std::string> sources = {"events." + spec.time_column,
                                                "events." + spec.entity_column};
            if (!feature.value_column.empty()) {
                sources.push_back("events." + feature.value_column);
            }
            if (feature.has_type_filter()) {
                sources.push_back("events." + feature.type_column);
            }
            const std::string window = feature.window_start
                ? std::to_string(*feature.window_start) : "all history";
            feature_plan.entries.push_back({std::move(sources),
                {feature.name}, std::string(zetta::ddf::to_string(feature.agg)) +
                    " over [" + window + ", " + std::to_string(spec.as_of) + "]",
                "the native churn specification defines the event filter and aggregate"});
        }
    }

    zetta::usecases::churn::ChurnSolutionOptions options;
    const bool automated_outbound =
        request.input.value("solution", std::string{}) == "S-9";
    const bool healthcare_operations =
        request.input.value("solution", std::string{}) == "B-6";
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.model_name = request.input.value("model_name", options.model_name);
    options.top_k = request.input.value("top_k", options.top_k);
    options.coordinate = request.input.value("coordinate", true);
    if (automated_outbound) {
        options.model_name = request.input.value(
            "model_name", std::string("outbound_response_model"));
        options.result_database = "solution_methodology_results";
        options.result_table_prefix = "outbound_response_";
        options.result_entity_column = "entity";
        options.result_score_column = "response_propensity";
    } else if (healthcare_operations) {
        options.model_name = request.input.value(
            "model_name", std::string("healthcare_operations_risk"));
        options.result_database = "healthcare_results";
        options.result_table_prefix = "operation_risk_";
        options.result_entity_column = "entity";
        options.result_score_column = "operational_risk";
    }
    options.params.holdout_fraction = request.input.value(
        "holdout_fraction", options.params.holdout_fraction);
    for (const auto& model :
         request.graph->graph().nodes_of_kind(ir::StepKind::Model)) {
        if (model->attributes.value("objective", std::string{}) ==
            "squared_error") {
            options.params.objective = "squared_error";
            options.calibration.enabled = false;
            if (!request.input.contains("model_name")) {
                options.model_name = "customer_lifetime_value";
            }
            break;
        }
    }

    std::set<std::string> prior_runs;
    for (const auto& existing : context.node.runs()) {
        if (existing) {
            prior_runs.insert(existing->id());
        }
    }
    auto handle = table.empty() ? zetta::usecases::churn::AsyncChurnRun::start(
                                      context.node, request.graph->graph(),
                                      std::move(dataset), options)
                                : zetta::usecases::churn::AsyncChurnRun::start(
                                      context.node, request.graph->graph(),
                                      std::move(table), options);
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string run_id;
    while (std::chrono::steady_clock::now() < deadline) {
        context.node.drain_owner_work();
        for (const auto& candidate : context.node.runs()) {
            if (!candidate || prior_runs.contains(candidate->id())) {
                continue;
            }
            if (candidate->project() != request.project->id() ||
                candidate->graph() != request.graph->id()) {
                continue;
            }
            run_id = candidate->id();
            break;
        }
        if (!run_id.empty()) {
            break;
        }
        if (handle->done()) {
            if (handle->result().execution.run) {
                run_id = handle->result().execution.run->id();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (run_id.empty()) {
        if (handle->done() && !handle->result().ok()) {
            return api::ApiResult::failure("engine_error",
                                           handle->result().execution.error);
        }
        return api::ApiResult::failure(
            "engine_error",
            "the run was submitted but its Run record did not appear in time "
            "— poll list_runs for a run labelled project '" +
                request.project->id() + "'");
    }

    nlohmann::json output;
    if (!feature_plan.dataset.empty()) {
        record_run_features(context.node, run_id, feature_plan);
    }
    output["run_id"] = run_id;
    output["engine"] = "churn";
    output["status"] = "running";
    output["poll"] =
        "get_run / system.runs for status+logs; system.jobs for the "
        "featurize/train/explain/score jobs";
    if (!dataset_contract.is_null()) {
        output["dataset"] = std::move(dataset_contract);
        output["dataset_notes"] = std::move(dataset_notes);
    }
    return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_m1_methodology() {
    Methodology methodology;
    methodology.id = "M-1";
    methodology.name = "Tabular supervised learning";
    methodology.family = "The tabular decision core";
    methodology.model_kinds = {"gbdt",       "gradient_boosting",
                               "classifier", "classification",
                               "regression", "score",
                               "tabular"};
    methodology.solutions = {
        "S-1", "S-2", "S-3", "S-4", "S-10", "S-12", "B-2", "B-5", "B-6", "B-7"};
    methodology.keywords = {
        "churn",    "retention",       "retain",     "attrition", "cancel",
        "renew",    "renewal",         "subscriber", "lapse",     "downgrade",
        "win-back", "predict whether", "classify",   "score",
        "customer lifetime value", "lifetime value", "clv",
        "no-show", "denial risk", "healthcare operations",
        "credit underwriting", "credit risk", "limit management"};
    methodology.inputs = {
        {"entity", true, "entity identifier"},
        {"label", true, "outcome column"},
        {"time", false, "point-in-time cutoff column"},
    };
    methodology.outputs = {"model", "scores", "drivers", "ranked entities"};
    methodology.metrics = {"ROC-AUC", "PR-AUC", "log loss", "Brier score",
                           "lift", "RMSE", "MAE"};
    methodology.runnable = true;
    methodology.build_shape = build_m1_shape;
    methodology.owns = m1_owns;
    methodology.run = run_m1;
    return methodology;
}

std::string unsupported_methodology_message(const Methodology& methodology) {
    return methodology.name + " (" + methodology.id +
           ") is not supported yet: " + methodology.issue;
}

std::string runnable_methodologies() {
    std::string result;
    for (const Methodology& methodology : methodologies()) {
        if (!methodology.runnable) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += methodology.name + " (" + methodology.id + ")";
    }
    return result;
}

nlohmann::json methodology_json(const Methodology& methodology) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const MethodologyInput& input : methodology.inputs) {
        inputs.push_back({{"role", input.role},
                          {"required", input.required},
                          {"description", input.description}});
    }
    return nlohmann::json{{"id", methodology.id},
                          {"name", methodology.name},
                          {"family", methodology.family},
                          {"model_kinds", methodology.model_kinds},
                          {"solutions", methodology.solutions},
                          {"inputs", std::move(inputs)},
                          {"outputs", methodology.outputs},
                          {"metrics", methodology.metrics},
                          {"runnable", methodology.runnable},
                          {"partial", methodology.partial},
                          {"issue", methodology.issue}};
}

} // namespace zetta::ml
