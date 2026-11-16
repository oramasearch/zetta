// SPDX-License-Identifier: MIT

#include "zetta/ml/segmentation.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>

#include "zetta/runtime.hpp"

namespace zetta::ml {
namespace {

constexpr double kEpsilon = 1e-12;
constexpr int kInitializationRounds = 5;

struct PreparedData {
    std::vector<std::int64_t> entities;
    std::vector<std::vector<double>> points;
    std::vector<SegmentationFeature> features;
    std::vector<std::string> excluded;
    compute::ComputeStatus status;
};

struct SamplePoint {
    std::uint64_t hash = 0;
    std::int64_t entity = 0;
    std::vector<double> point;
};

struct LloydResult {
    std::vector<std::vector<double>> centers;
    std::vector<double> counts;
    std::vector<int> sample_assignments;
    double inertia = 0.0;
    compute::ComputeStatus status;
};

struct CandidateForK {
    LloydResult fit;
    SegmentationKMetric metric;
    double stability = 1.0;
};

std::string lowered(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

bool same_name(std::string_view left, std::string_view right) {
    return lowered(left) == lowered(right);
}

bool is_identifier_name(std::string_view name) {
    const std::string value = lowered(name);
    return value == "id" || value == "entity" || value == "uuid" ||
           value == "key" || value.ends_with("_id") ||
           value.ends_with("_uuid") || value.ends_with("_key");
}

std::uint64_t mix_hash(std::uint64_t hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t stable_hash(std::uint64_t seed, std::int64_t entity,
                          std::uint64_t salt = 0) {
    std::uint64_t hash = 1469598103934665603ull;
    hash = mix_hash(hash, seed);
    hash = mix_hash(hash, static_cast<std::uint64_t>(entity));
    hash = mix_hash(hash, salt);
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebull;
    return hash ^ (hash >> 31);
}

double unit_hash(std::uint64_t seed, std::int64_t entity, std::uint64_t salt) {
    return static_cast<double>(stable_hash(seed, entity, salt) >> 11) /
           static_cast<double>(1ull << 53);
}

void append_uint64(std::vector<double> &encoded, std::uint64_t value) {
    encoded.push_back(static_cast<double>(value >> 32));
    encoded.push_back(static_cast<double>(value & 0xffffffffull));
}

std::uint64_t read_uint64(const std::vector<double> &encoded,
                          std::size_t offset) {
    const auto high = static_cast<std::uint64_t>(encoded[offset]);
    const auto low = static_cast<std::uint64_t>(encoded[offset + 1]);
    return (high << 32) | low;
}

void append_int64(std::vector<double> &encoded, std::int64_t value) {
    append_uint64(encoded, static_cast<std::uint64_t>(value));
}

std::int64_t read_int64(const std::vector<double> &encoded,
                        std::size_t offset) {
    return static_cast<std::int64_t>(read_uint64(encoded, offset));
}

std::optional<std::vector<double>>
reduce_values(const SegmentationCollectives &collectives,
              const std::string &tag, const std::vector<double> &local,
              SegmentationReduce operation) {
    if (!collectives.allreduce)
        return local;
    return collectives.allreduce(tag, local, operation);
}

std::optional<std::vector<std::vector<double>>>
gather_values(const SegmentationCollectives &collectives,
              const std::string &tag, const std::vector<double> &local) {
    if (!collectives.gather)
        return std::vector<std::vector<double>>{local};
    return collectives.gather(tag, local);
}

std::optional<std::vector<double>>
broadcast_values(const SegmentationCollectives &collectives,
                 const std::string &tag,
                 const std::vector<double> &root_value) {
    if (!collectives.broadcast)
        return root_value;
    return collectives.broadcast(tag, root_value);
}

bool unique_entities(const std::vector<SegmentationRow> &rows,
                     const SegmentationCollectives &collectives,
                     const std::string &tag) {
    // Exact uniqueness validation coordinates integer keys only, never
    // features.
    std::vector<double> keys;
    for (const auto &row : rows)
        append_int64(keys, row.entity);
    const auto gathered = gather_values(collectives, tag + ".gather", keys);
    if (!gathered)
        return false;
    std::set<std::int64_t> seen;
    bool unique = true;
    for (const auto &contribution : *gathered) {
        if (contribution.size() % 2 != 0)
            unique = false;
        for (std::size_t offset = 0; offset + 1 < contribution.size();
             offset += 2) {
            if (!seen.insert(read_int64(contribution, offset)).second)
                unique = false;
        }
    }
    const auto agreed = broadcast_values(
        collectives, tag + ".broadcast",
        gathered->empty() ? std::vector<double>{}
                          : std::vector<double>{unique ? 1.0 : 0.0});
    return agreed && agreed->size() == 1 && (*agreed)[0] == 1.0;
}

double squared_distance(const std::vector<double> &left,
                        const std::vector<double> &right) {
    double result = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const double delta = left[index] - right[index];
        result += delta * delta;
    }
    return result;
}

int nearest_center(const std::vector<double> &point,
                   const std::vector<std::vector<double>> &centers,
                   double *squared = nullptr) {
    int best = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < centers.size(); ++index) {
        const double distance = squared_distance(point, centers[index]);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(index);
        }
    }
    if (squared != nullptr)
        *squared = best_distance;
    return best;
}

std::vector<int> assign_points(const std::vector<std::vector<double>> &points,
                               const std::vector<std::vector<double>> &centers,
                               std::vector<double> &distances) {
    std::vector<int> labels(points.size());
    distances.resize(points.size());
    // Independent distances fan out across CPU workers; reductions below retain
    // input order so thread scheduling cannot alter the result under a seed.
    runtime::parallel_for(points.size(), 64,
                          [&](std::size_t, std::size_t begin, std::size_t end) {
                              for (std::size_t row = begin; row < end; ++row)
                                  labels[row] = nearest_center(
                                      points[row], centers, &distances[row]);
                          });
    return labels;
}

std::uint64_t ordered_bits(double number) {
    const auto bits = std::bit_cast<std::uint64_t>(number);
    constexpr std::uint64_t sign = 1ull << 63;
    return (bits & sign) ? ~bits : bits ^ sign;
}

double from_ordered_bits(std::uint64_t bits) {
    constexpr std::uint64_t sign = 1ull << 63;
    return std::bit_cast<double>((bits & sign) ? bits ^ sign : ~bits);
}

// Exact interpolated quartiles by distributed rank selection. Only six counts
// per search round cross the network; even extreme outliers cannot flatten IQR.
std::optional<std::vector<double>>
global_quartiles(const std::vector<double> &numbers, double minimum,
                 double maximum, double total,
                 const SegmentationCollectives &collectives,
                 const std::string &tag) {
    std::vector<std::uint64_t> sorted;
    sorted.reserve(numbers.size());
    for (double number : numbers)
        sorted.push_back(ordered_bits(number));
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::uint64_t> low(6, ordered_bits(minimum));
    std::vector<std::uint64_t> high(6, ordered_bits(maximum));
    std::vector<double> ranks;
    for (double fraction : {0.25, 0.5, 0.75}) {
        const double rank = fraction * (total - 1.0);
        ranks.push_back(std::floor(rank));
        ranks.push_back(std::ceil(rank));
    }
    for (int round = 0; round < 64 && low != high; ++round) {
        std::vector<double> counts;
        for (std::size_t index = 0; index < low.size(); ++index) {
            const auto middle = std::midpoint(low[index], high[index]);
            counts.push_back(static_cast<double>(
                std::upper_bound(sorted.begin(), sorted.end(), middle) -
                sorted.begin()));
        }
        const auto global =
            reduce_values(collectives, tag + "." + std::to_string(round),
                          counts, SegmentationReduce::Sum);
        if (!global || global->size() != ranks.size())
            return std::nullopt;
        for (std::size_t index = 0; index < low.size(); ++index) {
            const auto middle = std::midpoint(low[index], high[index]);
            if ((*global)[index] > ranks[index])
                high[index] = middle;
            else
                low[index] = middle + 1;
        }
    }
    std::vector<double> result;
    for (std::size_t index = 0; index < 3; ++index) {
        const double rank =
            0.25 * static_cast<double>(index + 1) * (total - 1.0);
        result.push_back(std::lerp(from_ordered_bits(low[2 * index]),
                                   from_ordered_bits(low[2 * index + 1]),
                                   rank - std::floor(rank)));
    }
    return result;
}

std::vector<double>
encode_string_counts(const std::map<std::string, double> &counts) {
    std::vector<double> encoded;
    for (const auto &[value, count] : counts) {
        encoded.push_back(static_cast<double>(value.size()));
        for (const unsigned char byte : value) {
            encoded.push_back(static_cast<double>(byte));
        }
        encoded.push_back(count);
    }
    return encoded;
}

bool decode_string_counts(const std::vector<double> &encoded,
                          std::map<std::string, double> &counts) {
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        if (!std::isfinite(encoded[offset]) || encoded[offset] < 0.0 ||
            encoded[offset] > static_cast<double>(encoded.size()) ||
            std::trunc(encoded[offset]) != encoded[offset])
            return false;
        const std::size_t length = static_cast<std::size_t>(encoded[offset++]);
        if (length > encoded.size() - offset ||
            offset + length >= encoded.size()) {
            return false;
        }
        std::string value;
        value.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            const double byte = encoded[offset++];
            if (!std::isfinite(byte) || byte < 0.0 || byte > 255.0 ||
                std::trunc(byte) != byte)
                return false;
            value.push_back(
                static_cast<char>(static_cast<unsigned char>(byte)));
        }
        if (!std::isfinite(encoded[offset]) || encoded[offset] < 0.0)
            return false;
        counts[value] += encoded[offset++];
    }
    return true;
}

