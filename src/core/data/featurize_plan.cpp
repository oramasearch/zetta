#include "zetta/data/featurize_plan.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/node.hpp"

namespace zetta::data {
namespace {

struct MethodologyRuleEntry {
    MethodologyRule rule;
    std::vector<std::string_view> aliases;
};

const std::vector<MethodologyRuleEntry>& methodology_rules() {
    static const std::vector<MethodologyRuleEntry> rules = {
        {MethodologyRule{"M-1", MethodologyFamily::Tree, true, true, false, false},
         {"m-1", "classification", "classifier", "churn", "tree", "gbdt"}},
        {MethodologyRule{"M-2", MethodologyFamily::Native, false, true, false, false},
         {"entity_resolution", "entity-resolution", "m-2"}},
        {MethodologyRule{"M-3", MethodologyFamily::Distance, false, true, true, false},
         {"segmentation", "segment", "clustering", "cluster", "m-3"}},
        {MethodologyRule{"M-4", MethodologyFamily::Forecast, true, false, false, true},
         {"forecast", "forecasting", "time_series", "m-4"}},
        {MethodologyRule{"M-5", MethodologyFamily::Tree, true, true, false, true},
         {"survival", "time_to_event", "m-5"}},
        {MethodologyRule{"M-6", MethodologyFamily::Uplift, true, true, false, false},
         {"uplift", "causal", "treatment_effect", "m-6"}},
        {MethodologyRule{"M-7", MethodologyFamily::Distance, false, false, true, false},
         {"anomaly", "anomaly_detection", "m-7"}},
        {MethodologyRule{"M-8", MethodologyFamily::Native, true, false, false, true},
         {"next_best_action", "recommendation", "m-8"}},
        {MethodologyRule{"M-9", MethodologyFamily::Ranking, true, false, false, false},
         {"ranking", "rank", "learning_to_rank", "m-9"}},
        {MethodologyRule{"M-10", MethodologyFamily::Native, false, false, false, false},
         {"language", "nlp", "embedding", "embeddings", "rag", "fine_tuning", "retrieval", "m-10"}},
        {MethodologyRule{"M-11", MethodologyFamily::Native, false, false, false, false},
         {"document_extraction", "document_intelligence", "ocr", "document", "m-11"}},
        {MethodologyRule{"M-12", MethodologyFamily::Native, true, false, false, true},
         {"graph", "graph_ml", "m-12"}},
        {MethodologyRule{"M-13", MethodologyFamily::Native, true, false, false, false},
         {"vision", "computer_vision", "m-13"}},
    };
    return rules;
}

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool contains(std::string_view text, std::string_view fragment) {
    return lowered(text).find(lowered(fragment)) != std::string::npos;
}

bool has_role(const ColumnAnalysis& column, std::string_view role) {
    return std::ranges::find(column.role_candidates, role) != column.role_candidates.end();
}

const ColumnAnalysis* find_column(const DatasetAnalysis& analysis, std::string_view name) {
    if (name.empty()) {
        return nullptr;
    }
    const std::string wanted = lowered(name);
    const auto found =
        std::ranges::find_if(analysis.columns, [&wanted](const ColumnAnalysis& column) {
            return lowered(column.name) == wanted;
        });
    return found == analysis.columns.end() ? nullptr : &*found;
}

const ColumnAnalysis* first_with_role(const DatasetAnalysis& analysis, std::string_view role) {
    const auto found = std::ranges::find_if(
        analysis.columns, [role](const ColumnAnalysis& column) { return has_role(column, role); });
    return found == analysis.columns.end() ? nullptr : &*found;
}

bool is_leakage(const DatasetAnalysis& analysis, const std::string& column, std::string& reason) {
    const auto found = std::ranges::find_if(
        analysis.leakage_candidates,
        [&column](const LeakageCandidate& candidate) { return candidate.column == column; });
    if (found == analysis.leakage_candidates.end()) {
        return false;
    }
    reason = found->reason;
    return true;
}

void exclude_once(FeaturePlan& plan, const std::string& column, std::string reason) {
    const bool already_excluded =
        std::ranges::any_of(plan.excluded, [&column](const FeatureExclusion& exclusion) {
            return exclusion.source_column == column;
        });
    if (!already_excluded) {
        plan.excluded.push_back({column, std::move(reason)});
    }
}

FeaturePlan require_transform(FeaturePlan plan, std::string name, std::string description,
                              std::string proposed_sql = {}, std::string clean_step = {}) {
    plan.status = "transform_required";
    plan.transform_required = RequiredTransform{std::move(name), std::move(description),
                                                std::move(proposed_sql), std::move(clean_step)};
    return plan;
}

bool is_long_measurement_table(const DatasetAnalysis& analysis) {
    bool has_measure_name = false;
    bool has_value = false;
    for (const ColumnAnalysis& column : analysis.columns) {
        const std::string name = lowered(column.name);
        has_measure_name = has_measure_name || name == "metric" || name == "measure" ||
                           name == "measurement" || name == "metric_name" || name == "variable";
        has_value =
            has_value || name == "value" || name == "metric_value" || name == "measurement_value";
    }
    return has_measure_name && has_value;
}

const ColumnAnalysis* repeated_entity_column(const DatasetAnalysis& analysis) {
    const auto found = std::ranges::find_if(analysis.columns, [](const ColumnAnalysis& column) {
        return has_role(column, "entity_key") && column.distinct_ratio < 0.98;
    });
    if (found != analysis.columns.end()) {
        return &*found;
    }
    return first_with_role(analysis, "entity_key");
}

std::string sql_literal(const std::string& value) {
    const bool numeric = !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
        return std::isdigit(character) || character == '.' || character == '-';
    });
    if (numeric) {
        return value;
    }
    std::string escaped;
    for (char character : value) {
        escaped += character == '\'' ? "''" : std::string(1, character);
    }
    return "'" + escaped + "'";
}

