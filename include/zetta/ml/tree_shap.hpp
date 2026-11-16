// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zetta/ml/gbdt.hpp"

// Exact **TreeSHAP** explanation of a trained histogram GBDT (FEAT-6, #393; part of
// the native distributed accelerated-compute pass for UC-1 churn, #264). This is the
// explainability the rented-model approach can't give: for a single entity, attribute
// the model's prediction to its features — `φ_i` per feature — so a per-entity
// **dominant churn driver** falls out.
//
// **Exact, path-dependent (Lundberg, Erion, Lee 2018, Algorithm 2).** SHAP values are
// the unique additive feature attributions satisfying the Shapley axioms; for tree
// ensembles they can be computed *exactly* in polynomial time
// (`O(trees · leaves · depth²)`) by a single recursive traversal per tree that tracks,
// for every subset of features, the proportion of permutations consistent with it. The
// "missing feature" expectation is the **path-dependent** one: a feature not in the
// explained subset is integrated out by following *both* children weighted by the
// training **cover** that flowed each way — `cover[child] / cover[node]`, the
// background branch probability the trainer recorded on every `TreeNode`
// (see gbdt.hpp). No background dataset is needed: the model is self-describing.
//
// **Margin (raw / logit) space.** SHAP is additive, so the explanation lives in the
// model's additive output — the **raw score** `predict_raw` (a logit for the binary
// logistic objective), not the squashed probability. The ensemble's SHAP values are
// the sum of the per-tree ones (the model is a sum of trees), so the whole ensemble is
// explained in one pass. The headline invariant is **efficiency / local accuracy**:
//
//   predict_raw(x)  ==  base_value + Σ_i φ_i(x)              (within FP tolerance)
//
// where `base_value` is the model's intrinsic expected raw score under the cover
// distribution (`base_score` plus each tree's cover-weighted mean leaf). A positive
// `φ_i` pushes the prediction up (toward churn for the churn model), a negative one
// down. This identity is what `tests/test_tree_shap.cpp` asserts per entity.
//
// **Pure + deterministic + non-throwing.** `tree_shap` is a pure function of
// `(model, features)` with no device, no I/O, no shared state — so it is trivially
// thread-safe (the distributed `explain` job fans it across a node's entities with
// `parallel_map`; see gbdt_explain_job.hpp) and reproducible. It introduces no new JSON
// boundary: it reads the already-schema-validated `GbdtModel` and returns plain data.
//
// See docs/ml/tree-shap.md.
namespace zetta::ml {

// One entity's exact SHAP explanation, in the model's raw-score (margin / logit) space.
// Invariant (efficiency): `base_value + Σ values ≈ predicted`.
struct ShapExplanation {
    double base_value = 0.0;            // the model's expected raw score (cover-weighted)
    double predicted = 0.0;            // predict_raw(features) for this entity
    std::vector<double> values;        // φ per feature, in the model's feature ordering

    // The residual `predicted - (base_value + Σ values)` — zero up to floating point
    // when the explanation is exact. Exposed so callers/tests can assert efficiency.
    double efficiency_residual() const;
};

// Exact path-dependent TreeSHAP for one feature row against the trained ensemble.
// `features[f]` is the value of the model's feature `f` (model.feature_names order);
// a short row is treated as 0-padded, an over-long row ignores the extra columns. The
// returned `values` has `model.feature_count()` entries. Non-throwing.
ShapExplanation tree_shap(const GbdtModel& model,
                          const std::vector<double>& features);

// The model's **base value**: its expected raw score under the cover distribution
// (`base_score` + Σ over trees of the cover-weighted mean leaf). This is the constant
// the SHAP values of every entity sum *away from*. Computed directly (no entity needed),
// it equals `tree_shap(model, x).base_value` for any x.
double model_base_value(const GbdtModel& model);

// The single dominant driver of an explanation: the feature with the largest influence
// magnitude `|φ|`. `sign(value)` says the direction (positive = pushed the prediction
// up / toward churn). `feature_index` is the model feature index, `feature` its name
// (empty when the model has no features or the explanation is degenerate).
struct ShapDriver {
    int feature_index = -1;   // index into model.feature_names; -1 when none
    std::string feature;      // the dominant feature's name
    double value = 0.0;       // its signed SHAP attribution φ (raw-score space)
};

// The dominant driver of `explanation` w.r.t. `model` (largest |φ|; ties broken by the
// lower feature index, so the result is deterministic). Returns a default ShapDriver
// (feature_index -1) when the model has no features.
ShapDriver dominant_driver(const GbdtModel& model,
                           const ShapExplanation& explanation);

}  // namespace zetta::ml
