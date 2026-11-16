#include "zetta/ml/segmentation_score.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "zetta/node.hpp"

namespace zetta::ml {
namespace {

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) out += ", ";
        out += items[index];
    }
    return out;
}

double squared_distance(const std::vector<double>& left, const std::vector<double>& right) {
    double total = 0.0;
    for (std::size_t index = 0; index < left.size() && index < right.size(); ++index) {
        const double delta = left[index] - right[index];
        total += delta * delta;
    }
    return total;
}

// The nearest centre and the runner-up, by squared distance; ties go to the
// lower id, as training's nearest_center does (strict less-than, in order).
struct Nearest {
    int best = 0;
    double best_squared = std::numeric_limits<double>::infinity();
    int second = -1;
    double second_squared = std::numeric_limits<double>::infinity();
};

Nearest nearest_two(const SegmentationModel& model, const std::vector<double>& point) {
    Nearest out;
    for (std::size_t index = 0; index < model.centers.size(); ++index) {
        const double distance = squared_distance(point, model.centers[index]);
        if (distance < out.best_squared) {
            out.second = out.best_squared == std::numeric_limits<double>::infinity()
                             ? -1
                             : out.best;
            out.second_squared = out.best_squared;
            out.best = static_cast<int>(index);
            out.best_squared = distance;
        } else if (distance < out.second_squared) {
            out.second = static_cast<int>(index);
            out.second_squared = distance;
        }
    }
    return out;
}

std::vector<std::string> labels_of(const SegmentationModel& model) {
    return model.labels.size() == model.centers.size() ? model.labels
                                                       : std::vector<std::string>{};
}

}  // namespace

bool is_segmentation_model(const Model& model) {
    return model.kind() == "segmentation";
}

std::optional<SegmentationModel> segmentation_model_from_json(const nlohmann::json& body,
                                                              std::string* error) {
    const auto fail = [&](const std::string& message) -> std::optional<SegmentationModel> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    if (!body.is_object() || body.value("kind", std::string()) != "segmentation") {
        return fail("the weights are not a segmentation model's");
    }
    const auto features = body.find("features");
    const auto centers = body.find("centers");
    if (features == body.end() || !features->is_array() || features->empty()) {
        return fail("the segmentation weights record no features");
    }
    if (centers == body.end() || !centers->is_array() || centers->empty()) {
        return fail("the segmentation weights record no centres");
    }
    SegmentationModel model;
    for (const nlohmann::json& feature : *features) {
        if (!feature.is_object() || !feature.contains("source") ||
            !feature["source"].is_string()) {
            return fail("a segmentation feature names no source column");
        }
        SegmentationFeature parsed;
        parsed.source = feature["source"].get<std::string>();
        parsed.encoded_name = feature.value("encoded_name", parsed.source);
        const std::string kind = feature.value("kind", std::string("numeric"));
        if (kind != "numeric" && kind != "categorical") {
            return fail("segmentation feature '" + parsed.encoded_name +
                        "' has an unknown kind '" + kind + "'");
        }
        parsed.kind = kind == "numeric" ? SegmentationFeatureKind::Numeric
                                        : SegmentationFeatureKind::Categorical;
        parsed.level = feature.value("level", std::string());
        parsed.location = feature.value("location", 0.0);
        parsed.scale = feature.value("scale", 1.0);
        parsed.robust = feature.value("robust", false);
        parsed.rare_bucket = feature.value("rare_bucket", false);
        if (parsed.kind == SegmentationFeatureKind::Numeric &&
            (!std::isfinite(parsed.scale) || parsed.scale == 0.0 ||
             !std::isfinite(parsed.location))) {
            return fail("segmentation feature '" + parsed.encoded_name +
                        "' has an invalid scale or location");
        }
        model.features.push_back(std::move(parsed));
    }
    for (const nlohmann::json& center : *centers) {
        if (!center.is_array() || center.size() != model.features.size()) {
            return fail("a segmentation centre does not have one value per feature");
        }
        std::vector<double> values;
        for (const nlohmann::json& value : center) {
            if (!value.is_number()) return fail("a segmentation centre value is not a number");
            values.push_back(value.get<double>());
        }
        model.centers.push_back(std::move(values));
    }
    if (const auto excluded = body.find("excluded_columns");
        excluded != body.end() && excluded->is_array()) {
        for (const nlohmann::json& column : *excluded) {
            if (column.is_string()) model.excluded_columns.push_back(column.get<std::string>());
        }
    }
    if (const auto labels = body.find("labels"); labels != body.end() && labels->is_array()) {
        for (const nlohmann::json& label : *labels) {
            model.labels.push_back(label.is_string() ? label.get<std::string>() : std::string());
        }
    }
    model.seed = body.value("seed", std::uint64_t{0});
    return model;
}

