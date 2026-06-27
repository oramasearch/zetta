#include "zetta/ml/gbdt.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/runtime.hpp"

// The histogram GBDT trainer + the binary-logistic model. The algorithm overview
// and the distributed-by-allreduce design live in the header; this file is the
// numerical reference. See docs/gbdt.md.
namespace zetta::ml {

namespace {

using compute::ComputeStatus;

// --- scalar helpers ----------------------------------------------------------

// Numerically stable logistic sigmoid.
double sigmoid(double x) {
    if (x >= 0.0) {
        const double z = std::exp(-x);
        return 1.0 / (1.0 + z);
    }
    const double z = std::exp(x);
    return z / (1.0 + z);
}

// log(p / (1 - p)), p already clamped off 0/1 by the caller.
double logit(double p) { return std::log(p / (1.0 - p)); }

constexpr double kProbEpsilon = 1e-15;  // keeps log-loss / logit finite

// --- GbdtParams JSON boundary ------------------------------------------------

// Read a numeric field into `out` if present; false on a wrong type / range.
bool read_positive_int(const nlohmann::json& object, const char* key, int& out,
                       int minimum) {
    if (!object.contains(key)) return true;  // keep the default
    const nlohmann::json& field = object.at(key);
    if (!field.is_number_integer()) return false;
    const long long value = field.get<long long>();
    if (value < minimum) return false;
    out = static_cast<int>(value);
    return true;
}

bool read_number(const nlohmann::json& object, const char* key, double& out,
                 double minimum, bool exclusive) {
    if (!object.contains(key)) return true;
    const nlohmann::json& field = object.at(key);
    if (!field.is_number()) return false;
    const double value = field.get<double>();
    if (exclusive ? !(value > minimum) : !(value >= minimum)) return false;
    out = value;
    return true;
}

// --- GbdtModel JSON boundary (pboettch draft-07, like the Run record) ---------

const nlohmann::json& model_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ml.GbdtModel",
      "type": "object",
      "properties": {
        "objective": { "type": "string", "enum": ["binary_logistic"] },
        "base_score": { "type": "number" },
        "learning_rate": { "type": "number" },
        "feature_names": { "type": "array", "items": { "type": "string" } },
        "trees": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "nodes": {
                "type": "array",
                "minItems": 1,
                "items": {
                  "type": "object",
                  "properties": {
                    "feature":   { "type": "integer" },
                    "threshold": { "type": "number" },
                    "left":      { "type": "integer" },
                    "right":     { "type": "integer" },
                    "value":     { "type": "number" },
                    "cover":     { "type": "number" }
                  },
                  "required": ["feature", "threshold", "left", "right", "value",
                               "cover"],
                  "additionalProperties": false
                }
              }
            },
            "required": ["nodes"],
            "additionalProperties": false
          }
        },
        "calibration": { "type": "object" }
      },
      "required": ["objective", "base_score", "learning_rate", "feature_names",
                   "trees"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& model_validator() {
    static nlohmann::json_schema::json_validator validator(model_schema_document());
    return validator;
}

// Collects the first validation failure without letting the validator throw
// across the library boundary (the run.cpp pattern).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// --- binning + split-finding (the histogram primitives) ----------------------

// Per-feature global bin layout: lower bound + bin width. width == 0 marks a
// globally-constant feature (every value falls in bin 0; it can never split).
struct FeatureBins {
    std::vector<double> minimum;  // global min per feature
    std::vector<double> width;    // (max - min) / bin_count, or 0 if constant
    std::size_t bin_count = 0;

    // The bin index of `value` for feature `f`, clamped into [0, bin_count).
    std::size_t bin_of(std::size_t feature, double value) const {
        if (width[feature] <= 0.0) return 0;
        const double scaled = (value - minimum[feature]) / width[feature];
        if (scaled <= 0.0) return 0;
        const auto bin = static_cast<long long>(scaled);
        if (bin >= static_cast<long long>(bin_count)) return bin_count - 1;
        return static_cast<std::size_t>(bin);
    }

    // The numeric split threshold for "left = bins [0, bin]" — the upper edge of
    // bin `bin`, so x < threshold reproduces the bin partition exactly.
    double threshold_after(std::size_t feature, std::size_t bin) const {
        return minimum[feature] +
               static_cast<double>(bin + 1) * width[feature];
    }
};

// A chosen split for one tree node.
struct Split {
    bool valid = false;
    std::size_t feature = 0;
    std::size_t bin = 0;   // left child = bins [0, bin], right = (bin, last]
    double gain = 0.0;
};

// A tree node still being grown: its index in the tree, its depth, and the LOCAL
// row indices that fall into it on this node of the cluster.
struct OpenNode {
    int tree_index = 0;
    int depth = 0;
    std::vector<std::size_t> rows;
};

// Best split for one node from its (global) per-feature gradient/Hessian histogram.
// `hist` is laid out [feature][bin][g, h]; `total_g`/`total_h` are the node totals.
Split best_split(const std::vector<double>& hist, std::size_t feature_count,
                 std::size_t bin_count, double total_g, double total_h,
                 const GbdtParams& params) {
    Split best;
    const double lambda = params.l2_regularization;
    const double parent = total_g * total_g / (total_h + lambda);
    for (std::size_t feature = 0; feature < feature_count; ++feature) {
        double left_g = 0.0;
        double left_h = 0.0;
        // Split after bin b: left = [0, b], right = (b, last]. Stop at the
        // second-to-last bin so the right side is non-empty.
        for (std::size_t bin = 0; bin + 1 < bin_count; ++bin) {
            const std::size_t base = (feature * bin_count + bin) * 2;
            left_g += hist[base];
            left_h += hist[base + 1];
            const double right_g = total_g - left_g;
            const double right_h = total_h - left_h;
            if (left_h < params.min_child_hessian ||
                right_h < params.min_child_hessian) {
                continue;
            }
            const double gain =
                0.5 * (left_g * left_g / (left_h + lambda) +
                       right_g * right_g / (right_h + lambda) - parent) -
                params.min_split_gain;
            if (gain > best.gain) {
                best.valid = true;
                best.feature = feature;
                best.bin = bin;
                best.gain = gain;
            }
        }
    }
    return best;
}

// Walk a partially-grown tree to the leaf a local row lands in, via the numeric
// thresholds (so the running prediction matches the saved model exactly).
double predict_local_row(const std::vector<TreeNode>& nodes,
                         const std::vector<std::vector<double>>& columns,
                         std::size_t row) {
    if (nodes.empty()) return 0.0;
    int cursor = 0;
    while (!nodes[cursor].is_leaf()) {
        const TreeNode& node = nodes[cursor];
        cursor = columns[static_cast<std::size_t>(node.feature)][row] < node.threshold
                     ? node.left
                     : node.right;
    }
    return nodes[cursor].value;
}

}  // namespace