std::optional<std::map<std::string, double>>
global_string_counts(const std::map<std::string, double> &local,
                     const SegmentationCollectives &collectives,
                     const std::string &tag) {
    const auto gathered = gather_values(collectives, tag + ".gather",
                                        encode_string_counts(local));
    if (!gathered)
        return std::nullopt;

    std::vector<double> root_payload;
    if (!gathered->empty()) {
        std::map<std::string, double> combined;
        for (const std::vector<double> &contribution : *gathered) {
            if (!decode_string_counts(contribution, combined))
                return std::nullopt;
        }
        root_payload = encode_string_counts(combined);
    }
    const auto global =
        broadcast_values(collectives, tag + ".broadcast", root_payload);
    if (!global)
        return std::nullopt;
    std::map<std::string, double> result;
    if (!decode_string_counts(*global, result))
        return std::nullopt;
    return result;
}

std::vector<std::string>
infer_local_columns(const std::vector<SegmentationRow> &rows) {
    std::set<std::string> names;
    for (const SegmentationRow &row : rows) {
        if (!row.values.is_object())
            continue;
        for (auto iterator = row.values.begin(); iterator != row.values.end();
             ++iterator) {
            names.insert(iterator.key());
        }
    }
    return {names.begin(), names.end()};
}

struct FeaturePlan {
    std::string source;
    SegmentationFeatureKind kind = SegmentationFeatureKind::Numeric;
    double location = 0.0;
    double scale = 1.0;
    bool robust = false;
    std::vector<std::string> levels;
};

std::optional<FeaturePlan>
fit_numeric_feature(const std::vector<SegmentationRow> &rows,
                    const std::string &column, double global_rows,
                    const SegmentationCollectives &collectives,
                    const std::string &tag, compute::ComputeStatus &status) {
    double local_minimum = std::numeric_limits<double>::max();
    double local_maximum = std::numeric_limits<double>::lowest();
    double local_sum = 0.0;
    std::vector<double> numbers;
    numbers.reserve(rows.size());
    for (const SegmentationRow &row : rows) {
        const nlohmann::json &value = row.values.at(column);
        const double number = value.is_boolean()
                                  ? (value.get<bool>() ? 1.0 : 0.0)
                                  : value.get<double>();
        local_minimum = std::min(local_minimum, number);
        local_maximum = std::max(local_maximum, number);
        local_sum += number;
        numbers.push_back(number);
    }
    const auto minimum =
        reduce_values(collectives, tag + ".min." + column, {local_minimum},
                      SegmentationReduce::Minimum);
    const auto maximum =
        reduce_values(collectives, tag + ".max." + column, {local_maximum},
                      SegmentationReduce::Maximum);
    const auto moments = reduce_values(collectives, tag + ".moments." + column,
                                       {local_sum}, SegmentationReduce::Sum);
    if (!minimum || !maximum || !moments || minimum->empty() ||
        maximum->empty() || moments->size() != 1 ||
        !std::isfinite((*moments)[0])) {
        status = compute::ComputeStatus::backend_error(
            "segmentation: global statistics failed for '" + column + "'");
        return std::nullopt;
    }
    const double global_minimum = (*minimum)[0];
    const double global_maximum = (*maximum)[0];
    const auto quartiles =
        global_quartiles(numbers, global_minimum, global_maximum, global_rows,
                         collectives, tag + ".quartiles." + column);
    if (!quartiles) {
        status = compute::ComputeStatus::backend_error(
            "segmentation: global quartiles failed for '" + column + "'");
        return std::nullopt;
    }
    const double first_quartile = (*quartiles)[0];
    const double median = (*quartiles)[1];
    const double third_quartile = (*quartiles)[2];
    const double interquartile_range = third_quartile - first_quartile;
    const bool heavy_tailed =
        interquartile_range > kEpsilon &&
        (global_maximum - global_minimum) / interquartile_range > 8.0;
    const double mean = (*moments)[0] / global_rows;
    double local_variance = 0.0;
    for (double number : numbers)
        local_variance += (number - mean) * (number - mean);
    const auto variance =
        reduce_values(collectives, tag + ".variance." + column,
                      {local_variance}, SegmentationReduce::Sum);
    if (!variance || variance->empty() || !std::isfinite((*variance)[0])) {
        status = compute::ComputeStatus::invalid_argument(
            "segmentation: numeric range overflow for '" + column + "'");
        return std::nullopt;
    }
    FeaturePlan plan;
    plan.source = column;
    plan.kind = SegmentationFeatureKind::Numeric;
    plan.robust = heavy_tailed;
    plan.location = heavy_tailed ? median : mean;
    plan.scale = heavy_tailed ? interquartile_range
                              : std::sqrt((*variance)[0] / global_rows);
    if (plan.scale <= kEpsilon)
        plan.scale = 1.0;
    return plan;
}