std::optional<SegmentationModel> load_segmentation_model(const Model& model,
                                                         std::string* error) {
    const std::optional<std::string> body = model.read_weights();
    if (!body.has_value()) {
        if (error != nullptr) *error = "the model's weights are not present on this member";
        return std::nullopt;
    }
    const nlohmann::json value = nlohmann::json::parse(*body, nullptr, false);
    if (value.is_discarded()) {
        if (error != nullptr) *error = "the model's weights are not valid JSON";
        return std::nullopt;
    }
    return segmentation_model_from_json(value, error);
}

std::vector<std::string> segmentation_source_columns(const SegmentationModel& model) {
    std::vector<std::string> columns;
    for (const SegmentationFeature& feature : model.features) {
        if (std::find(columns.begin(), columns.end(), feature.source) == columns.end()) {
            columns.push_back(feature.source);
        }
    }
    return columns;
}

SegmentationEncoding encode_segmentation_row(const SegmentationModel& model,
                                             const nlohmann::json& row) {
    SegmentationEncoding out;
    if (!row.is_object()) {
        out.error = "is not an object";
        return out;
    }
    out.point.reserve(model.features.size());
    for (std::size_t index = 0; index < model.features.size();) {
        const SegmentationFeature& feature = model.features[index];
        const auto found = row.find(feature.source);
        if (found == row.end() || found->is_null()) {
            out.error = "has no '" + feature.source + "'";
            out.point.clear();
            return out;
        }
        if (feature.kind == SegmentationFeatureKind::Numeric) {
            double number = 0.0;
            if (found->is_boolean()) {
                number = found->get<bool>() ? 1.0 : 0.0;
            } else if (found->is_number() && std::isfinite(found->get<double>())) {
                number = found->get<double>();
            } else {
                out.error = "has a non-numeric '" + feature.source + "'";
                out.point.clear();
                return out;
            }
            out.point.push_back((number - feature.location) / feature.scale);
            ++index;
            continue;
        }
        // A categorical column: its consecutive levels, the last one the rare
        // bucket (transform_rows writes them in this order).
        if (!found->is_string()) {
            out.error = "has a non-text '" + feature.source + "'";
            out.point.clear();
            return out;
        }
        const std::string& category = found->get_ref<const std::string&>();
        std::size_t end = index;
        while (end < model.features.size() &&
               model.features[end].source == feature.source &&
               model.features[end].kind == SegmentationFeatureKind::Categorical) {
            ++end;
        }
        bool known = false;
        for (std::size_t level = index; level < end; ++level) {
            if (!model.features[level].rare_bucket &&
                model.features[level].level == category) {
                known = true;
            }
        }
        for (std::size_t level = index; level < end; ++level) {
            const SegmentationFeature& indicator = model.features[level];
            const bool selected =
                indicator.rare_bucket ? !known : indicator.level == category;
            out.point.push_back(selected ? 1.0 : 0.0);
        }
        index = end;
    }
    return out;
}

SegmentPlacement place_segmentation_point(const SegmentationModel& model,
                                          const std::vector<double>& point) {
    const Nearest nearest = nearest_two(model, point);
    return {.segment_id = nearest.best, .distance = std::sqrt(nearest.best_squared)};
}