// --- GbdtParams --------------------------------------------------------------

nlohmann::json GbdtParams::to_json() const {
    return nlohmann::json{{"num_trees", num_trees},
                          {"max_depth", max_depth},
                          {"max_bins", max_bins},
                          {"learning_rate", learning_rate},
                          {"l2_regularization", l2_regularization},
                          {"min_split_gain", min_split_gain},
                          {"min_child_hessian", min_child_hessian}};
}

std::optional<GbdtParams> GbdtParams::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    static const char* known[] = {"num_trees",         "max_depth",
                                  "max_bins",          "learning_rate",
                                  "l2_regularization", "min_split_gain",
                                  "min_child_hessian"};
    for (const auto& [key, _] : value.items()) {
        bool recognized = false;
        for (const char* name : known) {
            if (key == name) {
                recognized = true;
                break;
            }
        }
        if (!recognized) return std::nullopt;
    }
    GbdtParams params;
    if (!read_positive_int(value, "num_trees", params.num_trees, 1))
        return std::nullopt;
    if (!read_positive_int(value, "max_depth", params.max_depth, 1))
        return std::nullopt;
    if (!read_positive_int(value, "max_bins", params.max_bins, 2))
        return std::nullopt;
    if (!read_number(value, "learning_rate", params.learning_rate, 0.0, true))
        return std::nullopt;
    if (!read_number(value, "l2_regularization", params.l2_regularization, 0.0,
                     false))
        return std::nullopt;
    if (!read_number(value, "min_split_gain", params.min_split_gain, 0.0, false))
        return std::nullopt;
    if (!read_number(value, "min_child_hessian", params.min_child_hessian, 0.0,
                     false))
        return std::nullopt;
    return params;
}

// --- RegressionTree / GbdtModel ----------------------------------------------

double RegressionTree::predict(const std::vector<double>& features) const {
    if (nodes.empty()) return 0.0;
    int cursor = 0;
    while (!nodes[cursor].is_leaf()) {
        const TreeNode& node = nodes[cursor];
        cursor = features[static_cast<std::size_t>(node.feature)] < node.threshold
                     ? node.left
                     : node.right;
    }
    return nodes[cursor].value;
}