FeaturePlan event_aggregation_transform(FeaturePlan plan, const FeaturePlanOptions& options) {
    const ColumnAnalysis* entity = repeated_entity_column(plan.analysis);
    const ColumnAnalysis* time = find_column(plan.analysis, options.time_column);
    if (time == nullptr) {
        time = first_with_role(plan.analysis, "time");
    }
    if (entity == nullptr) {
        return require_transform(std::move(plan), "add_entity_key",
                                 "The event rows do not identify which entity owns each event.", {},
                                 "Add an entity key to every event row before point-in-time "
                                 "aggregation.");
    }
    if (time == nullptr || !options.as_of) {
        return require_transform(std::move(plan), "add_snapshot_date",
                                 "Point-in-time event aggregation needs an event "
                                 "time and an as_of or snapshot date.",
                                 {},
                                 "Add the event timestamp and the prediction "
                                 "snapshot date; aggregate only events "
                                 "at or before that snapshot.");
    }

    std::string select = "SELECT " + entity->name + ", COUNT(*) AS event_count";
    for (const ColumnAnalysis& column : plan.analysis.columns) {
        if (column.semantic_type != SemanticType::Numeric || column.name == entity->name) {
            continue;
        }
        select += ", SUM(" + column.name + ") AS sum_" + column.name;
        select += ", AVG(" + column.name + ") AS mean_" + column.name;
        select += ", MIN(" + column.name + ") AS min_" + column.name;
        select += ", MAX(" + column.name + ") AS max_" + column.name;
    }
    select += " FROM " + plan.analysis.database + "." + plan.analysis.table;
    select += " WHERE " + time->name + " <= " + sql_literal(*options.as_of);
    select += " GROUP BY " + entity->name;
    return require_transform(std::move(plan), "aggregate_events_to_entity",
                             "The methodology predicts one row per entity, but "
                             "the dataset contains repeated event "
                             "rows. Create point-in-time count, sum, mean, min, "
                             "max, distinct, last-value, and "
                             "time-since-last features over 7/30/90-day and "
                             "all-history windows up to as_of, keeping "
                             "each output's source-column and window provenance.",
                             std::move(select),
                             "Use the proposed supported SQL for the all-history "
                             "aggregate, then add 7/30/90-day "
                             "point-in-time windows, distinct counts, last "
                             "values, and time-since-last in a clean "
                             "step before approving the transformed dataset.");
}