SegmentationScoreResult score_segmentation_rows(
    const Model& model, const std::vector<nlohmann::json>& rows,
    const std::vector<std::int64_t>& entities,
    std::shared_ptr<const data::EntityKeyMapping> entity_keys) {
    SegmentationScoreResult result;
    if (rows.empty()) {
        result.error = "score: no rows given";
        return result;
    }
    if (!entities.empty() && entities.size() != rows.size()) {
        result.error = "score: " + std::to_string(entities.size()) + " entity keys for " +
                       std::to_string(rows.size()) + " rows";
        return result;
    }
    std::string weights_error;
    const std::optional<SegmentationModel> segments =
        load_segmentation_model(model, &weights_error);
    if (!segments) {
        result.error = "score: segmentation model '" + model.id() +
                       "' cannot be read: " + weights_error;
        return result;
    }
    result.feature_columns = segmentation_source_columns(*segments);
    result.labels = labels_of(*segments);
    result.entity_keys = std::move(entity_keys);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const SegmentationEncoding encoded = encode_segmentation_row(*segments, rows[row]);
        if (!encoded.ok()) {
            result.error = "score: row " + std::to_string(row) + " " + encoded.error +
                           "; the model needs " + join(result.feature_columns);
            result.assignments.clear();
            return result;
        }
        const SegmentPlacement placed = place_segmentation_point(*segments, encoded.point);
        result.assignments.push_back(
            {.entity = entities.empty() ? static_cast<std::int64_t>(row) : entities[row],
             .segment_id = placed.segment_id,
             .distance = placed.distance});
    }
    result.rows_scored = rows.size();
    return result;
}

SegmentationScoreResult score_segmentation_table(Node& node, const Model& model,
                                                 const data::ResolvedDataset& dataset) {
    SegmentationScoreResult result;
    if (dataset.contract.entity_column.empty()) {
        result.error = "score: the dataset contract names no entity column";
        return result;
    }
    std::string weights_error;
    const std::optional<SegmentationModel> segments =
        load_segmentation_model(model, &weights_error);
    if (!segments) {
        result.error = "score: segmentation model '" + model.id() +
                       "' cannot be read: " + weights_error;
        return result;
    }
    result.feature_columns = segmentation_source_columns(*segments);
    result.labels = labels_of(*segments);
    result.entity_keys = dataset.contract.entity_keys;

    data::ResolvedDataset reading = dataset;
    // The model's columns, not the contract's: a column the table has gained
    // since training is not read, and one it lost is a hard failure.
    reading.contract.feature_columns = result.feature_columns;
    std::vector<SegmentationRow> rows;
    std::string read_error;
    if (!read_segmentation_rows(node, reading, rows, result.partitions_read, read_error)) {
        result.error = "score: could not read '" + dataset.database + "." + dataset.table +
                       "' with the model's columns (" + join(result.feature_columns) +
                       "): " + read_error;
        return result;
    }
    for (const SegmentationRow& row : rows) {
        const SegmentationEncoding encoded = encode_segmentation_row(*segments, row.values);
        if (!encoded.ok()) {
            result.error = "score: entity " + std::to_string(row.entity) + " " +
                           encoded.error + "; the model needs " +
                           join(result.feature_columns);
            result.assignments.clear();
            return result;
        }
        const SegmentPlacement placed = place_segmentation_point(*segments, encoded.point);
        result.assignments.push_back(
            {.entity = row.entity, .segment_id = placed.segment_id, .distance = placed.distance});
    }
    result.rows_scored = rows.size();
    return result;
}