double GbdtModel::predict_raw(const std::vector<double>& features) const {
    double raw = base_score;
    for (const RegressionTree& tree : trees) raw += tree.predict(features);
    return raw;
}

double GbdtModel::predict_proba(const std::vector<double>& features) const {
    const double raw = predict_raw(features);
    if (calibration) return calibration->calibrate(raw);
    return sigmoid(raw);
}

nlohmann::json GbdtModel::to_json() const {
    nlohmann::json trees_json = nlohmann::json::array();
    for (const RegressionTree& tree : trees) {
        nlohmann::json nodes_json = nlohmann::json::array();
        for (const TreeNode& node : tree.nodes) {
            nodes_json.push_back({{"feature", node.feature},
                                  {"threshold", node.threshold},
                                  {"left", node.left},
                                  {"right", node.right},
                                  {"value", node.value},
                                  {"cover", node.cover}});
        }
        trees_json.push_back({{"nodes", std::move(nodes_json)}});
    }
    nlohmann::json value{{"objective", objective},
                         {"base_score", base_score},
                         {"learning_rate", learning_rate},
                         {"feature_names", feature_names},
                         {"trees", std::move(trees_json)}};
    if (calibration) value["calibration"] = calibration->to_json();
    return value;
}

std::optional<GbdtModel> GbdtModel::from_json(const nlohmann::json& value,
                                             std::string* error) {
    collecting_error_handler handler;
    model_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return std::nullopt;
    }
    GbdtModel model;
    model.objective = value.at("objective").get<std::string>();
    model.base_score = value.at("base_score").get<double>();
    model.learning_rate = value.at("learning_rate").get<double>();
    model.feature_names =
        value.at("feature_names").get<std::vector<std::string>>();
    for (const nlohmann::json& tree_json : value.at("trees")) {
        RegressionTree tree;
        const nlohmann::json& nodes_json = tree_json.at("nodes");
        const auto node_count = static_cast<int>(nodes_json.size());
        for (const nlohmann::json& node_json : nodes_json) {
            TreeNode node;
            node.feature = node_json.at("feature").get<int>();
            node.threshold = node_json.at("threshold").get<double>();
            node.left = node_json.at("left").get<int>();
            node.right = node_json.at("right").get<int>();
            node.value = node_json.at("value").get<double>();
            node.cover = node_json.at("cover").get<double>();
            // Structural rule the schema can't express: an internal node's
            // children must be valid, distinct, forward indices in this tree.
            if (!node.is_leaf()) {
                if (node.left < 0 || node.right < 0 || node.left >= node_count ||
                    node.right >= node_count || node.left == node.right) {
                    if (error) *error = "trees: child index out of range";
                    return std::nullopt;
                }
            }
            tree.nodes.push_back(node);
        }
        model.trees.push_back(std::move(tree));
    }
    // An attached calibration is validated by its own boundary (the loose object
    // the model schema allows is pinned down strictly here, like the tree child
    // indices above).
    if (value.contains("calibration")) {
        std::optional<Calibrator> calibrator =
            Calibrator::from_json(value.at("calibration"), error);
        if (!calibrator) return std::nullopt;
        model.calibration = std::move(*calibrator);
    }
    return model;
}

const nlohmann::json& gbdt_model_schema() { return model_schema_document(); }

// --- Metrics -----------------------------------------------------------------

nlohmann::json GbdtMetrics::to_json() const {
    return nlohmann::json{
        {"auc", auc}, {"log_loss", log_loss}, {"accuracy", accuracy}};
}

