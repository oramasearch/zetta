// SPDX-License-Identifier: MIT

#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <ranges>
#include <set>

#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

void collect_features(const nlohmann::json& object, std::set<std::string>& names) {
    for (const auto* key : {"features", "feature_columns", "context_columns"}) {
        const auto found = object.find(key);
        if (found == object.end() || !found->is_array()) {
            continue;
        }
        for (const auto& feature : *found) {
            if (feature.is_string()) {
                names.insert(feature.get<std::string>());
            } else if (feature.is_object() && feature.contains("name") &&
                       feature.at("name").is_string()) {
                names.insert(feature.at("name").get<std::string>());
            }
        }
    }
}

std::set<std::string> requested_features(const ir::ExecutionGraph& graph,
                                         const nlohmann::json& input) {
    std::set<std::string> names;
    collect_features(input, names);
    if (const auto given = input.find("dataset"); given != input.end()) {
        collect_features(*given, names);
    }
    for (const auto& step : graph.nodes()) {
        collect_features(step->attributes, names);
        if (const auto config = step->attributes.find("segmentation");
            config != step->attributes.end()) {
            collect_features(*config, names);
        }
    }
    return names;
}

std::string unsafe_feature(const ir::ExecutionGraph& pipeline, const data::DatasetAnalysis& analysis, const std::string& source,
                           const data::FeaturePlanOptions& options) {
    ir::ExecutionGraph graph;
    // Analysis already compared sampled timestamps to the real cutoff. The
    // default below only permits checking its flags; declared timestamp
    // annotations must use their own graph cutoff, or the check refuses them.
    nlohmann::json attributes = {{"as_of", "9999-12-31"},
                                 {"features", nlohmann::json::array()}};
    if (!options.label_column.empty()) {
        attributes["label"] = options.label_column;
    }
    nlohmann::json annotation = {{"name", source}};
    for (const auto& step : pipeline.nodes()) {
        const auto features = step->attributes.find("features");
        if (features == step->attributes.end() || !features->is_array()) {
            continue;
        }
        for (const auto& feature : *features) {
            if (!feature.is_object() || feature.value("name", "") != source) {
                continue;
            }
            for (const auto* flag : {"leaks_target", "post_treatment"}) {
                if (feature.value(flag, false)) {
                    annotation[flag] = true;
                }
            }
            for (const auto* field : {"derived_from", "data_as_of"}) {
                if (feature.contains(field)) {
                    annotation[field] = feature.at(field);
                }
            }
            for (const auto* field : {"as_of", "treatment_time"}) {
                if (step->attributes.contains(field)) {
                    attributes[field] = step->attributes.at(field);
                }
            }
            if (feature.contains("data_as_of") && !step->attributes.contains("as_of")) {
                attributes.erase("as_of");
            }
        }
    }
    for (const auto& candidate : analysis.leakage_candidates) {
        if (candidate.column == source) {
            annotation["leaks_target"] = true;
        }
    }
    if (options.methodology == "M-6") {
        attributes["model_kind"] = "uplift";
        attributes["treatment"] = options.treatment_column;
        attributes["outcome"] = options.label_column;
        for (const auto& column : analysis.columns) {
            if (column.name == source &&
                std::ranges::contains(column.role_candidates, "post_treatment")) {
                annotation["post_treatment"] = true;
            }
        }
    }
    attributes["features"].push_back(std::move(annotation));
    graph.add_node(ir::StepKind::Model, "Planned feature selection", attributes, "features");
    const auto leakage = pipeline::DataLeakageCheck{}.evaluate(graph);
    if (leakage.status == pipeline::CheckStatus::Fail) {
        return leakage.reason;
    }
    if (options.methodology == "M-6") {
        const auto uplift = pipeline::CausalUpliftCheck{}.evaluate(graph);
        if (uplift.status == pipeline::CheckStatus::Fail) {
            return uplift.reason;
        }
    }
    return {};
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

// How a fitted category is encoded, for the run's feature report (#1203).
std::string categorical_transform(const data::CategoricalEncoding& encoding) {
    return "one-hot encoding of " + std::to_string(encoding.levels.size()) + " levels (" +
           join_names(encoding.levels) +
           ") fitted on the training rows only and stored with the model; predict "
           "reads the original text column, and an unseen or missing value encodes as "
           "all zeros";
}

void add_exclusion(data::FeaturePlan& plan, const std::string& column, std::string reason) {
    if (!column.empty() && !std::ranges::any_of(plan.excluded, [&](const auto& excluded) {
            return excluded.source_column == column;
        })) {
        plan.excluded.push_back({column, std::move(reason)});
    }
}

nlohmann::json usage_json(const data::FeaturePlan& plan) {
    nlohmann::json result = {{"used", nlohmann::json::array()},
                             {"excluded", nlohmann::json::array()}};
    for (const auto& entry : plan.entries) {
        for (const auto& feature : entry.features) {
            result["used"].push_back({{"name", feature}, {"source", entry.source_columns},
                                       {"transform", entry.transform}, {"why", entry.reason}});
        }
    }
    for (const auto& excluded : plan.excluded) {
        result["excluded"].push_back({{"name", excluded.source_column}, {"why", excluded.reason}});
    }
    return result;
}

// #1216: an empty plan names every excluded column and why; each reason says
// how to include that column, so the failure is actionable without the plan JSON.
std::string no_inputs_description(const data::FeaturePlan& plan,
                                  const data::FeaturePlanOptions& options) {
    const std::string description =
        "The feature plan leaves no inputs this trainer can consume.";
    std::string listed;
    for (const auto& excluded : plan.excluded) {
        if (excluded.source_column == options.label_column ||
            excluded.source_column == options.entity_column) {
            continue;
        }
        listed += (listed.empty() ? " Excluded columns: " : "; ") + excluded.source_column +
                  " — " + excluded.reason;
    }
    if (listed.empty()) {
        return description + " The table has no columns besides the entity '" +
               options.entity_column + "' and the label '" + options.label_column +
               "'; add at least one numeric column that describes each row.";
    }
    return description + listed + ".";
}

} // namespace