namespace {

SegmentExplanation explain_values(const SegmentationModel& segments,
                                  const nlohmann::json& row,
                                  const std::vector<double>& point) {
    SegmentExplanation out;
    const Nearest nearest = nearest_two(segments, point);
    out.segment_id = nearest.best;
    out.distance = std::sqrt(nearest.best_squared);
    if (const std::vector<std::string> labels = labels_of(segments); !labels.empty()) {
        out.label = labels[static_cast<std::size_t>(nearest.best)];
    }
    const std::vector<std::string> columns = segmentation_source_columns(segments);
    std::vector<double> totals(columns.size(), 0.0);
    if (nearest.second >= 0) {
        out.runner_up_segment_id = nearest.second;
        out.runner_up_distance = std::sqrt(nearest.second_squared);
        const std::vector<double>& own = segments.centers[static_cast<std::size_t>(nearest.best)];
        const std::vector<double>& other =
            segments.centers[static_cast<std::size_t>(nearest.second)];
        for (std::size_t index = 0; index < segments.features.size(); ++index) {
            const auto column = std::find(columns.begin(), columns.end(),
                                          segments.features[index].source);
            const double to_other = point[index] - other[index];
            const double to_own = point[index] - own[index];
            totals[static_cast<std::size_t>(column - columns.begin())] +=
                to_other * to_other - to_own * to_own;
        }
    }
    double sum = 0.0;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        sum += totals[index];
        out.contributions.push_back({.feature = columns[index],
                                     .value = row.value(columns[index], nlohmann::json()),
                                     .contribution = totals[index]});
    }
    std::stable_sort(out.contributions.begin(), out.contributions.end(),
                     [](const SegmentContribution& left, const SegmentContribution& right) {
                         return std::abs(left.contribution) > std::abs(right.contribution);
                     });
    out.residual = nearest.second >= 0
                       ? (nearest.second_squared - nearest.best_squared) - sum
                       : 0.0;
    return out;
}

}  // namespace

SegmentExplanation explain_segmentation_row(const Model& model, const nlohmann::json& row) {
    SegmentExplanation out;
    std::string weights_error;
    const std::optional<SegmentationModel> segments =
        load_segmentation_model(model, &weights_error);
    if (!segments) {
        out.error = "explain: segmentation model '" + model.id() +
                    "' cannot be read: " + weights_error;
        return out;
    }
    const SegmentationEncoding encoded = encode_segmentation_row(*segments, row);
    if (!encoded.ok()) {
        out.error = "explain: the row " + encoded.error + "; the model needs " +
                    join(segmentation_source_columns(*segments));
        return out;
    }
    return explain_values(*segments, row, encoded.point);
}

SegmentExplanation explain_segmentation_entity(Node& node, const Model& model,
                                               const data::ResolvedDataset& dataset,
                                               std::int64_t entity) {
    SegmentExplanation out;
    if (dataset.contract.entity_column.empty()) {
        out.error = "explain: the dataset contract names no entity column";
        return out;
    }
    std::string weights_error;
    const std::optional<SegmentationModel> segments =
        load_segmentation_model(model, &weights_error);
    if (!segments) {
        out.error = "explain: segmentation model '" + model.id() +
                    "' cannot be read: " + weights_error;
        return out;
    }
    data::ResolvedDataset reading = dataset;
    reading.contract.feature_columns = segmentation_source_columns(*segments);
    std::vector<SegmentationRow> rows;
    std::size_t partitions = 0;
    std::string read_error;
    if (!read_segmentation_rows(node, reading, rows, partitions, read_error)) {
        out.error = "explain: could not read '" + dataset.database + "." + dataset.table +
                    "': " + read_error;
        return out;
    }
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const SegmentationRow& row) {
        return row.entity == entity;
    });
    if (found == rows.end()) {
        out.error = "explain: entity " + std::to_string(entity) +
                    " is not in the rows this member holds";
        return out;
    }
    const SegmentationEncoding encoded = encode_segmentation_row(*segments, found->values);
    if (!encoded.ok()) {
        out.error = "explain: entity " + std::to_string(entity) + " " + encoded.error;
        return out;
    }
    out = explain_values(*segments, found->values, encoded.point);
    out.entity = entity;
    return out;
}

}  // namespace zetta::ml