std::optional<FeaturePlan>
fit_categorical_feature(const std::map<std::string, double> &category_counts,
                        const std::string &column, double global_rows,
                        const SegmentationParams &params,
                        const SegmentationCollectives &collectives,
                        const std::string &tag,
                        compute::ComputeStatus &status) {
    if (std::find(params.categorical_columns.begin(),
                  params.categorical_columns.end(),
                  column) == params.categorical_columns.end()) {
        return std::nullopt;
    }
    const auto counts = global_string_counts(category_counts, collectives,
                                             tag + ".categories." + column);
    if (!counts) {
        status = compute::ComputeStatus::backend_error(
            "segmentation: category agreement failed for '" + column + "'");
        return std::nullopt;
    }
    if (counts->size() > params.maximum_category_levels ||
        params.maximum_category_levels < 2) {
        return std::nullopt;
    }
    std::vector<std::pair<std::string, double>> ranked(counts->begin(),
                                                       counts->end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto &left, const auto &right) {
                  if (left.second != right.second)
                      return left.second > right.second;
                  return left.first < right.first;
              });
    FeaturePlan plan;
    plan.source = column;
    plan.kind = SegmentationFeatureKind::Categorical;
    const std::size_t regular_limit = params.maximum_category_levels - 1;
    for (const auto &[level, count] : ranked) {
        if (plan.levels.size() >= regular_limit)
            break;
        if (count / global_rows < params.rare_category_share)
            continue;
        plan.levels.push_back(level);
    }
    std::string rare_level = "other";
    while (counts->contains(rare_level))
        rare_level = "_" + rare_level;
    plan.levels.push_back(rare_level);
    return plan;
}

void plan_feature(const std::vector<SegmentationRow> &rows,
                  const std::string &column, double global_rows,
                  const SegmentationParams &params,
                  const SegmentationCollectives &collectives,
                  const std::string &tag, PreparedData &prepared,
                  std::vector<FeaturePlan> &plans) {

    if (segmentation_excludes_column(column, params)) {
        prepared.excluded.push_back(column);
        return;
    }

    double numeric = 0.0;
    double strings = 0.0;
    double invalid = 0.0;
    double missing = 0.0;
    double nonfinite = 0.0;
    std::map<std::string, double> category_counts;
    for (const SegmentationRow &row : rows) {
        if (!row.values.is_object()) {
            ++missing;
            continue;
        }
        const auto found = row.values.find(column);
        if (found == row.values.end() || found->is_null()) {
            ++missing;
        } else if (found->is_number() || found->is_boolean()) {
            ++numeric;
            if (found->is_number() && !std::isfinite(found->get<double>()))
                ++nonfinite;
        } else if (found->is_string()) {
            ++strings;
            if (category_counts.size() <= params.maximum_category_levels)
                category_counts[found->get<std::string>()] += 1.0;
        } else {
            ++invalid;
        }
    }
    const auto kinds =
        reduce_values(collectives, tag + ".kind." + column,
                      {numeric, strings, invalid, missing, nonfinite},
                      SegmentationReduce::Sum);
    if (!kinds || kinds->size() != 5) {
        prepared.status = compute::ComputeStatus::backend_error(
            "segmentation: feature type allreduce failed for '" + column + "'");
        return;
    }
    const double global_numeric = (*kinds)[0];
    const double global_strings = (*kinds)[1];
    const double global_invalid = (*kinds)[2];
    const double global_missing = (*kinds)[3];
    if ((*kinds)[4] > 0.0) {
        prepared.status = compute::ComputeStatus::invalid_argument(
            "segmentation: non-finite numeric feature '" + column + "'");
        return;
    }
    if (global_invalid > 0.0 || global_missing > 0.0 ||
        (global_numeric > 0.0 && global_strings > 0.0)) {
        prepared.excluded.push_back(column);
        return;
    }

    if (global_numeric == global_rows) {
        auto plan = fit_numeric_feature(rows, column, global_rows, collectives,
                                        tag, prepared.status);
        if (plan)
            plans.push_back(std::move(*plan));
        return;
    }

    if (global_strings == global_rows) {
        auto plan =
            fit_categorical_feature(category_counts, column, global_rows,
                                    params, collectives, tag, prepared.status);
        if (plan)
            plans.push_back(std::move(*plan));
        else
            prepared.excluded.push_back(column);
        return;
    }

    prepared.excluded.push_back(column);
}

void transform_rows(const std::vector<SegmentationRow> &rows,
                    const std::vector<FeaturePlan> &plans,
                    PreparedData &prepared) {
    for (const FeaturePlan &plan : plans) {
        if (plan.kind == SegmentationFeatureKind::Numeric) {
            prepared.features.push_back(
                SegmentationFeature{.source = plan.source,
                                    .encoded_name = plan.source,
                                    .kind = plan.kind,
                                    .location = plan.location,
                                    .scale = plan.scale,
                                    .robust = plan.robust});
        } else {
            for (std::size_t index = 0; index < plan.levels.size(); ++index) {
                const std::string &level = plan.levels[index];
                prepared.features.push_back(SegmentationFeature{
                    .source = plan.source,
                    .encoded_name = plan.source + "=" + level,
                    .kind = plan.kind,
                    .level = level,
                    .rare_bucket = index + 1 == plan.levels.size()});
            }
        }
    }
    if (prepared.features.empty()) {
        prepared.status = compute::ComputeStatus::invalid_argument(
            "segmentation: no usable numeric or bounded categorical features");
        return;
    }

    prepared.entities.reserve(rows.size());
    prepared.points.reserve(rows.size());
    for (const SegmentationRow &row : rows) {
        prepared.entities.push_back(row.entity);
        std::vector<double> point;
        point.reserve(prepared.features.size());
        for (const FeaturePlan &plan : plans) {
            const nlohmann::json &value = row.values.at(plan.source);
            if (plan.kind == SegmentationFeatureKind::Numeric) {
                const double number = value.is_boolean()
                                          ? (value.get<bool>() ? 1.0 : 0.0)
                                          : value.get<double>();
                point.push_back((number - plan.location) / plan.scale);
            } else {
                const std::string category = value.get<std::string>();
                const bool known =
                    std::find(plan.levels.begin(), plan.levels.end() - 1,
                              category) != plan.levels.end() - 1;
                for (std::size_t index = 0; index < plan.levels.size();
                     ++index) {
                    const bool selected = index + 1 == plan.levels.size()
                                              ? !known
                                              : category == plan.levels[index];
                    point.push_back(selected ? 1.0 : 0.0);
                }
            }
        }
        prepared.points.push_back(std::move(point));
    }
}