std::string default_label(const DatasetAnalysis& analysis, const FeaturePlanOptions& options) {
    if (const ColumnAnalysis* explicit_label = find_column(analysis, options.label_column)) {
        return explicit_label->name;
    }
    if (const ColumnAnalysis* inferred = first_with_role(analysis, "label")) {
        return inferred->name;
    }
    return {};
}

std::string default_time(const DatasetAnalysis& analysis, const FeaturePlanOptions& options) {
    if (const ColumnAnalysis* explicit_time = find_column(analysis, options.time_column)) {
        return explicit_time->name;
    }
    if (const ColumnAnalysis* inferred = first_with_role(analysis, "time")) {
        return inferred->name;
    }
    return {};
}

std::string default_group(const DatasetAnalysis& analysis, const FeaturePlanOptions& options) {
    if (const ColumnAnalysis* explicit_group = find_column(analysis, options.group_column)) {
        return explicit_group->name;
    }
    if (const ColumnAnalysis* inferred = first_with_role(analysis, "group")) {
        return inferred->name;
    }
    return {};
}

void append_numeric_plan(FeaturePlan& plan, const ColumnAnalysis& column,
                         const MethodologyRule& rule) {
    std::vector<std::string> features{column.name, column.name + "__missing"};
    std::string transform = "recode sentinel/null values as missing; median impute on training "
                            "rows; add a missing flag";
    if (rule.needs_distances) {
        transform += "; robust-scale from training-row median and IQR";
    } else {
        transform += "; no scaling for tree-based learning";
    }
    plan.entries.push_back({{column.name},
                            std::move(features),
                            std::move(transform),
                            rule.needs_distances
                                ? "distance-based methods need comparable robust scales"
                                : "numeric signal can be used directly by trees"});
}

void append_boolean_plan(FeaturePlan& plan, const ColumnAnalysis& column) {
    std::vector<std::string> features{column.name + "__indicator"};
    std::string transform = "map the two values to 0/1";
    if (column.null_rate > 0.0) {
        features.push_back(column.name + "__missing");
        transform += "; add a missing flag";
    }
    plan.entries.push_back({{column.name},
                            std::move(features),
                            std::move(transform),
                            "two-valued data has a direct binary representation"});
}

void append_categorical_plan(FeaturePlan& plan, const ColumnAnalysis& column,
                             const MethodologyRule& rule, bool time_ordered) {
    const std::string rare_rule = "group levels with fewer than 10 rows or below 0.5% into OTHER; "
                                  "keep missing as its own level";
    if (column.distinct_count <= 20) {
        plan.entries.push_back({{column.name},
                                {column.name + "__one_hot"},
                                rare_rule + "; one-hot encode the remaining levels",
                                "categoricals with at most 20 levels stay explicit"});
        return;
    }
    if (rule.supervised && rule.family != MethodologyFamily::Forecast) {
        plan.entries.push_back({{column.name},
                                {column.name + "__target_mean"},
                                rare_rule + "; out-of-fold target encode with smoothing m=20" +
                                    (time_ordered ? "; use forward-only time-aware folds"
                                                  : "; fit folds on training rows only"),
                                "high-cardinality categoricals need leakage-safe target encoding"});
        return;
    }
    exclude_once(plan, column.name,
                 rule.family == MethodologyFamily::Forecast
                     ? "forecasting does not target-encode across the forecast origin"
                     : "high-cardinality categorical data has no label-safe encoding for "
                       "this unsupervised methodology");
}

