// SPDX-License-Identifier: MIT

#include "zetta/ml/tree_shap.hpp"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

// Exact path-dependent TreeSHAP (Lundberg, Erion, Lee 2018, Algorithm 2), matching
// the reference `shap` C extension (cext/tree_shap.h). The overview + the efficiency
// invariant live in the header; this file is the polynomial-time recursion. See
// docs/ml/tree-shap.md.
namespace zetta::ml {

namespace {

// One element of the "unique path" the recursion carries: a feature split on so far,
// the fraction of background paths that take the off-/on-path branch for it, and the
// running permutation weight that encodes the Shapley kernel across subset sizes.
struct PathElement {
    int feature_index = -1;
    double zero_fraction = 0.0;  // background fraction flowing the *off-path* way
    double one_fraction = 0.0;   // 1 if the instance takes this branch, else 0
    double pweight = 0.0;        // permutation weight (the polynomial bookkeeping)
};

// Grow the path by one split. The new element lands at index `path.size()`; the
// existing weights are redistributed with the (depth+1) denominator that keeps them
// equal to the Shapley subset weights. (shap's extend_path.)
void extend_path(std::vector<PathElement>& path, double zero_fraction,
                 double one_fraction, int feature_index) {
    const std::size_t unique_depth = path.size();  // insert index
    path.push_back(PathElement{.feature_index = feature_index,
                               .zero_fraction = zero_fraction,
                               .one_fraction = one_fraction,
                               .pweight = unique_depth == 0 ? 1.0 : 0.0});
    for (std::size_t i = unique_depth; i-- > 0;) {
        path[i + 1].pweight += one_fraction * path[i].pweight *
                               static_cast<double>(i + 1) /
                               static_cast<double>(unique_depth + 1);
        path[i].pweight = zero_fraction * path[i].pweight *
                          static_cast<double>(unique_depth - i) /
                          static_cast<double>(unique_depth + 1);
    }
}

// Undo a previous extend of position `path_index` (inverting the recurrence), then
// drop that element. Used when a feature is split on twice along one root-to-leaf
// path. (shap's unwind_path.)
void unwind_path(std::vector<PathElement>& path, std::size_t path_index) {
    const std::size_t unique_depth = path.size() - 1;  // last valid index
    const double one_fraction = path[path_index].one_fraction;
    const double zero_fraction = path[path_index].zero_fraction;
    double next_one_portion = path[unique_depth].pweight;
    for (std::size_t i = unique_depth; i-- > 0;) {
        if (one_fraction != 0.0) {
            const double tmp = path[i].pweight;
            path[i].pweight = next_one_portion *
                              static_cast<double>(unique_depth + 1) /
                              (static_cast<double>(i + 1) * one_fraction);
            next_one_portion = tmp - path[i].pweight * zero_fraction *
                                         static_cast<double>(unique_depth - i) /
                                         static_cast<double>(unique_depth + 1);
        } else if (zero_fraction != 0.0) {
            path[i].pweight = path[i].pweight *
                              static_cast<double>(unique_depth + 1) /
                              (zero_fraction * static_cast<double>(unique_depth - i));
        }
    }
    for (std::size_t i = path_index; i < unique_depth; ++i) {
        path[i].feature_index = path[i + 1].feature_index;
        path[i].zero_fraction = path[i + 1].zero_fraction;
        path[i].one_fraction = path[i + 1].one_fraction;
    }
    path.pop_back();
}

// The total permutation weight the path would carry if position `path_index` were
// unwound — computed without mutating `path` (the per-feature leaf contribution).
// (shap's unwound_path_sum.)
double unwound_path_sum(const std::vector<PathElement>& path,
                        std::size_t path_index) {
    const std::size_t unique_depth = path.size() - 1;
    const double one_fraction = path[path_index].one_fraction;
    const double zero_fraction = path[path_index].zero_fraction;
    double next_one_portion = path[unique_depth].pweight;
    double total = 0.0;
    // The restored weight at each position mirrors unwind_path exactly (so the two
    // never drift): `restored` carries the full (unique_depth+1) factor, and the
    // next_one_portion recurrence subtracts the full restored weight.
    for (std::size_t i = unique_depth; i-- > 0;) {
        if (one_fraction != 0.0) {
            const double restored = next_one_portion *
                                    static_cast<double>(unique_depth + 1) /
                                    (static_cast<double>(i + 1) * one_fraction);
            total += restored;
            next_one_portion = path[i].pweight -
                               restored * zero_fraction *
                                   static_cast<double>(unique_depth - i) /
                                   static_cast<double>(unique_depth + 1);
        } else if (zero_fraction != 0.0) {
            total += path[i].pweight * static_cast<double>(unique_depth + 1) /
                     (zero_fraction * static_cast<double>(unique_depth - i));
        }
    }
    return total;
}

// The background branch probability of going to `child` from `node`: the fraction of
// training cover (Σ Hessian) that flowed that way. A node with no recorded cover
// (degenerate / unreachable) splits its mass evenly so the recursion stays finite.
double branch_fraction(double child_cover, double node_cover) {
    if (node_cover > 0.0) return child_cover / node_cover;
    return 0.5;
}

// The recursive TreeSHAP traversal over one tree, accumulating into `phi` (indexed by
// the model's feature ordering). `path` is taken by value so each branch owns an
// independent copy (the extend/unwind mutate it). `x` is the instance; a short row is
// 0-padded.
void tree_shap_recurse(const std::vector<TreeNode>& nodes, int index,
                       std::vector<PathElement> path, double zero_fraction,
                       double one_fraction, int feature_index,
                       const std::vector<double>& x, std::vector<double>& phi) {
    extend_path(path, zero_fraction, one_fraction, feature_index);
    const TreeNode& node = nodes[static_cast<std::size_t>(index)];

    if (node.is_leaf()) {
        const std::size_t unique_depth = path.size() - 1;
        for (std::size_t i = 1; i <= unique_depth; ++i) {
            const PathElement& element = path[i];
            if (element.feature_index < 0) continue;  // the root sentinel
            // A validated model never splits past its feature list
            // (GbdtModel::validate_structure, #1390); never write past phi either.
            if (static_cast<std::size_t>(element.feature_index) >= phi.size()) continue;
            const double weight = unwound_path_sum(path, i);
            phi[static_cast<std::size_t>(element.feature_index)] +=
                weight * (element.one_fraction - element.zero_fraction) * node.value;
        }
        return;
    }

    const int split = node.feature;
    const double value = static_cast<std::size_t>(split) < x.size()
                             ? x[static_cast<std::size_t>(split)]
                             : 0.0;
    // The instance follows the same routing predict() uses (`<`, and missing_left for NaN), so the "hot" child is
    // the one it actually takes (its on-path branch carries one_fraction = 1).
    const int hot = node.child_for(value);
    const int cold = hot == node.left ? node.right : node.left;

    double incoming_zero = 1.0;
    double incoming_one = 1.0;
    // If this feature already split higher up, undo that so we can fold it back in at
    // this depth (the canonical unwind-then-re-extend for repeated features).
    std::size_t path_index = 0;
    for (; path_index < path.size(); ++path_index) {
        if (path[path_index].feature_index == split) break;
    }
    if (path_index < path.size()) {
        incoming_zero = path[path_index].zero_fraction;
        incoming_one = path[path_index].one_fraction;
        unwind_path(path, path_index);
    }

    const double node_cover = node.cover;
    const double hot_zero =
        branch_fraction(nodes[static_cast<std::size_t>(hot)].cover, node_cover);
    const double cold_zero =
        branch_fraction(nodes[static_cast<std::size_t>(cold)].cover, node_cover);

    tree_shap_recurse(nodes, hot, path, hot_zero * incoming_zero, incoming_one,
                      split, x, phi);
    tree_shap_recurse(nodes, cold, path, cold_zero * incoming_zero, 0.0, split, x,
                      phi);
}

// The cover-weighted expected raw output of one tree: telescoping the branch
// fractions down to the leaves yields Σ_leaves (cover[leaf]/cover[root]) · value.
double tree_expected_value(const std::vector<TreeNode>& nodes, int index) {
    const TreeNode& node = nodes[static_cast<std::size_t>(index)];
    if (node.is_leaf()) return node.value;
    const double node_cover = node.cover;
    const double left_fraction =
        branch_fraction(nodes[static_cast<std::size_t>(node.left)].cover, node_cover);
    const double right_fraction =
        branch_fraction(nodes[static_cast<std::size_t>(node.right)].cover, node_cover);
    return left_fraction * tree_expected_value(nodes, node.left) +
           right_fraction * tree_expected_value(nodes, node.right);
}

}  // namespace

double ShapExplanation::efficiency_residual() const {
    double sum = base_value;
    for (const double value : values) sum += value;
    return predicted - sum;
}

double model_base_value(const GbdtModel& model) {
    double base = model.base_score;
    for (const RegressionTree& tree : model.trees) {
        if (!tree.nodes.empty()) base += tree_expected_value(tree.nodes, 0);
    }
    return base;
}

ShapExplanation tree_shap(const GbdtModel& model,
                          const std::vector<double>& features) {
    ShapExplanation explanation;
    explanation.values.assign(model.feature_count(), 0.0);
    explanation.predicted = model.predict_raw(features);
    explanation.base_value = model_base_value(model);

    if (model.feature_count() == 0) return explanation;
    for (const RegressionTree& tree : model.trees) {
        if (tree.nodes.empty()) continue;
        // Seed the root with the empty path (sentinel feature -1, full background).
        tree_shap_recurse(tree.nodes, 0, std::vector<PathElement>{}, 1.0, 1.0, -1,
                          features, explanation.values);
    }
    return explanation;
}

ShapDriver dominant_driver(const GbdtModel& model,
                           const ShapExplanation& explanation) {
    ShapDriver driver;
    double best_magnitude = -1.0;
    const std::size_t count =
        std::min(explanation.values.size(), model.feature_names.size());
    for (std::size_t index = 0; index < count; ++index) {
        const double magnitude = std::abs(explanation.values[index]);
        if (magnitude > best_magnitude) {  // first (lowest index) wins ties
            best_magnitude = magnitude;
            driver.feature_index = static_cast<int>(index);
            driver.feature = model.feature_names[index];
            driver.value = explanation.values[index];
        }
    }
    return driver;
}

}  // namespace zetta::ml