PreparedData prepare_data(const std::vector<SegmentationRow> &rows,
                          const SegmentationParams &params,
                          const SegmentationCollectives &collectives,
                          const std::string &tag) {
    PreparedData prepared;
    std::vector<std::string> columns = params.feature_columns;
    if (columns.empty()) {
        if (collectives.allreduce || collectives.gather ||
            collectives.broadcast) {
            prepared.status = compute::ComputeStatus::invalid_argument(
                "segmentation: distributed training requires explicit "
                "feature_columns");
            return prepared;
        }
        columns = infer_local_columns(rows);
    }
    if (columns.empty()) {
        prepared.status = compute::ComputeStatus::invalid_argument(
            "segmentation: no candidate feature columns");
        return prepared;
    }

    std::vector<FeaturePlan> plans;
    const double local_rows = static_cast<double>(rows.size());
    const auto global_row_values = reduce_values(
        collectives, tag + ".rows", {local_rows}, SegmentationReduce::Sum);
    if (!global_row_values || global_row_values->empty() ||
        (*global_row_values)[0] < 2.0) {
        prepared.status = compute::ComputeStatus::invalid_argument(
            "segmentation: at least two rows are required globally");
        return prepared;
    }
    const double global_rows = (*global_row_values)[0];

    for (const auto &column : columns) {
        plan_feature(rows, column, global_rows, params, collectives, tag,
                     prepared, plans);
        if (!prepared.status.ok())
            return prepared;
    }
    transform_rows(rows, plans, prepared);
    return prepared;
}

std::vector<double> encode_sample_points(const std::vector<SamplePoint> &points,
                                         std::size_t dimensions) {
    std::vector<double> encoded;
    encoded.push_back(static_cast<double>(points.size()));
    for (const SamplePoint &point : points) {
        append_uint64(encoded, point.hash);
        append_int64(encoded, point.entity);
        encoded.insert(encoded.end(), point.point.begin(), point.point.end());
    }
    if (dimensions == 0)
        encoded.clear();
    return encoded;
}

bool decode_sample_points(const std::vector<double> &encoded,
                          std::size_t dimensions,
                          std::vector<SamplePoint> &points) {
    if (encoded.empty())
        return true;
    if (!std::isfinite(encoded[0]) || encoded[0] < 0.0 ||
        encoded[0] > static_cast<double>(encoded.size()) ||
        std::trunc(encoded[0]) != encoded[0])
        return false;
    const std::size_t count = static_cast<std::size_t>(encoded[0]);
    const std::size_t width = 4 + dimensions;
    if (encoded.size() != 1 + count * width)
        return false;
    std::size_t offset = 1;
    for (std::size_t index = 0; index < count; ++index) {
        for (std::size_t field = 0; field < 4; ++field) {
            const double part = encoded[offset + field];
            if (!std::isfinite(part) || part < 0.0 || part > 0xffffffffull ||
                std::trunc(part) != part)
                return false;
        }
        SamplePoint point;
        point.hash = read_uint64(encoded, offset);
        point.entity = read_int64(encoded, offset + 2);
        offset += 4;
        point.point.assign(
            encoded.begin() + static_cast<std::ptrdiff_t>(offset),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset + dimensions));
        offset += dimensions;
        if (std::any_of(point.point.begin(), point.point.end(),
                        [](double value) {
                            return !std::isfinite(value);
                        }))
            return false;
        points.push_back(std::move(point));
    }
    return true;
}

std::optional<std::vector<SamplePoint>>
make_global_sample(const PreparedData &data, const SegmentationParams &params,
                   const SegmentationCollectives &collectives,
                   const std::string &tag) {
    std::vector<SamplePoint> local;
    local.reserve(data.points.size());
    for (std::size_t row = 0; row < data.points.size(); ++row) {
        local.push_back({stable_hash(params.seed, data.entities[row]),
                         data.entities[row], data.points[row]});
    }
    std::sort(local.begin(), local.end(),
              [](const SamplePoint &left, const SamplePoint &right) {
                  if (left.hash != right.hash)
                      return left.hash < right.hash;
                  return left.entity < right.entity;
              });
    if (local.size() > params.silhouette_sample_size) {
        local.resize(params.silhouette_sample_size);
    }
    const std::size_t dimensions = data.features.size();
    const auto gathered = gather_values(
        collectives, tag + ".gather", encode_sample_points(local, dimensions));
    if (!gathered)
        return std::nullopt;
    std::vector<double> root_payload;
    if (!gathered->empty()) {
        std::vector<SamplePoint> combined;
        for (const std::vector<double> &contribution : *gathered) {
            if (!decode_sample_points(contribution, dimensions, combined)) {
                return std::nullopt;
            }
        }
        std::sort(combined.begin(), combined.end(),
                  [](const SamplePoint &left, const SamplePoint &right) {
                      if (left.hash != right.hash)
                          return left.hash < right.hash;
                      return left.entity < right.entity;
                  });
        combined.erase(
            std::unique(combined.begin(), combined.end(),
                        [](const SamplePoint &left, const SamplePoint &right) {
                            return left.entity == right.entity;
                        }),
            combined.end());
        if (combined.size() > params.silhouette_sample_size) {
            combined.resize(params.silhouette_sample_size);
        }
        root_payload = encode_sample_points(combined, dimensions);
    }
    const auto global =
        broadcast_values(collectives, tag + ".broadcast", root_payload);
    if (!global)
        return std::nullopt;
    std::vector<SamplePoint> result;
    if (!decode_sample_points(*global, dimensions, result))
        return std::nullopt;
    return result;
}