void append_datetime_plan(FeaturePlan& plan, const ColumnAnalysis& column,
                          const MethodologyRule& rule, const FeaturePlanOptions& options) {
    if (!options.as_of && rule.family != MethodologyFamily::Forecast) {
        exclude_once(plan, column.name,
                     "raw timestamps are never features and no as_of was supplied");
        return;
    }
    plan.entries.push_back(
        {{column.name},
         {column.name + "__elapsed_days", column.name + "__month", column.name + "__hour_sin",
          column.name + "__hour_cos", column.name + "__weekday_sin", column.name + "__weekday_cos"},
         rule.family == MethodologyFamily::Forecast
             ? "derive calendar parts from the observation time; encode hour and "
               "weekday cyclically; never use the raw timestamp"
             : "subtract from as_of for elapsed age/tenure/recency; add calendar "
               "parts with cyclical hour/weekday; never use the raw timestamp",
         "timestamps become relative or cyclical features, never raw ordering "
         "values"});
}

void append_column_plan(FeaturePlan& plan, const ColumnAnalysis& column,
                        const MethodologyRule& rule, const FeaturePlanOptions& options,
                        const std::string& label, const std::string& group, bool time_ordered) {
    std::string leakage_reason;
    if (is_leakage(plan.analysis, column.name, leakage_reason)) {
        exclude_once(plan, column.name, "leakage candidate: " + leakage_reason);
        return;
    }
    if (column.name == label) {
        exclude_once(plan, column.name,
                     rule.family == MethodologyFamily::Distance
                         ? "segmentation excludes the label"
                         : "the label is the target, never an input feature");
        return;
    }
    if (column.name == options.entity_column) {
        exclude_once(plan, column.name, "the entity key identifies rows, not a model feature");
        return;
    }
    if (column.name == group) {
        exclude_once(plan, column.name, "used to form ranking groups, not as a feature");
        return;
    }
    if (rule.family == MethodologyFamily::Uplift &&
        (has_role(column, "treatment") || column.name == options.treatment_column)) {
        exclude_once(plan, column.name,
                     "used as the uplift treatment assignment, not as an input feature");
        return;
    }
    if (rule.family == MethodologyFamily::Uplift && has_role(column, "post_treatment")) {
        exclude_once(plan, column.name, "uplift excludes columns observed after treatment");
        return;
    }

    switch (column.semantic_type) {
    case SemanticType::Identifier:
        exclude_once(plan, column.name, "identifiers are excluded and are never target-encoded");
        return;
    case SemanticType::Numeric:
        append_numeric_plan(plan, column, rule);
        return;
    case SemanticType::Boolean:
        append_boolean_plan(plan, column);
        return;
    case SemanticType::Categorical:
        append_categorical_plan(plan, column, rule, time_ordered);
        return;
    case SemanticType::Datetime:
        append_datetime_plan(plan, column, rule, options);
        return;
    case SemanticType::FreeText:
        exclude_once(plan, column.name, "needs M-10 embeddings");
        return;
    case SemanticType::Json:
        exclude_once(plan, column.name,
                     "nested JSON must be flattened into stable source columns first");
        return;
    case SemanticType::Unknown:
        exclude_once(plan, column.name, "the sampled values do not have a supported semantic type");
        return;
    }
}

} // namespace

std::optional<MethodologyRule> methodology_rule(std::string_view methodology) {
    const std::string wanted = lowered(methodology);
    for (const MethodologyRuleEntry& entry : methodology_rules()) {
        if (std::ranges::any_of(entry.aliases,
                                [&wanted](std::string_view alias) { return wanted == alias; })) {
            return entry.rule;
        }
    }
    return std::nullopt;
}

