// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/data/dataset_path.hpp"

// Native distributed entity segmentation (#1042).
//
// The trainer standardizes numeric features from global statistics, encodes
// bounded categorical values, initializes with k-means||, and runs Lloyd rounds
// whose local counts and sums are combined by allreduce. Model selection uses a
// consistently selected sample, and every random-looking decision is a stable
// hash of the configured seed and entity key.
//
// The public runner consumes the same ResolvedDataset contract as table-backed
// churn runs. It reads only owned partitions, creates a Model whose kind is
// `segmentation`, and materializes assignment and profile tables with Run
// lineage. The methodology registry adapter can therefore remain a thin call to
// run_segmentation once #1061 is available.
//
// See docs/ml/segmentation.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

struct SegmentationRow {
    std::int64_t entity = 0;
    nlohmann::json values = nlohmann::json::object();
};

struct SegmentationParams {
    int minimum_k = 2;
    int maximum_k = 8;
    int restarts = 5;
    int max_iterations = 100;
    double center_shift_tolerance = 1e-4;
    double minimum_segment_share = 0.02;
    std::size_t silhouette_sample_size = 2048;
    std::uint64_t seed = 42;

    // Explicit input columns keep the data contract reviewable. An empty list
    // infers columns on a single member only; distributed callers must supply
    // it.
    std::vector<std::string> feature_columns;
    std::string entity_column;
    std::string label_column;
    // Explicit roles prevent low-cardinality free text or additional keys from
    // being mistaken for features. Only declared categoricals admit strings.
    std::vector<std::string> categorical_columns;
    std::vector<std::string> excluded_columns;

    // Categorical features are admitted only when their global cardinality is
    // bounded. Infrequent values share one `other` indicator.
    std::size_t maximum_category_levels = 16;
    double rare_category_share = 0.01;
};

enum class SegmentationReduce {
    Sum,
    Minimum,
    Maximum,
};

// The collective operations used by the pure trainer. The default-constructed
// value is the single-member identity. The JobRound adapter supplies all three
// callbacks for a cluster run.
struct SegmentationCollectives {
    std::function<std::optional<std::vector<double>>(
        const std::string &, const std::vector<double> &, SegmentationReduce)>
        allreduce;
    std::function<std::optional<std::vector<std::vector<double>>>(
        const std::string &, const std::vector<double> &)>
        gather;
    std::function<std::optional<std::vector<double>>(
        const std::string &, const std::vector<double> &)>
        broadcast;
};

enum class SegmentationFeatureKind {
    Numeric,
    Categorical,
};

struct SegmentationFeature {
    std::string source;
    std::string encoded_name;
    SegmentationFeatureKind kind = SegmentationFeatureKind::Numeric;
    std::string level;
    double location = 0.0;
    double scale = 1.0;
    bool robust = false;
    bool rare_bucket = false;
};

struct SegmentAssignment {
    std::int64_t entity = 0;
    int segment_id = 0;
    double distance_to_center = 0.0;
};

struct SegmentProfile {
    int segment_id = 0;
    std::uint64_t size = 0;
    double share = 0.0;
    nlohmann::json center = nlohmann::json::object();
    std::vector<std::string> distinguishing_features;
    std::string label;
};

struct SegmentationKMetric {
    int k = 0;
    double inertia = 0.0;
    double silhouette = 0.0;
    double davies_bouldin = 0.0;
    double minimum_share = 0.0;
    bool satisfies_minimum_share = false;
};

struct SegmentationMetrics {
    int chosen_k = 0;
    std::string chosen_k_reason;
    double silhouette = 0.0;
    double davies_bouldin = 0.0;
    double stability = 0.0;
    std::vector<SegmentationKMetric> tried;
};

struct SegmentationModel {
    std::vector<SegmentationFeature> features;
    std::vector<std::vector<double>> centers;
    std::vector<std::string> excluded_columns;
    std::uint64_t seed = 0;

    nlohmann::json to_json() const;
};

struct SegmentationResult {
    SegmentationModel model;
    SegmentationMetrics metrics;
    std::vector<SegmentAssignment> assignments;
    std::vector<SegmentProfile> profiles;
    compute::ComputeStatus status;

    bool ok() const noexcept {
        return status.ok();
    }
};

// Fit over this member's owned rows. The returned assignments cover only the
// local rows; centers, profiles, and metrics are global and identical on every
// participant. All participants must call with the same params and round tag.
SegmentationResult
train_segmentation(const std::vector<SegmentationRow> &local_rows,
                   const SegmentationParams &params = {},
                   const SegmentationCollectives &collectives = {},
                   const std::string &round_tag = "ml.segmentation");

// Adjusted Rand index, exposed for the stability contract and correctness
// tests.
double adjusted_rand_index(const std::vector<int> &left,
                           const std::vector<int> &right);

// Shared by preprocessing and the table reader, before inspecting values.
bool segmentation_excludes_column(const std::string &column,
                                  const SegmentationParams &params);

struct SegmentationRunOptions {
    std::string project;
    std::string graph;
    std::string model_name = "segmentation";
    std::string output_database = "segmentation_results";
    std::string round_tag;
    // A registry submission supplies its already-visible Run and writer. Empty
    // values retain the native runner's root-created Run behavior.
    std::string run_id;
    std::string output_node;
    std::chrono::milliseconds timeout{10000};
};

struct SegmentationRunResult {
    SegmentationResult segmentation;
    compute::JobStatus status = compute::JobStatus::Queued;
    bool is_root = false;
    std::uint64_t read_rows = 0;
    std::string job_id;
    std::string run_id;
    std::string model_id;
    std::string assignment_table;
    std::string profile_table;
    std::string error;

    bool ok() const noexcept {
        return status == compute::JobStatus::Succeeded && error.empty();
    }
};

// Run segmentation over a resolved table using the Node's live JobCoordinator.
// Invoke concurrently on every collective participant. Only the deterministic
// root writes the replicated Model, Run, assignment table, and profile table
// unless options.output_node designates the initiating registry member.
SegmentationRunResult
run_segmentation(Node &node, const zetta::data::ResolvedDataset &dataset,
                 SegmentationParams params = {},
                 const SegmentationRunOptions &options = {});

} // namespace zetta::ml