data::FeaturePlan plan_run_features(RunRequest& request, const std::string& methodology,
                                    data::ResolvedDataset& dataset,
                                    data::FeaturePlanOptions options) {
    return plan_table_features(request.context->node, request.context->cluster,
                               request.graph->graph(), request.input, methodology, dataset,
                               std::move(options));
}

data::FeaturePlan plan_table_features(Node& node, const zetta::cluster::Cluster* cluster,
                                      const ir::ExecutionGraph& graph,
                                      const nlohmann::json& input,
                                      const std::string& methodology,
                                      data::ResolvedDataset& dataset,
                                      data::FeaturePlanOptions options) {
    options.methodology = methodology;
    // Table trainers read the contract's prepared numeric columns; only native
    // featurizers declare their own derived inputs (#1216).
    options.numeric_inputs_only = options.native_entries.empty();
    if (options.label_column.empty()) {
        options.label_column = dataset.contract.label_column;
    }
    if (options.time_column.empty()) {
        options.time_column = dataset.contract.time_column;
    }
    if (options.entity_column.empty()) {
        options.entity_column = dataset.contract.entity_column;
    }
    if (!options.as_of && !dataset.contract.time_column.empty() && methodology != "M-21") {
        options.as_of = std::to_string(dataset.contract.as_of);
    }
    if (!options.as_of) {
        const auto cutoff = input.find("solution_snapshot_as_of");
        if (cutoff != input.end() && (cutoff->is_string() || cutoff->is_number())) {
            options.as_of = cutoff->is_string() ? cutoff->get<std::string>() : cutoff->dump();
            options.time_column = "as_of";
        }
    }
    if (!options.as_of) {
        for (const auto& source : graph.nodes_of_kind(ir::StepKind::Connect)) {
            if (source->attributes.value("source_kind", "") == "snapshot" &&
                source->attributes.contains("snapshot_as_of")) {
                options.as_of = source->attributes.at("snapshot_as_of").get<std::string>();
                options.time_column = "as_of";
                break;
            }
        }
    }
    const auto path = data::parse_dataset_path(dataset.database + "." + dataset.table);
    data::AnalysisOptions analysis_options;
    analysis_options.label_column = options.label_column;
    analysis_options.time_column = options.time_column;
    analysis_options.as_of = options.as_of;
    auto analysis = data::analyze_dataset(node, path, analysis_options, {}, cluster);
    const auto requested = requested_features(graph, input);
    // Explicit numeric feature roles resolve the high-uniqueness heuristic;
    // named keys remain excluded even when a caller requests them.
    for (auto& column : analysis.columns) {
        if (requested.contains(column.name) && column.semantic_type == data::SemanticType::Identifier &&
            !column.quantiles.empty() && column.name != options.entity_column &&
            !std::ranges::contains(column.role_candidates, "entity_key")) {
            column.semantic_type = data::SemanticType::Numeric;
            column.evidence.push_back("explicit numeric feature role overrides sampled uniqueness");
        }
    }
    auto plan = data::plan_features(analysis, options);
    if (!feature_plan_error(plan).empty()) {
        return plan;
    }
    const bool native = !options.native_entries.empty();
    // #1203: whether this trainer one-hot encodes the contract's text categories.
    const std::optional<data::MethodologyRule> rule = data::methodology_rule(methodology);
    const bool one_hot = !native && rule && rule->one_hot_text_categories;
    std::vector<data::FeaturePlanEntry> accepted;
    std::vector<std::string> sources;
    std::vector<std::string> categorical_sources;
    for (const auto& entry : plan.entries) {
        bool allowed = true;
        for (const auto& source : entry.source_columns) {
            // Forecast/bandit history and graph endpoints are structural
            // inputs, not contemporaneous target features. Only the bandit's
            // native per-arm statistic may read reward, never its context.
            if ((methodology == "M-4" &&
                 (source == options.label_column || source == options.time_column)) ||
                (methodology == "M-12" && source == options.time_column)) {
                continue;
            }
            if (methodology == "M-15" && source == options.label_column &&
                entry.features == std::vector<std::string>{"arm_policy_statistics"} &&
                entry.source_columns == std::vector<std::string>{options.group_column,
                                                                 options.label_column}) {
                continue;
            }
            // Grouped observations fit a posterior, not a contemporaneous
            // predictor of themselves. No other Bayesian feature may read label.
            if (methodology == "M-17" && source == options.label_column &&
                options.group_column != options.label_column &&
                entry.features == std::vector<std::string>{"hierarchical_group_statistics"} &&
                entry.source_columns == std::vector<std::string>{options.group_column,
                                                                 options.label_column}) {
                continue;
            }
            // Streaming uses event time to bound windows and advance watermarks,
            // not as a scalar predictor or a batch snapshot cutoff.
            if (methodology == "M-21" && source == options.time_column &&
                options.time_column != options.label_column &&
                ((entry.features == std::vector<std::string>{"window_count"} &&
                  entry.source_columns == std::vector<std::string>{options.entity_column,
                                                                   options.time_column}) ||
                 (entry.source_columns.size() == 3 &&
                  entry.source_columns[0] == options.entity_column &&
                  entry.source_columns[1] == options.time_column &&
                  entry.features == std::vector<std::string>{"window_sum_" +
                                                              entry.source_columns[2]}))) {
                continue;
            }
            const std::string reason = unsafe_feature(graph, analysis, source, options);
            if (!reason.empty()) {
                if (native || requested.contains(source)) {
                    plan.error = reason;
                    return plan;
                }
                allowed = false;
                add_exclusion(plan, source, reason);
            }
        }
        if (!allowed) {
            continue;
        }
        if (native) {
            accepted.push_back(entry);
            continue;
        }
        const auto& source = entry.source_columns.front();
        if (!requested.empty() && !requested.contains(source)) {
            add_exclusion(plan, source,
                          "not selected by the pipeline's explicit feature roles; add it to the "
                          "Model step's features to include it");
            continue;
        }
        if (one_hot) {
            const auto category = std::ranges::find_if(
                dataset.contract.categorical_features,
                [&source](const data::CategoricalEncoding& encoding) {
                    return encoding.column == source;
                });
            if (category != dataset.contract.categorical_features.end()) {
                categorical_sources.push_back(source);
                accepted.push_back({{source}, category->feature_names(),
                                    categorical_transform(*category),
                                    "a text category with at most " +
                                        std::to_string(data::kCategoricalMaxLevels) +
                                        " levels; the GBDT reads one 0/1 column per level"});
                continue;
            }
        }
        if (!std::ranges::contains(dataset.contract.feature_columns, source)) {
            add_exclusion(plan, source,
                          "not supported by the trainer's numeric input contract; to include it, "
                          "write it as a numeric column in a clean step");
            continue;
        }
        sources.push_back(source);
        accepted.push_back({{source}, {source},
                            "native numeric carry-through; no learned encoding or scaling",
                            "eligible numeric input selected by the plan; this trainer consumes prepared numeric columns"});
    }
    for (const auto& excluded : plan.excluded) {
        if (requested.contains(excluded.source_column)) {
            const auto reason = unsafe_feature(graph, analysis, excluded.source_column, options);
            if (!reason.empty() || excluded.source_column == options.entity_column) {
                plan.error = reason.empty() ? excluded.reason : reason;
                return plan;
            }
        }
    }
    if (!native) {
        for (const auto& source : dataset.contract.feature_columns) {
            if (!std::ranges::contains(sources, source) &&
                !std::ranges::contains(categorical_sources, source)) {
                add_exclusion(plan, source, "excluded by the feature plan");
            }
        }
        dataset.contract.feature_columns = std::move(sources);
    }
    // Keep only the categories this trainer encodes, and make the run log say what
    // happened to each one the contract found (#1203).
    std::vector<data::CategoricalEncoding> kept;
    for (data::CategoricalEncoding& encoding : dataset.contract.categorical_features) {
        const std::string prefix = "categorical column: '" + encoding.column + "'";
        const auto note = std::ranges::find_if(
            dataset.notes, [&prefix](const std::string& line) { return line.starts_with(prefix); });
        std::string line;
        if (std::ranges::contains(categorical_sources, encoding.column)) {
            line = "categorical feature: '" + encoding.column + "' one-hot encoded into " +
                   join_names(encoding.feature_names()) + " (" +
                   std::to_string(encoding.levels.size()) +
                   " levels fitted on the training rows and stored with the model; an "
                   "unseen or missing value encodes as all zeros)";
            kept.push_back(std::move(encoding));
        } else if (std::ranges::contains(dataset.contract.feature_columns, encoding.column)) {
            // A caller-named column this trainer reads as it is (a text methodology):
            // the category note no longer applies, and nothing was left out.
            if (note != dataset.notes.end()) dataset.notes.erase(note);
            continue;
        } else {
            const auto excluded = std::ranges::find_if(plan.excluded, [&](const auto& item) {
                return item.source_column == encoding.column;
            });
            if (excluded == plan.excluded.end()) {
                add_exclusion(plan, encoding.column,
                              one_hot ? "excluded by the feature plan"
                                      : "text categories are not encoded for this "
                                        "methodology, which reads numeric columns only");
            }
            const auto reason = std::ranges::find_if(plan.excluded, [&](const auto& item) {
                return item.source_column == encoding.column;
            });
            line = "not used: '" + encoding.column + "' (" + reason->reason + ")";
        }
        if (note != dataset.notes.end()) {
            *note = std::move(line);
        } else {
            dataset.notes.push_back(std::move(line));
        }
    }
    dataset.contract.categorical_features = std::move(kept);
    plan.entries = std::move(accepted);
    add_exclusion(plan, options.label_column, "the label is the target, never an input feature");
    add_exclusion(plan, options.entity_column, "the entity key identifies rows, not a model feature");
    if (plan.entries.empty()) {
        plan.status = "transform_required";
        plan.transform_required = data::RequiredTransform{
            "create_eligible_features", no_inputs_description(plan, options), {},
            "Create supported features in a clean step and rerun the plan."};
    }
    return plan;
}