FeaturePlan plan_features(const DatasetAnalysis& analysis, const FeaturePlanOptions& options) {
    FeaturePlan plan;
    plan.dataset = analysis.database + "." + analysis.table;
    plan.methodology = options.methodology;
    plan.analysis = analysis;
    if (!analysis.ok()) {
        plan.error = analysis.error;
        return plan;
    }

    const std::optional<MethodologyRule> rule = methodology_rule(options.methodology);
    if (!rule) {
        plan.error = "methodology: unknown '" + options.methodology +
                     "' (known strings include M-1, segmentation, forecast, "
                     "uplift, ranking)";
        return plan;
    }
    plan.methodology = rule->canonical_name;

    if (is_long_measurement_table(analysis)) {
        return require_transform(std::move(plan), "pivot_long_to_wide",
                                 "The table stores measurement names and values in "
                                 "rows; the methodology needs one "
                                 "feature column per measurement.",
                                 {},
                                 "Pivot the metric or measure name into columns, "
                                 "keyed by entity and observation "
                                 "time. Refuse duplicate entity/time/measure cells "
                                 "or define their aggregation.");
    }

    if (rule->prediction_per_entity &&
        (analysis.grain == DatasetGrain::Event || analysis.grain == DatasetGrain::TimeSeries)) {
        return event_aggregation_transform(std::move(plan), options);
    }

    const std::string label = default_label(analysis, options);
    const std::string time = default_time(analysis, options);
    const std::string group = default_group(analysis, options);

    if ((analysis.grain == DatasetGrain::Snapshot ||
         std::ranges::any_of(analysis.columns,
                             [](const ColumnAnalysis& column) {
                                 return column.semantic_type == SemanticType::Datetime;
                             })) &&
        !options.as_of && rule->family != MethodologyFamily::Forecast &&
        rule->family != MethodologyFamily::Native && options.native_entries.empty()) {
        return require_transform(
            std::move(plan), "add_snapshot_date",
            "Datetime features must be computed relative to a declared as_of or "
            "snapshot date; "
            "using the current time would change training data silently.",
            {}, "Add a snapshot date for each prediction row and pass it as as_of.");
    }

    if (rule->supervised && label.empty()) {
        const auto join =
            std::ranges::find_if(analysis.joinable_tables, [](const JoinCandidate& candidate) {
                return std::ranges::contains(candidate.target_roles, "label");
            });
        if (join != analysis.joinable_tables.end()) {
            return require_transform(
                std::move(plan), "join_label_table",
                "The supervised methodology has no label in this table, but a "
                "joinable table " +
                    join->database + "." + join->table + " has a label candidate.",
                {},
                "Join " + analysis.database + "." + analysis.table + " to " + join->database + "." +
                    join->table + " on " + join->source_column + " = " + join->target_column +
                    ", verify one label per entity, and approve the new "
                    "dataset. The current SQL subset has no JOIN, so this is a clean "
                    "step.");
        }
        return require_transform(std::move(plan), "capture_label",
                                 "The supervised methodology needs a label, and no "
                                 "label candidate is present.",
                                 {},
                                 "Capture the outcome column at the prediction "
                                 "unit and rerun analysis.");
    }

    if (rule->family == MethodologyFamily::Forecast && time.empty()) {
        return require_transform(
            std::move(plan), "add_time_column",
            "Forecasting needs a time column to order lags, rolling windows, and "
            "the temporal "
            "split.",
            {}, "Add the observation timestamp for every row and rerun the plan.");
    }
    if (rule->family == MethodologyFamily::Ranking && group.empty()) {
        return require_transform(std::move(plan), "add_ranking_group",
                                 "Ranking needs the query or session group that defines which rows "
                                 "compete.",
                                 {}, "Add a group column such as query_id and rerun the plan.");
    }
    if (rule->family == MethodologyFamily::Uplift &&
        first_with_role(analysis, "treatment") == nullptr &&
        find_column(analysis, options.treatment_column) == nullptr) {
        return require_transform(std::move(plan), "capture_treatment",
                                 "Uplift needs the treatment assignment in "
                                 "addition to the outcome label.",
                                 {},
                                 "Capture treatment assignment and ensure all "
                                 "features precede treatment.");
    }

    const bool time_ordered = rule->time_ordered || !time.empty() ||
                              analysis.grain == DatasetGrain::TimeSeries ||
                              analysis.grain == DatasetGrain::Snapshot;
    if (!options.native_entries.empty()) {
        for (const auto& entry : options.native_entries) {
            for (const auto& source : entry.source_columns) {
                if (find_column(analysis, source) == nullptr) {
                    return require_transform(
                        std::move(plan), "add_feature_source",
                        "The native featurizer needs source column '" + source + "'.", {},
                        "Add the source column or correct the pipeline's input roles.");
                }
            }
            plan.entries.push_back(entry);
        }
        for (const auto& column : analysis.columns) {
            const bool used = std::ranges::any_of(plan.entries, [&](const auto& entry) {
                return std::ranges::contains(entry.source_columns, column.name);
            });
            if (column.name == label || column.name == options.entity_column || !used) {
                std::string reason;
                if (column.name == label) {
                    reason = "the label is a target or evaluation role, never a contemporaneous feature";
                } else if (column.name == options.entity_column) {
                    reason = "the entity key identifies rows, not a model feature";
                } else if (is_leakage(analysis, column.name, reason)) {
                    reason = "leakage candidate: " + reason;
                } else if (rule->family == MethodologyFamily::Uplift &&
                           has_role(column, "post_treatment")) {
                    reason = "uplift excludes columns observed after treatment";
                } else {
                    reason = "not an input to this methodology's native featurizer";
                }
                exclude_once(plan, column.name, std::move(reason));
            }
        }
        return plan;
    }
    for (const ColumnAnalysis& column : analysis.columns) {
        append_column_plan(plan, column, *rule, options, label, group, time_ordered);
    }

    if (rule->family == MethodologyFamily::Forecast) {
        plan.entries.push_back({{label, time},
                                {label + "__lag_1", label + "__lag_7", label + "__lag_28",
                                 label + "__rolling_mean_7", label + "__rolling_mean_30"},
                                "order by time; compute lags and rolling windows from strictly "
                                "earlier rows; "
                                "evaluate on a temporal split; never target-encode across the "
                                "forecast origin",
                                "forecasting learns from prior target history without seeing the "
                                "current or future "
                                "target"});
    }
    return plan;
}

