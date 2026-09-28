#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"

// Data-first featurization (#1062): inspect a landed table before deciding how
// a methodology may use it. Analysis and planning are read-only value results;
// registered runs validate the plan before handing inputs to native trainers.
namespace zetta::data {

enum class SemanticType {
    Boolean,
    Datetime,
    FreeText,
    Identifier,
    Categorical,
    Numeric,
    Json,
    Unknown,
};

std::string_view to_string(SemanticType type);

struct ColumnAnalysis {
    std::string name;
    SemanticType semantic_type = SemanticType::Unknown;
    std::vector<std::string> evidence;
    std::vector<std::string> role_candidates;
    std::size_t null_count = 0;
    double null_rate = 0.0;
    std::size_t distinct_count = 0;
    double distinct_ratio = 0.0;
    nlohmann::json top_values = nlohmann::json::array();
    nlohmann::json quantiles = nlohmann::json::object();
    nlohmann::json string_lengths = nlohmann::json::object();
    nlohmann::json sentinel_values = nlohmann::json::array();
};

struct CandidateKey {
    std::vector<std::string> columns;
    double uniqueness = 0.0;
};

enum class DatasetGrain {
    Entity,
    Event,
    TimeSeries,
    Snapshot,
    Ambiguous,
};

std::string_view to_string(DatasetGrain grain);

struct JoinCandidate {
    std::string database;
    std::string table;
    std::string source_column;
    std::string target_column;
    double key_overlap = 0.0;
    std::vector<std::string> target_roles;
};

struct LeakageCandidate {
    std::string column;
    std::string reason;
    double score = 0.0;
};

struct DatasetAnalysis {
    std::string table_id;
    std::string database;
    std::string table;
    std::uint64_t rows = 0;
    std::size_t sampled_rows = 0;
    std::vector<ColumnAnalysis> columns;
    DatasetGrain grain = DatasetGrain::Ambiguous;
    std::string grain_evidence;
    std::vector<CandidateKey> candidate_keys;
    std::vector<JoinCandidate> joinable_tables;
    std::vector<LeakageCandidate> leakage_candidates;
    std::string error;

    bool ok() const { return error.empty(); }
};

struct AnalysisOptions {
    std::size_t sample_rows = kDefaultSampleRows;
    std::string label_column;
    std::string time_column;
    std::optional<std::string> as_of;
    std::size_t categorical_max_distinct = 50;
    double categorical_max_ratio = 0.05;
};

// Analyze an already sampled table. This pure entry point makes the inference
// rules testable independently of storage and cluster placement.
DatasetAnalysis analyze_sampled_dataset(const SampledDataset& sampled,
                                        const AnalysisOptions& options = {});

// Resolve, sample, and analyze a landed table. Other tables in the same
// database are sampled through the same query path to find key overlap.
DatasetAnalysis analyze_dataset(Node& node, const DatasetPath& path,
                                const AnalysisOptions& options = {},
                                const DatasetSampler& sampler = {},
                                const zetta::cluster::Cluster* cluster = nullptr);

nlohmann::json to_json(const ColumnAnalysis& column);
nlohmann::json to_json(const DatasetAnalysis& analysis);

enum class MethodologyFamily {
    Tree,
    Distance,
    Forecast,
    Uplift,
    Ranking,
    Native,
};

struct MethodologyRule {
    std::string canonical_name;
    MethodologyFamily family = MethodologyFamily::Tree;
    bool supervised = true;
    bool prediction_per_entity = true;
    bool needs_distances = false;
    bool time_ordered = false;
};

// One rule table keyed by the methodology registry's stable IDs and aliases.
std::optional<MethodologyRule> methodology_rule(std::string_view methodology);

struct FeaturePlanEntry {
    std::vector<std::string> source_columns;
    std::vector<std::string> features;
    std::string transform;
    std::string reason;
};

struct FeatureExclusion {
    std::string source_column;
    std::string reason;
};

struct RequiredTransform {
    std::string name;
    std::string description;
    std::string proposed_sql;
    std::string clean_step;
};

struct FeaturePlan {
    std::string status = "ready";
    std::string dataset;
    std::string methodology;
    DatasetAnalysis analysis;
    std::vector<FeaturePlanEntry> entries;
    std::vector<FeatureExclusion> excluded;
    std::optional<RequiredTransform> transform_required;
    std::string error;

    bool ok() const { return error.empty(); }
};

struct FeaturePlanOptions {
    std::string methodology;
    std::string label_column;
    std::string time_column;
    std::string group_column;
    std::optional<std::string> as_of;
    std::size_t sample_rows = kDefaultSampleRows;
    std::string entity_column;
    std::string treatment_column;
    // Native featurizers declare their actual derived features here. Sources
    // are checked against the same analysis before they can be used (#1062).
    std::vector<FeaturePlanEntry> native_entries;
};

FeaturePlan plan_features(const DatasetAnalysis& analysis, const FeaturePlanOptions& options);

FeaturePlan plan_features(Node& node, const DatasetPath& path, const FeaturePlanOptions& options,
                          const DatasetSampler& sampler = {},
                          const zetta::cluster::Cluster* cluster = nullptr);

nlohmann::json to_json(const FeaturePlanEntry& entry);
nlohmann::json to_json(const FeaturePlan& plan);

} // namespace zetta::data
