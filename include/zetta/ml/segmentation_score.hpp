#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/segmentation.hpp"

// **Place a row in a trained segmentation model's segments** (#1497).
//
// `score` and `explain` read gradient-boosted weights. A segmentation model's
// weights are different: `SegmentationModel::to_json()` (the encoded features
// with their scaling, the centres, and, from #1497, each segment's label). So
// these functions load that body and assign a row the way training did:
//
//   * a numeric feature is `(value - location) / scale`, a boolean 1 or 0;
//   * a categorical column writes one indicator per fitted level, and its last
//     level is the rare bucket, set for any value that is not one of the
//     others;
//   * the row's segment is the nearest centre (squared Euclidean distance),
//     ties to the lower segment id, and the distance reported is its square
//     root: the same `nearest_center` rule and the same `distance_to_center`
//     the training run wrote into its assignment table.
//
// A row that lacks a column the model reads is refused, never zero-filled: a
// zero is a real, standardized value and would place the row confidently in
// the wrong segment.
//
// Non-throwing throughout. See docs/ml/segmentation.md#scoring-and-explaining.
namespace zetta {
class Node;
}

namespace zetta::ml {

// Whether a registered model is a segmentation model (its kind).
bool is_segmentation_model(const Model& model);

// The weights body as a SegmentationModel, validated. nullopt with `error` set
// when the body is absent here, is not JSON, or is not a segmentation body.
std::optional<SegmentationModel>
segmentation_model_from_json(const nlohmann::json& body, std::string* error = nullptr);
std::optional<SegmentationModel> load_segmentation_model(const Model& model,
                                                         std::string* error = nullptr);

// The columns a row must carry, in the model's order, each named once.
std::vector<std::string> segmentation_source_columns(const SegmentationModel& model);

// One row encoded into the model's space. Empty `error` on success.
struct SegmentationEncoding {
    std::vector<double> point;
    std::string error;  // names the missing or unreadable column
    bool ok() const noexcept { return error.empty(); }
};
SegmentationEncoding encode_segmentation_row(const SegmentationModel& model,
                                             const nlohmann::json& row);

// The nearest centre of an encoded point and its (Euclidean) distance.
struct SegmentPlacement {
    int segment_id = 0;
    double distance = 0.0;
};
SegmentPlacement place_segmentation_point(const SegmentationModel& model,
                                          const std::vector<double>& point);

// One row's segment, keyed like a ranked entry: the caller's entity key (or
// the row's index when none was given).
struct SegmentScore {
    std::int64_t entity = 0;
    int segment_id = 0;
    double distance = 0.0;
};

struct SegmentationScoreResult {
    std::vector<SegmentScore> assignments;  // in row order
    std::vector<std::string> feature_columns;
    // Each segment's label, by segment id; empty for a model trained before
    // #1497 recorded them (the caller may read the run's profile table).
    std::vector<std::string> labels;
    std::shared_ptr<const data::EntityKeyMapping> entity_keys;
    std::size_t partitions_read = 0;
    std::uint64_t rows_scored = 0;
    std::string error;
    bool ok() const noexcept { return error.empty(); }
};

// Rows supplied directly. `entities[i]` labels `rows[i]`; when empty the rows
// are labelled by their index.
SegmentationScoreResult
score_segmentation_rows(const Model& model, const std::vector<nlohmann::json>& rows,
                        const std::vector<std::int64_t>& entities = {},
                        std::shared_ptr<const data::EntityKeyMapping> entity_keys = {});

// The rows of a resolved table this member owns (the primary replica of each
// partition, or the whole unpartitioned table), in table order.
SegmentationScoreResult score_segmentation_table(Node& node, const Model& model,
                                                 const data::ResolvedDataset& dataset);

// Why a row is in its segment.
//
// Each column's contribution is how much it favours the row's segment over the
// runner-up (the next nearest centre): the sum, over that column's encoded
// features, of (point - runner_up)^2 - (point - segment)^2. Positive pulls the
// row toward its own segment; negative pulls it toward the runner-up. The
// contributions are exact and additive:
//
//     runner_up_distance^2 - distance^2  ==  sum of contributions
//
// Strongest first, by absolute value, ties in the model's column order.
struct SegmentContribution {
    std::string feature;     // the source column
    nlohmann::json value;    // the row's value for it
    double contribution = 0.0;
};
struct SegmentExplanation {
    std::optional<std::int64_t> entity;
    int segment_id = 0;
    double distance = 0.0;
    std::optional<int> runner_up_segment_id;  // absent for a one-centre model
    double runner_up_distance = 0.0;
    std::string label;  // empty when the model records none
    std::vector<SegmentContribution> contributions;
    double residual = 0.0;  // (runner_up^2 - distance^2) - sum; ~0
    std::string error;
    bool ok() const noexcept { return error.empty(); }
};
SegmentExplanation explain_segmentation_row(const Model& model, const nlohmann::json& row);
SegmentExplanation explain_segmentation_entity(Node& node, const Model& model,
                                               const data::ResolvedDataset& dataset,
                                               std::int64_t entity);

}  // namespace zetta::ml