GbdtMetrics evaluate_binary(const std::vector<double>& probabilities,
                            const std::vector<double>& labels) {
    GbdtMetrics metrics;
    const std::size_t count = std::min(probabilities.size(), labels.size());
    if (count == 0) return metrics;

    // Log-loss + accuracy.
    double loss = 0.0;
    std::size_t correct = 0;
    std::size_t positives = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const double p =
            std::clamp(probabilities[i], kProbEpsilon, 1.0 - kProbEpsilon);
        const bool positive = labels[i] >= 0.5;
        if (positive) ++positives;
        loss += positive ? -std::log(p) : -std::log(1.0 - p);
        const bool predicted = probabilities[i] >= 0.5;
        if (predicted == positive) ++correct;
    }
    metrics.log_loss = loss / static_cast<double>(count);
    metrics.accuracy = static_cast<double>(correct) / static_cast<double>(count);

    const std::size_t negatives = count - positives;
    if (positives == 0 || negatives == 0) {
        metrics.auc = 0.5;  // ROC undefined with a single class present
        return metrics;
    }

    // AUC via the Mann-Whitney statistic: average-rank the probabilities, sum the
    // ranks of the positives. Ties share the mean rank (so identical scores don't
    // bias the ordering).
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return probabilities[a] < probabilities[b];
    });
    std::vector<double> rank(count, 0.0);
    std::size_t i = 0;
    while (i < count) {
        std::size_t j = i + 1;
        while (j < count &&
               probabilities[order[j]] == probabilities[order[i]]) {
            ++j;
        }
        // Ranks are 1-based; the tied block [i, j) shares the average rank.
        const double average = (static_cast<double>(i + 1) +
                                static_cast<double>(j)) /
                               2.0;
        for (std::size_t k = i; k < j; ++k) rank[order[k]] = average;
        i = j;
    }
    double rank_sum = 0.0;
    for (std::size_t r = 0; r < count; ++r) {
        if (labels[r] >= 0.5) rank_sum += rank[r];
    }
    const double pos = static_cast<double>(positives);
    const double neg = static_cast<double>(negatives);
    metrics.auc = (rank_sum - pos * (pos + 1.0) / 2.0) / (pos * neg);
    return metrics;
}

// --- Training ----------------------------------------------------------------