std::string feature_plan_error(const data::FeaturePlan& plan) {
    if (!plan.ok()) {
        return plan.error;
    }
    if (plan.transform_required) {
        return "transform_required: " + plan.transform_required->name + ": " +
               plan.transform_required->description;
    }
    return {};
}

void record_run_features(Node& node, const std::string& run_id, const data::FeaturePlan& plan) {
    if (const auto run = node.find_run(run_id)) {
        std::string error;
        if (!run->set_features(usage_json(plan), &error)) {
            run->add_log({util::now_iso8601(), "error", "feature provenance: " + error});
        }
    }
}

api::ApiResult run_dataset_failure(RunRequest& request, const std::string& methodology,
                                  const data::ResolvedDataset& dataset) {
    // Contract inference can fail before the planner gets a chance to name a
    // missing label or a required shape correction. Preserve that diagnosis.
    if (!dataset.database.empty() && !dataset.table.empty()) {
        data::FeaturePlanOptions options;
        options.methodology = methodology;
        options.entity_column = dataset.contract.entity_column;
        options.label_column = dataset.contract.label_column;
        options.time_column = dataset.contract.time_column;
        if (!options.time_column.empty()) {
            options.as_of = std::to_string(dataset.contract.as_of);
        }
        const auto plan = data::plan_features(request.context->node,
            data::parse_dataset_path(dataset.database + "." + dataset.table), options,
            {}, request.context->cluster);
        if (plan.transform_required) {
            return api::ApiResult::failure("invalid_input", feature_plan_error(plan));
        }
        // A contract with no numeric features fails before plan_run_features;
        // name what the plan excluded and how to include it (#1216).
        if (plan.ok() && plan.entries.empty()) {
            return api::ApiResult::failure(
                "invalid_input", "transform_required: create_eligible_features: " +
                                     no_inputs_description(plan, options));
        }
    }
    return api::ApiResult::failure("invalid_input", dataset.error);
}