FeaturePlan plan_features(Node& node, const DatasetPath& path, const FeaturePlanOptions& options,
                          const DatasetSampler& sampler, const zetta::cluster::Cluster* cluster) {
    AnalysisOptions analysis_options;
    analysis_options.sample_rows = options.sample_rows;
    analysis_options.label_column = options.label_column;
    analysis_options.time_column = options.time_column;
    analysis_options.as_of = options.as_of;
    const DatasetAnalysis analysis =
        analyze_dataset(node, path, analysis_options, sampler, cluster);
    return plan_features(analysis, options);
}

nlohmann::json to_json(const FeaturePlanEntry& entry) {
    return nlohmann::json{{"source_columns", entry.source_columns},
                          {"features", entry.features},
                          {"transform", entry.transform},
                          {"reason", entry.reason}};
}

nlohmann::json to_json(const FeaturePlan& plan) {
    nlohmann::json entries = nlohmann::json::array();
    for (const FeaturePlanEntry& entry : plan.entries) {
        entries.push_back(to_json(entry));
    }
    nlohmann::json excluded = nlohmann::json::array();
    for (const FeatureExclusion& exclusion : plan.excluded) {
        excluded.push_back(
            {{"source_column", exclusion.source_column}, {"reason", exclusion.reason}});
    }
    nlohmann::json result{{"status", plan.status},           {"dataset", plan.dataset},
                          {"methodology", plan.methodology}, {"analysis", to_json(plan.analysis)},
                          {"entries", std::move(entries)},   {"excluded", std::move(excluded)}};
    if (plan.transform_required) {
        result["transform_required"] = {
            {"name", plan.transform_required->name},
            {"description", plan.transform_required->description},
            {"proposed_create_dataset_from_query_sql", plan.transform_required->proposed_sql},
            {"clean_step", plan.transform_required->clean_step},
        };
    } else {
        result["transform_required"] = nullptr;
    }
    return result;
}

} // namespace zetta::data