std::optional<std::vector<std::vector<double>>> reduce_candidates(
    const PreparedData &data, const std::vector<SamplePoint> &candidates, int k,
    std::uint64_t seed, const SegmentationCollectives &collectives,
    const std::string &tag) {
    // k-means|| finishes by weighting each candidate by its globally assigned
    // population, then reducing the candidate set with weighted k-means++.
    std::vector<std::vector<double>> candidate_vectors;
    for (const auto &candidate : candidates)
        candidate_vectors.push_back(candidate.point);
    std::vector<double> local_weights(candidates.size(), 0.0);
    for (const auto &point : data.points)
        local_weights[static_cast<std::size_t>(
            nearest_center(point, candidate_vectors))] += 1.0;
    const auto weights = reduce_values(collectives, tag + ".weights",
                                       local_weights, SegmentationReduce::Sum);
    if (!weights || weights->size() != candidates.size())
        return std::nullopt;
    std::vector<std::vector<double>> centers;
    std::set<std::size_t> chosen;
    while (centers.size() < static_cast<std::size_t>(k)) {
        std::vector<double> mass(candidates.size(), 0.0);
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (chosen.contains(index))
                continue;
            double distance = 1.0;
            if (!centers.empty())
                nearest_center(candidates[index].point, centers, &distance);
            mass[index] = (*weights)[index] * distance;
        }
        const double total = std::accumulate(mass.begin(), mass.end(), 0.0);
        double threshold =
            unit_hash(seed, static_cast<std::int64_t>(centers.size()), 101) *
            total;
        std::size_t selected = candidates.size();
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (chosen.contains(index))
                continue;
            selected = index;
            if (total <= kEpsilon || threshold < mass[index])
                break;
            threshold -= mass[index];
        }
        if (selected == candidates.size())
            return std::nullopt;
        centers.push_back(candidates[selected].point);
        chosen.insert(selected);
    }
    return centers;
}

std::optional<std::vector<std::vector<double>>> initialize_centers(
    const PreparedData &data, const std::vector<SamplePoint> &sample, int k,
    std::uint64_t seed, const SegmentationCollectives &collectives,
    const std::string &tag) {
    if (sample.empty())
        return std::nullopt;
    const std::size_t dimensions = data.features.size();
    SegmentationParams initial_params;
    initial_params.seed = seed;
    initial_params.silhouette_sample_size = 1;
    const auto initial =
        make_global_sample(data, initial_params, collectives, tag + ".first");
    if (!initial || initial->empty())
        return std::nullopt;
    std::vector<SamplePoint> candidates = {*initial->begin()};

    for (int round = 0; round < kInitializationRounds; ++round) {
        double local_potential = 0.0;
        std::vector<double> distances(data.points.size(), 0.0);
        std::vector<std::vector<double>> candidate_vectors;
        candidate_vectors.reserve(candidates.size());
        for (const SamplePoint &candidate : candidates) {
            candidate_vectors.push_back(candidate.point);
        }
        assign_points(data.points, candidate_vectors, distances);
        for (std::size_t row = 0; row < data.points.size(); ++row) {
            local_potential += distances[row];
        }
        const auto potential = reduce_values(
            collectives, tag + ".potential." + std::to_string(round),
            {local_potential}, SegmentationReduce::Sum);
        if (!potential || potential->empty())
            return std::nullopt;
        const double global_potential = (*potential)[0];
        std::vector<SamplePoint> selected;
        if (global_potential > kEpsilon) {
            const double oversampling = static_cast<double>(2 * k);
            for (std::size_t row = 0; row < data.points.size(); ++row) {
                const double probability = std::min(
                    1.0, oversampling * distances[row] / global_potential);
                if (unit_hash(seed, data.entities[row],
                              static_cast<std::uint64_t>(round + 1)) <
                    probability) {
                    selected.push_back(
                        {stable_hash(seed, data.entities[row], round + 1),
                         data.entities[row], data.points[row]});
                }
            }
        }
        const auto gathered = gather_values(
            collectives, tag + ".candidates.gather." + std::to_string(round),
            encode_sample_points(selected, dimensions));
        if (!gathered)
            return std::nullopt;
        std::vector<double> root_payload;
        if (!gathered->empty()) {
            std::vector<SamplePoint> combined = candidates;
            for (const std::vector<double> &contribution : *gathered) {
                if (!decode_sample_points(contribution, dimensions, combined)) {
                    return std::nullopt;
                }
            }
            std::sort(combined.begin(), combined.end(),
                      [](const SamplePoint &left, const SamplePoint &right) {
                          if (left.entity != right.entity)
                              return left.entity < right.entity;
                          return left.hash < right.hash;
                      });
            combined.erase(std::unique(combined.begin(), combined.end(),
                                       [](const SamplePoint &left,
                                          const SamplePoint &right) {
                                           return left.entity == right.entity;
                                       }),
                           combined.end());
            root_payload = encode_sample_points(combined, dimensions);
        }
        const auto agreed = broadcast_values(
            collectives, tag + ".candidates.broadcast." + std::to_string(round),
            root_payload);
        if (!agreed)
            return std::nullopt;
        candidates.clear();
        if (!decode_sample_points(*agreed, dimensions, candidates))
            return std::nullopt;
    }

    for (const SamplePoint &point : sample) {
        if (candidates.size() >= static_cast<std::size_t>(k))
            break;
        const auto found =
            std::find_if(candidates.begin(), candidates.end(),
                         [&](const SamplePoint &candidate) {
                             return candidate.entity == point.entity;
                         });
        if (found == candidates.end())
            candidates.push_back(point);
    }
    if (candidates.size() < static_cast<std::size_t>(k))
        return std::nullopt;

    return reduce_candidates(data, candidates, k, seed, collectives, tag);
}