GbdtTrainResult train_gbdt(const std::vector<std::vector<double>>& columns,
                           const std::vector<std::string>& feature_names,
                           const std::vector<double>& labels,
                           const GbdtParams& params, const AllReduce& allreduce,
                           const std::string& tag) {
    GbdtTrainResult result;
    const std::size_t feature_count = feature_names.size();
    const std::size_t local_rows = labels.size();

    // Shape checks at the boundary.
    if (feature_count == 0) {
        result.status = ComputeStatus::invalid_argument("train_gbdt: no features");
        return result;
    }
    if (columns.size() != feature_count) {
        result.status = ComputeStatus::invalid_argument(
            "train_gbdt: column count does not match feature_names");
        return result;
    }
    for (const std::vector<double>& column : columns) {
        if (column.size() != local_rows) {
            result.status = ComputeStatus::invalid_argument(
                "train_gbdt: ragged columns / labels length mismatch");
            return result;
        }
    }
    const std::size_t bin_count =
        static_cast<std::size_t>(std::max(2, params.max_bins));

    // Helper that runs an allreduce and verifies the global vector kept its shape
    // (an empty / wrong-length reply means a collective failure).
    bool collective_failed = false;
    const auto reduce = [&](const std::string& sub,
                            const std::vector<double>& local,
                            GbdtReduce op) -> std::vector<double> {
        std::vector<double> global = allreduce(tag + "." + sub, local, op);
        if (global.size() != local.size()) {
            collective_failed = true;
            return local;  // ignored; the caller checks collective_failed
        }
        return global;
    };

    // 1. Global bin edges: local min/max per feature -> allreduce Min / Max. A
    // node with no rows contributes the identities (+inf / -inf), so it does not
    // move the global range.
    std::vector<double> local_min(feature_count,
                                  std::numeric_limits<double>::infinity());
    std::vector<double> local_max(feature_count,
                                  -std::numeric_limits<double>::infinity());
    for (std::size_t feature = 0; feature < feature_count; ++feature) {
        for (std::size_t row = 0; row < local_rows; ++row) {
            const double value = columns[feature][row];
            local_min[feature] = std::min(local_min[feature], value);
            local_max[feature] = std::max(local_max[feature], value);
        }
    }
    const std::vector<double> global_min =
        reduce("edges.min", local_min, GbdtReduce::Min);
    const std::vector<double> global_max =
        reduce("edges.max", local_max, GbdtReduce::Max);

    // 2. Global base rate: local [Σy, n] -> allreduce Sum.
    double local_pos = 0.0;
    for (const double label : labels) local_pos += label >= 0.5 ? 1.0 : 0.0;
    const std::vector<double> base =
        reduce("base", {local_pos, static_cast<double>(local_rows)},
               GbdtReduce::Sum);
    if (collective_failed) {
        result.status = ComputeStatus::backend_error(
            "train_gbdt: a collective round failed (peer unreachable or timeout)");
        return result;
    }
    const double global_pos = base[0];
    const double global_count = base[1];
    if (global_count <= 0.0) {
        result.status =
            ComputeStatus::invalid_argument("train_gbdt: no training rows");
        return result;
    }
    if (global_pos <= 0.0 || global_pos >= global_count) {
        result.status = ComputeStatus::invalid_argument(
            "train_gbdt: training labels are a single class");
        return result;
    }
    const double mean = std::clamp(global_pos / global_count, kProbEpsilon,
                                   1.0 - kProbEpsilon);

    // Establish the bin layout, then pre-bin every local row per feature.
    FeatureBins binner;
    binner.bin_count = bin_count;
    binner.minimum = global_min;
    binner.width.resize(feature_count);
    for (std::size_t feature = 0; feature < feature_count; ++feature) {
        const double span = global_max[feature] - global_min[feature];
        binner.width[feature] =
            span > 0.0 ? span / static_cast<double>(bin_count) : 0.0;
    }
    std::vector<std::vector<std::uint16_t>> bins(
        feature_count, std::vector<std::uint16_t>(local_rows));
    for (std::size_t feature = 0; feature < feature_count; ++feature) {
        for (std::size_t row = 0; row < local_rows; ++row) {
            bins[feature][row] = static_cast<std::uint16_t>(
                binner.bin_of(feature, columns[feature][row]));
        }
    }

    // Initialize the model + per-row running raw scores.
    GbdtModel& model = result.model;
    model.objective = "binary_logistic";
    model.base_score = logit(mean);
    model.learning_rate = params.learning_rate;
    model.feature_names = feature_names;
    result.feature_importances.assign(feature_count, 0.0);

    std::vector<double> raw(local_rows, model.base_score);
    std::vector<double> gradient(local_rows);
    std::vector<double> hessian(local_rows);

    const std::size_t node_stride = feature_count * bin_count * 2;

    for (int tree_index = 0; tree_index < params.num_trees; ++tree_index) {
        // Gradients + Hessians at the current predictions.
        for (std::size_t row = 0; row < local_rows; ++row) {
            const double p = sigmoid(raw[row]);
            gradient[row] = p - labels[row];
            hessian[row] = p * (1.0 - p);
        }

        // Grow one tree, level by level. The root is a leaf placeholder until it
        // either splits or is finalized below.
        RegressionTree tree;
        tree.nodes.push_back(TreeNode{});  // root, index 0
        std::vector<OpenNode> open;
        std::vector<std::size_t> all_rows(local_rows);
        std::iota(all_rows.begin(), all_rows.end(), std::size_t{0});
        open.push_back(OpenNode{.tree_index = 0, .depth = 0,
                                .rows = std::move(all_rows)});

        int level = 0;
        while (!open.empty()) {
            const std::size_t node_count = open.size();
            // Local histograms for every open node, flattened [node][feature][bin][g,h].
            std::vector<double> local_hist(node_count * node_stride, 0.0);
            for (std::size_t m = 0; m < node_count; ++m) {
                const OpenNode& node = open[m];
                const std::size_t node_base = m * node_stride;
                // Parallelize over features — each feature writes a disjoint
                // region, so accumulation is race-free (the CpuDevice pattern).
                runtime::parallel_for(
                    feature_count,
                    [&](std::size_t, std::size_t begin, std::size_t end) {
                        for (std::size_t feature = begin; feature < end;
                             ++feature) {
                            const std::size_t feature_base =
                                node_base + feature * bin_count * 2;
                            for (const std::size_t row : node.rows) {
                                const std::size_t bin = bins[feature][row];
                                local_hist[feature_base + bin * 2] +=
                                    gradient[row];
                                local_hist[feature_base + bin * 2 + 1] +=
                                    hessian[row];
                            }
                        }
                    });
            }

            // Sum the per-node histograms across the cluster: this is the round
            // that makes split-finding see global statistics.
            const std::vector<double> global_hist = reduce(
                "t" + std::to_string(tree_index) + ".l" + std::to_string(level),
                local_hist, GbdtReduce::Sum);
            if (collective_failed) {
                result.status = ComputeStatus::backend_error(
                    "train_gbdt: a histogram allreduce failed (peer unreachable "
                    "or timeout)");
                return result;
            }

            std::vector<OpenNode> next_open;
            for (std::size_t m = 0; m < node_count; ++m) {
                OpenNode& node = open[m];
                const std::size_t node_base = m * node_stride;
                const std::vector<double> node_hist(
                    global_hist.begin() + node_base,
                    global_hist.begin() + node_base + node_stride);
                // Node totals from feature 0's bins (every feature sums the same).
                double total_g = 0.0;
                double total_h = 0.0;
                for (std::size_t bin = 0; bin < bin_count; ++bin) {
                    total_g += node_hist[bin * 2];
                    total_h += node_hist[bin * 2 + 1];
                }
                // The node's training weight (XGBoost's "cover"): the global Σ of
                // Hessians reaching it. Recorded on every node — internal or leaf —
                // so the serialized tree carries the background branch probabilities
                // exact path-dependent TreeSHAP integrates over (see tree_shap.hpp).
                tree.nodes[node.tree_index].cover = total_h;

                Split split;
                if (node.depth < params.max_depth) {
                    split = best_split(node_hist, feature_count, bin_count,
                                       total_g, total_h, params);
                }
                if (split.valid) {
                    const int left_index = static_cast<int>(tree.nodes.size());
                    const int right_index = left_index + 1;
                    tree.nodes.push_back(TreeNode{});  // left leaf placeholder
                    tree.nodes.push_back(TreeNode{});  // right leaf placeholder
                    TreeNode& internal = tree.nodes[node.tree_index];
                    internal.feature = static_cast<int>(split.feature);
                    internal.threshold =
                        binner.threshold_after(split.feature, split.bin);
                    internal.left = left_index;
                    internal.right = right_index;
                    result.feature_importances[split.feature] += split.gain;

                    OpenNode left{.tree_index = left_index,
                                  .depth = node.depth + 1, .rows = {}};
                    OpenNode right{.tree_index = right_index,
                                   .depth = node.depth + 1, .rows = {}};
                    for (const std::size_t row : node.rows) {
                        if (bins[split.feature][row] <= split.bin) {
                            left.rows.push_back(row);
                        } else {
                            right.rows.push_back(row);
                        }
                    }
                    next_open.push_back(std::move(left));
                    next_open.push_back(std::move(right));
                } else {
                    // Leaf: the Newton step, shrunk by the learning rate.
                    TreeNode& leaf = tree.nodes[node.tree_index];
                    leaf.feature = -1;
                    leaf.value = -total_g / (total_h + params.l2_regularization) *
                                 params.learning_rate;
                }
            }
            open = std::move(next_open);
            ++level;
        }

        // Update the running raw scores with the new tree (numeric path), then
        // commit it.
        for (std::size_t row = 0; row < local_rows; ++row) {
            raw[row] += predict_local_row(tree.nodes, columns, row);
        }
        model.trees.push_back(std::move(tree));
    }

    result.status = ComputeStatus::success();
    return result;
}