nlohmann::json run_feature_report(const pipeline::Run& run, const Model& model) {
    auto result = run.value().features;
    const auto& schema = model.feature_schema();
    if (model.kind() == "segmentation" && schema.contains("features")) {
        result["used"] = nlohmann::json::array();
        for (const auto& feature : schema.at("features")) {
            if (!feature.is_object() || !feature.contains("encoded_name")) {
                continue;
            }
            const bool categorical = feature.value("kind", "") == "categorical";
            result["used"].push_back({
                {"name", feature.at("encoded_name")}, {"source", {feature.at("source")}},
                {"transform", categorical ? "bounded one-hot encoding; rare and missing buckets"
                     : feature.value("robust", false) ? "global median/IQR scaling and median imputation"
                                                       : "global mean/standard-deviation scaling and mean imputation"},
                {"why", "the native clustering model fitted this feature"}});
        }
        for (const auto& excluded : schema.value("excluded_columns", nlohmann::json::array())) {
            if (excluded.is_string() && !std::ranges::any_of(result["excluded"], [&](const auto& item) {
                    return item.at("name") == excluded;
                })) {
                result["excluded"].push_back({{"name", excluded},
                    {"why", "excluded by native clustering preprocessing or reserved as a role"}});
            }
        }
    }
    // Legacy/direct-run records still describe the features recorded by the
    // fitted model. They do not invent imputation or encoding that was not run.
    if (result["used"].empty()) {
        for (const auto& feature : schema.value("features", nlohmann::json::array())) {
            if (feature.is_string()) {
                result["used"].push_back({{"name", feature}, {"source", {feature}},
                    {"transform", "native trainer feature; detailed input provenance was not recorded"},
                    {"why", "listed in the fitted model's feature schema"}});
            }
        }
    }
    return result;
}

} // namespace zetta::ml