LloydResult run_lloyd(const PreparedData &data,
                      const std::vector<SamplePoint> &sample,
                      std::vector<std::vector<double>> centers,
                      const SegmentationParams &params,
                      const SegmentationCollectives &collectives,
                      const std::string &tag) {
    LloydResult result;
    const std::size_t dimensions = data.features.size();
    const std::size_t k = centers.size();
    for (int iteration = 0; iteration < params.max_iterations; ++iteration) {
        std::vector<double> partial(k * (dimensions + 1) + 1, 0.0);
        std::vector<double> distances;
        const auto labels = assign_points(data.points, centers, distances);
        for (std::size_t row = 0; row < data.points.size(); ++row) {
            const auto &point = data.points[row];
            const int cluster = labels[row];
            const std::size_t base =
                static_cast<std::size_t>(cluster) * (dimensions + 1);
            partial[base] += 1.0;
            for (std::size_t feature = 0; feature < dimensions; ++feature) {
                partial[base + 1 + feature] += point[feature];
            }
            partial.back() += distances[row];
        }
        const auto global = reduce_values(
            collectives, tag + ".lloyd." + std::to_string(iteration), partial,
            SegmentationReduce::Sum);
        if (!global || global->size() != partial.size()) {
            result.status = compute::ComputeStatus::backend_error(
                "segmentation: Lloyd allreduce failed");
            return result;
        }
        std::vector<std::vector<double>> next = centers;
        std::vector<double> counts(k, 0.0);
        for (std::size_t cluster = 0; cluster < k; ++cluster) {
            const std::size_t base = cluster * (dimensions + 1);
            counts[cluster] = (*global)[base];
            if (counts[cluster] <= 0.0)
                continue;
            for (std::size_t feature = 0; feature < dimensions; ++feature) {
                next[cluster][feature] =
                    (*global)[base + 1 + feature] / counts[cluster];
            }
        }

        std::set<std::int64_t> replacements;
        for (std::size_t cluster = 0; cluster < k; ++cluster) {
            if (counts[cluster] > 0.0)
                continue;
            const SamplePoint *farthest = nullptr;
            double farthest_distance = -1.0;
            for (const SamplePoint &candidate : sample) {
                if (replacements.contains(candidate.entity))
                    continue;
                double distance = 0.0;
                nearest_center(candidate.point, next, &distance);
                if (distance > farthest_distance + kEpsilon ||
                    (std::abs(distance - farthest_distance) <= kEpsilon &&
                     (farthest == nullptr ||
                      candidate.entity < farthest->entity))) {
                    farthest = &candidate;
                    farthest_distance = distance;
                }
            }
            if (farthest == nullptr) {
                result.status = compute::ComputeStatus::invalid_argument(
                    "segmentation: cannot deterministically replace an empty "
                    "cluster");
                return result;
            }
            next[cluster] = farthest->point;
            replacements.insert(farthest->entity);
        }

        double maximum_shift = 0.0;
        for (std::size_t cluster = 0; cluster < k; ++cluster) {
            maximum_shift = std::max(
                maximum_shift,
                std::sqrt(squared_distance(centers[cluster], next[cluster])));
        }
        centers = std::move(next);
        if (maximum_shift <= params.center_shift_tolerance &&
            replacements.empty())
            break;
    }

    // Canonical ids keep a recovered solution comparable across restarts.
    std::sort(centers.begin(), centers.end());
    std::vector<double> local_counts(k, 0.0);
    double local_inertia = 0.0;
    for (const std::vector<double> &point : data.points) {
        double distance = 0.0;
        const int cluster = nearest_center(point, centers, &distance);
        local_counts[static_cast<std::size_t>(cluster)] += 1.0;
        local_inertia += distance;
    }
    local_counts.push_back(local_inertia);
    const auto final = reduce_values(collectives, tag + ".final", local_counts,
                                     SegmentationReduce::Sum);
    if (!final || final->size() != k + 1) {
        result.status = compute::ComputeStatus::backend_error(
            "segmentation: final assignment allreduce failed");
        return result;
    }
    result.centers = std::move(centers);
    result.counts.assign(final->begin(), final->end() - 1);
    result.inertia = final->back();
    for (const SamplePoint &point : sample) {
        result.sample_assignments.push_back(
            nearest_center(point.point, result.centers));
    }
    return result;
}

double sample_silhouette(const std::vector<SamplePoint> &sample,
                         const std::vector<int> &assignments, int k) {
    if (sample.size() < 2 || assignments.size() != sample.size())
        return 0.0;
    double total = 0.0;
    for (std::size_t row = 0; row < sample.size(); ++row) {
        std::vector<double> sums(static_cast<std::size_t>(k), 0.0);
        std::vector<std::size_t> counts(static_cast<std::size_t>(k), 0);
        for (std::size_t other = 0; other < sample.size(); ++other) {
            if (row == other)
                continue;
            const int cluster = assignments[other];
            sums[static_cast<std::size_t>(cluster)] += std::sqrt(
                squared_distance(sample[row].point, sample[other].point));
            ++counts[static_cast<std::size_t>(cluster)];
        }
        const int own = assignments[row];
        if (counts[static_cast<std::size_t>(own)] == 0)
            continue;
        const double within =
            sums[static_cast<std::size_t>(own)] /
            static_cast<double>(counts[static_cast<std::size_t>(own)]);
        double nearest = std::numeric_limits<double>::infinity();
        for (int cluster = 0; cluster < k; ++cluster) {
            if (cluster == own ||
                counts[static_cast<std::size_t>(cluster)] == 0)
                continue;
            nearest = std::min(
                nearest, sums[static_cast<std::size_t>(cluster)] /
                             static_cast<double>(
                                 counts[static_cast<std::size_t>(cluster)]));
        }
        if (!std::isfinite(nearest) || std::max(nearest, within) <= kEpsilon)
            continue;
        total += (nearest - within) / std::max(nearest, within);
    }
    return total / static_cast<double>(sample.size());
}

double sample_davies_bouldin(const std::vector<SamplePoint> &sample,
                             const std::vector<int> &assignments,
                             const std::vector<std::vector<double>> &centers) {
    const std::size_t k = centers.size();
    std::vector<double> scatter(k, 0.0);
    std::vector<double> counts(k, 0.0);
    for (std::size_t row = 0; row < sample.size(); ++row) {
        const std::size_t cluster = static_cast<std::size_t>(assignments[row]);
        scatter[cluster] +=
            std::sqrt(squared_distance(sample[row].point, centers[cluster]));
        counts[cluster] += 1.0;
    }
    for (std::size_t cluster = 0; cluster < k; ++cluster) {
        if (counts[cluster] > 0.0)
            scatter[cluster] /= counts[cluster];
    }
    double sum = 0.0;
    for (std::size_t left = 0; left < k; ++left) {
        double worst = 0.0;
        for (std::size_t right = 0; right < k; ++right) {
            if (left == right)
                continue;
            const double separation =
                std::sqrt(squared_distance(centers[left], centers[right]));
            if (separation <= kEpsilon) {
                worst = std::numeric_limits<double>::infinity();
                break;
            }
            worst =
                std::max(worst, (scatter[left] + scatter[right]) / separation);
        }
        sum += worst;
    }
    return sum / static_cast<double>(k);
}

double choose2(double value) {
    return value < 2.0 ? 0.0 : value * (value - 1.0) / 2.0;
}

std::vector<double>
global_encoded_mean(const PreparedData &data,
                    const SegmentationCollectives &collectives,
                    const std::string &tag) {
    std::vector<double> partial(data.features.size() + 1, 0.0);
    for (const std::vector<double> &point : data.points) {
        for (std::size_t feature = 0; feature < point.size(); ++feature) {
            partial[feature] += point[feature];
        }
        partial.back() += 1.0;
    }
    const auto global =
        reduce_values(collectives, tag, partial, SegmentationReduce::Sum);
    if (!global || global->size() != partial.size() || global->back() <= 0.0)
        return {};
    std::vector<double> mean(data.features.size(), 0.0);
    for (std::size_t feature = 0; feature < mean.size(); ++feature) {
        mean[feature] = (*global)[feature] / global->back();
    }
    return mean;
}