GbdtTrainResult train_gbdt(const std::vector<std::vector<double>>& columns,
                           const std::vector<std::string>& feature_names,
                           const std::vector<double>& labels,
                           const GbdtParams& params) {
    // Identity reduction: this node's shard is the whole dataset.
    const AllReduce identity = [](const std::string&,
                                  const std::vector<double>& local,
                                  GbdtReduce) { return local; };
    return train_gbdt(columns, feature_names, labels, params, identity);
}

// --- Calibration -------------------------------------------------------------

ComputeStatus calibrate_model(
    GbdtModel& model, const std::vector<std::vector<double>>& holdout_columns,
    const std::vector<double>& holdout_labels, CalibrationMethod method) {
    const std::size_t feature_count = model.feature_names.size();
    if (holdout_columns.size() != feature_count) {
        return ComputeStatus::invalid_argument(
            "calibrate_model: column count does not match the model features");
    }
    const std::size_t rows = holdout_labels.size();
    for (const std::vector<double>& column : holdout_columns) {
        if (column.size() != rows) {
            return ComputeStatus::invalid_argument(
                "calibrate_model: ragged columns / labels length mismatch");
        }
    }

    // Score every held-out row through the (uncalibrated) trees.
    std::vector<double> raw_scores(rows);
    std::vector<double> row(feature_count);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t f = 0; f < feature_count; ++f) row[f] = holdout_columns[f][r];
        raw_scores[r] = model.predict_raw(row);
    }

    const CalibrationResult fit = fit_calibrator(raw_scores, holdout_labels, method);
    if (!fit.ok()) return fit.status;
    model.calibration = fit.calibrator;
    return ComputeStatus::success();
}

}  // namespace zetta::ml