std::vector<SegmentProfile>
build_profiles(const SegmentationModel &model,
               const std::vector<double> &counts,
               const std::vector<double> &overall_mean) {
    const double total = std::accumulate(counts.begin(), counts.end(), 0.0);
    std::vector<SegmentProfile> profiles;
    for (std::size_t cluster = 0; cluster < model.centers.size(); ++cluster) {
        SegmentProfile profile;
        profile.segment_id = static_cast<int>(cluster);
        profile.size =
            static_cast<std::uint64_t>(std::llround(counts[cluster]));
        profile.share = total > 0.0 ? counts[cluster] / total : 0.0;

        std::map<std::string, std::vector<std::size_t>> by_source;
        for (std::size_t feature = 0; feature < model.features.size();
             ++feature) {
            by_source[model.features[feature].source].push_back(feature);
        }
        for (const auto &[source, indexes] : by_source) {
            const SegmentationFeature &first = model.features[indexes.front()];
            if (first.kind == SegmentationFeatureKind::Numeric) {
                profile.center[source] =
                    first.location +
                    first.scale * model.centers[cluster][indexes.front()];
            } else {
                const SegmentationFeature *mode = &first;
                double best = -1.0;
                for (const std::size_t index : indexes) {
                    if (model.centers[cluster][index] > best) {
                        best = model.centers[cluster][index];
                        mode = &model.features[index];
                    }
                }
                profile.center[source] = mode->level;
            }
        }

        std::vector<std::pair<std::size_t, double>> deviations;
        for (std::size_t feature = 0; feature < model.features.size();
             ++feature) {
            deviations.emplace_back(feature,
                                    std::abs(model.centers[cluster][feature] -
                                             overall_mean[feature]));
        }
        std::sort(deviations.begin(), deviations.end(),
                  [&](const auto &left, const auto &right) {
                      if (left.second != right.second)
                          return left.second > right.second;
                      return model.features[left.first].encoded_name <
                             model.features[right.first].encoded_name;
                  });
        std::set<std::string> named_sources;
        for (const auto &[index, deviation] : deviations) {
            if (deviation <= kEpsilon || named_sources.size() >= 2)
                break;
            const SegmentationFeature &feature = model.features[index];
            if (!named_sources.insert(feature.source).second)
                continue;
            std::string phrase;
            if (feature.kind == SegmentationFeatureKind::Numeric) {
                const bool high =
                    model.centers[cluster][index] >= overall_mean[index];
                phrase = std::string(high ? "high " : "low ") + feature.source;
            } else {
                const bool high =
                    model.centers[cluster][index] >= overall_mean[index];
                phrase = std::string(high ? "high " : "low ") + feature.source +
                         "=" + feature.level;
            }
            profile.distinguishing_features.push_back(phrase);
        }
        if (profile.distinguishing_features.empty()) {
            profile.label = "typical across measured features";
        } else {
            for (std::size_t index = 0;
                 index < profile.distinguishing_features.size(); ++index) {
                if (index != 0)
                    profile.label += ", ";
                profile.label += profile.distinguishing_features[index];
            }
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::optional<CandidateForK>
fit_candidate(const PreparedData &prepared,
              const std::vector<SamplePoint> &sample, int k,
              const SegmentationParams &params,
              const SegmentationCollectives &collectives,
              const std::string &round_tag, compute::ComputeStatus &status) {

    std::optional<LloydResult> best;
    std::vector<std::vector<int>> restart_assignments;
    for (int restart = 0; restart < params.restarts; ++restart) {
        const std::uint64_t restart_seed =
            mix_hash(params.seed, static_cast<std::uint64_t>(restart + 1));
        const std::string tag = round_tag + ".k" + std::to_string(k) + ".r" +
                                std::to_string(restart);
        const auto centers = initialize_centers(
            prepared, sample, k, restart_seed, collectives, tag + ".init");
        if (!centers) {
            status = compute::ComputeStatus::backend_error(
                "segmentation: k-means|| initialization failed");
            return std::nullopt;
        }
        LloydResult fit =
            run_lloyd(prepared, sample, *centers, params, collectives, tag);
        if (!fit.status.ok()) {
            status = fit.status;
            return std::nullopt;
        }
        restart_assignments.push_back(fit.sample_assignments);
        if (!best || fit.inertia < best->inertia)
            best = std::move(fit);
    }
    double stability = 1.0;
    std::size_t stability_pairs = 0;
    if (restart_assignments.size() > 1) {
        stability = 0.0;
        for (std::size_t left = 0; left < restart_assignments.size(); ++left) {
            for (std::size_t right = left + 1;
                 right < restart_assignments.size(); ++right) {
                stability += adjusted_rand_index(restart_assignments[left],
                                                 restart_assignments[right]);
                ++stability_pairs;
            }
        }
        stability /= static_cast<double>(stability_pairs);
    }
    const double total =
        std::accumulate(best->counts.begin(), best->counts.end(), 0.0);
    const double minimum_count =
        *std::min_element(best->counts.begin(), best->counts.end());
    SegmentationKMetric metric;
    metric.k = k;
    metric.inertia = best->inertia;
    metric.silhouette = sample_silhouette(sample, best->sample_assignments, k);
    metric.davies_bouldin =
        sample_davies_bouldin(sample, best->sample_assignments, best->centers);
    metric.minimum_share = total > 0.0 ? minimum_count / total : 0.0;
    metric.satisfies_minimum_share =
        k == 2 ||
        metric.minimum_share + kEpsilon >= params.minimum_segment_share;
    return CandidateForK{std::move(*best), metric, stability};
}

const CandidateForK *
choose_candidate(const std::vector<CandidateForK> &candidates) {
    const CandidateForK *chosen = nullptr;
    for (const CandidateForK &candidate : candidates) {
        if (!candidate.metric.satisfies_minimum_share ||
            candidate.metric.minimum_share <= 0.0 ||
            !std::isfinite(candidate.metric.davies_bouldin) ||
            !std::isfinite(candidate.metric.inertia) ||
            !std::isfinite(candidate.metric.silhouette))
            continue;
        if (chosen == nullptr ||
            candidate.metric.silhouette > chosen->metric.silhouette + 1e-9 ||
            (std::abs(candidate.metric.silhouette -
                      chosen->metric.silhouette) <= 1e-9 &&
             candidate.metric.davies_bouldin <
                 chosen->metric.davies_bouldin - 1e-9) ||
            (std::abs(candidate.metric.silhouette -
                      chosen->metric.silhouette) <= 1e-9 &&
             std::abs(candidate.metric.davies_bouldin -
                      chosen->metric.davies_bouldin) <= 1e-9 &&
             candidate.metric.k < chosen->metric.k)) {
            chosen = &candidate;
        }
    }
    return chosen;
}

} // namespace

bool segmentation_excludes_column(const std::string &column,
                                  const SegmentationParams &params) {
    const std::string name = lowered(column);
    return same_name(column, params.entity_column) ||
           same_name(column, params.label_column) ||
           is_identifier_name(column) || name == "label" || name == "target" ||
           name == "outcome" ||
           std::any_of(params.excluded_columns.begin(),
                       params.excluded_columns.end(),
                       [&](const auto &excluded) {
                           return same_name(column, excluded);
                       });
}

nlohmann::json SegmentationModel::to_json() const {
    nlohmann::json encoded_features = nlohmann::json::array();
    for (const SegmentationFeature &feature : features) {
        encoded_features.push_back({
            {"source", feature.source},
            {"encoded_name", feature.encoded_name},
            {"kind", feature.kind == SegmentationFeatureKind::Numeric
                         ? "numeric"
                         : "categorical"},
            {"level", feature.level},
            {"location", feature.location},
            {"scale", feature.scale},
            {"robust", feature.robust},
            {"rare_bucket", feature.rare_bucket},
        });
    }
    nlohmann::json body = {{"kind", "segmentation"},
                           {"seed", seed},
                           {"features", std::move(encoded_features)},
                           {"centers", centers},
                           {"excluded_columns", excluded_columns}};
    if (!labels.empty())
        body["labels"] = labels;
    return body;
}

double adjusted_rand_index(const std::vector<int> &left,
                           const std::vector<int> &right) {
    if (left.size() != right.size() || left.empty())
        return 0.0;
    std::map<std::pair<int, int>, double> cells;
    std::map<int, double> left_counts;
    std::map<int, double> right_counts;
    for (std::size_t index = 0; index < left.size(); ++index) {
        cells[{left[index], right[index]}] += 1.0;
        left_counts[left[index]] += 1.0;
        right_counts[right[index]] += 1.0;
    }
    double paired = 0.0;
    for (const auto &[key, count] : cells) {
        static_cast<void>(key);
        paired += choose2(count);
    }
    double left_pairs = 0.0;
    for (const auto &[key, count] : left_counts) {
        static_cast<void>(key);
        left_pairs += choose2(count);
    }
    double right_pairs = 0.0;
    for (const auto &[key, count] : right_counts) {
        static_cast<void>(key);
        right_pairs += choose2(count);
    }
    const double all_pairs = choose2(static_cast<double>(left.size()));
    if (all_pairs <= 0.0)
        return 1.0;
    const double expected = left_pairs * right_pairs / all_pairs;
    const double maximum = 0.5 * (left_pairs + right_pairs);
    if (std::abs(maximum - expected) <= kEpsilon)
        return 1.0;
    return (paired - expected) / (maximum - expected);
}

SegmentationResult
train_segmentation(const std::vector<SegmentationRow> &local_rows,
                   const SegmentationParams &params,
                   const SegmentationCollectives &collectives,
                   const std::string &round_tag) {
    SegmentationResult result;
    const bool distributed = static_cast<bool>(collectives.allreduce) ||
                             static_cast<bool>(collectives.gather) ||
                             static_cast<bool>(collectives.broadcast);
    if (distributed && (!collectives.allreduce || !collectives.gather ||
                        !collectives.broadcast)) {
        result.status = compute::ComputeStatus::invalid_argument(
            "segmentation: all three collective callbacks are required");
        return result;
    }
    if (params.minimum_k < 2 || params.maximum_k < params.minimum_k ||
        params.restarts < 1 || params.max_iterations < 1 ||
        params.center_shift_tolerance < 0.0 ||
        params.minimum_segment_share < 0.0 ||
        params.minimum_segment_share >= 0.5 ||
        params.silhouette_sample_size < 2 ||
        params.maximum_category_levels < 2 ||
        !std::isfinite(params.center_shift_tolerance) ||
        !std::isfinite(params.minimum_segment_share) ||
        !std::isfinite(params.rare_category_share) ||
        params.rare_category_share < 0.0 || params.rare_category_share >= 1.0) {
        result.status = compute::ComputeStatus::invalid_argument(
            "segmentation: invalid k range, restart count, tolerance, minimum "
            "share, or sample size");
        return result;
    }

    if (!unique_entities(local_rows, collectives, round_tag + ".entities")) {
        result.status = compute::ComputeStatus::invalid_argument(
            "segmentation: entity keys must be unique globally (or collective "
            "failed)");
        return result;
    }
    PreparedData prepared = prepare_data(local_rows, params, collectives,
                                         round_tag + ".preprocess");
    if (!prepared.status.ok()) {
        result.status = prepared.status;
        return result;
    }
    const auto sample = make_global_sample(prepared, params, collectives,
                                           round_tag + ".sample");
    if (!sample ||
        sample->size() < static_cast<std::size_t>(params.minimum_k)) {
        result.status = compute::ComputeStatus::invalid_argument(
            "segmentation: the global sample has fewer rows than minimum_k");
        return result;
    }
    const int maximum_k =
        std::min(params.maximum_k, static_cast<int>(sample->size()));
    std::vector<CandidateForK> candidates;
    for (int k = params.minimum_k; k <= maximum_k; ++k) {
        auto candidate = fit_candidate(prepared, *sample, k, params,
                                       collectives, round_tag, result.status);
        if (!candidate)
            return result;
        result.metrics.tried.push_back(candidate->metric);
        candidates.push_back(std::move(*candidate));
    }

    const CandidateForK *chosen = choose_candidate(candidates);
    if (chosen == nullptr) {
        result.status = compute::ComputeStatus::invalid_argument(
            "segmentation: no candidate k satisfied the minimum segment share");
        return result;
    }

    result.metrics.chosen_k = chosen->metric.k;
    result.metrics.silhouette = chosen->metric.silhouette;
    result.metrics.davies_bouldin = chosen->metric.davies_bouldin;
    result.metrics.stability = chosen->stability;
    result.metrics.chosen_k_reason =
        "highest silhouette among k values satisfying the minimum segment "
        "share; "
        "k=2 may fall below that share; Davies-Bouldin and then smaller k "
        "break ties";
    result.model.features = prepared.features;
    result.model.centers = chosen->fit.centers;
    result.model.excluded_columns = prepared.excluded;
    result.model.seed = params.seed;

    const std::vector<double> overall_mean = global_encoded_mean(
        prepared, collectives, round_tag + ".profile.overall_mean");
    if (overall_mean.size() != prepared.features.size()) {
        result.status = compute::ComputeStatus::backend_error(
            "segmentation: profile statistics allreduce failed");
        return result;
    }
    result.profiles =
        build_profiles(result.model, chosen->fit.counts, overall_mean);
    for (const SegmentProfile &profile : result.profiles)
        result.model.labels.push_back(profile.label);
    for (std::size_t row = 0; row < prepared.points.size(); ++row) {
        double squared = 0.0;
        const int cluster = nearest_center(prepared.points[row],
                                           result.model.centers, &squared);
        result.assignments.push_back(
            {prepared.entities[row], cluster, std::sqrt(squared)});
    }
    return result;
}

} // namespace zetta::ml
