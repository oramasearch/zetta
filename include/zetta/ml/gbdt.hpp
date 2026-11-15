// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"
#include "zetta/ml/calibration.hpp"

// Histogram gradient-boosted decision trees with a **binary logistic** objective
// (FEAT-6 / ENT-14, #390; part of the native distributed accelerated-compute pass
// for UC-1 churn, #264). This is the differentiator: train a model on the
// customer's own data, rather than running inference over a rented model.
//
// **Histogram GBDT, the short version.** A model is a base score plus an ordered
// ensemble of small regression trees. Each boosting round fits one tree to the
// gradient/Hessian of the binary log-loss at the current predictions:
//
//   p_i = sigmoid(raw_i),  g_i = p_i - y_i,  h_i = p_i (1 - p_i)
//
// A tree is grown by, at every (tree-node, feature), binning the feature's values
// into a small number of bins and accumulating the per-bin sum of gradients +
// Hessians — the **gradient histogram**. The best split for a node is the bin
// boundary that maximizes the standard regularized split gain
//
//   gain = 1/2 [ G_L^2/(H_L+λ) + G_R^2/(H_R+λ) - G^2/(H+λ) ] - γ
//
// and a leaf's output is the Newton step  w = -G/(H+λ)  scaled by the learning
// rate. Binning makes split-finding O(bins) per feature instead of O(rows), which
// is what lets it scale.
//
// **Distributed by histogram allreduce.** The rows are sharded across the cluster.
// Every node bins against the *same* globally-agreed bin edges (a min/max
// allreduce), seeds the base score from the global label rate, and at each tree
// level builds the histograms over only its local rows — then those per-node
// histograms are **summed across the cluster** (an allreduce). Because split
// finding then runs off the *global* histograms (identical on every node) the
// SPMD bodies grow **bit-equivalent trees** with no further coordination, so a
// single-node and a multi-node run produce the same model. This file is the
// algorithm; the cross-node combine is injected as an `AllReduce` callable, so the
// same boosting loop drives both the single-node path (identity reduction) and the
// distributed `train` ComputeJob (gbdt_train_job.hpp).
//
// **Host-side reference, device-backed data.** Like the as-of / group-by ops, the
// trainer reads its feature columns back through `ComputeDevice::download`
// (so it runs the same over a CPU or CUDA frame — see gbdt_train_job.hpp) and
// accumulates the gradient/Hessian histograms host-side via `runtime::parallel_for`
// — the numerical reference. A device-native weighted-histogram kernel is a later
// optimization behind this same API. Non-throwing throughout (zetta-core's
// no-exceptions convention): the entry points return a `compute::ComputeStatus`.
//
// See docs/ml/gbdt.md.
namespace zetta::ml {

// --- Hyperparameters ---------------------------------------------------------

// The knobs that govern training. Defaults are a sensible shallow-ensemble
// starting point. Carries a validated JSON boundary (schemas/gbdt_params.schema.json,
// the hand-written non-throwing pattern of DataFrameSchema / FeaturizeSpec).
struct GbdtParams {
    int num_trees = 100;          // boosting rounds (trees in the ensemble)
    int max_depth = 4;            // maximum tree depth (root is depth 0)
    int max_bins = 32;            // histogram bins per feature
    double learning_rate = 0.3;   // shrinkage applied to each tree's leaf outputs
    double l2_regularization = 1.0;   // λ: ridge penalty in the split gain / leaf
    double min_split_gain = 0.0;      // γ: minimum gain to make a split
    double min_child_hessian = 1e-3;  // minimum Σh in either child of a split

    // Fraction of rows held OUT of training and measured separately (#953).
    // Without it the reported metrics are in-sample: every node scores the rows it
    // trained on, which is the standard way a model flatters itself and has already
    // reached a client-facing report here.
    //
    // Separate from CalibrationConfig::holdout_fraction — a run may want either,
    // both, or neither, and one shared fraction would couple two decisions.
    //
    // **Must round-trip through to_json/from_json.** Every participant in a
    // coordinated run splits its own shard with this value, so a peer that parsed
    // a different one trains on rows its peers held back — and the "held-out"
    // figure is then measured partly on rows some node fit. That is invisible on
    // a single node and silently wrong on a cluster.
    //
    // Defaults to 0, so adding this moves no existing model's numbers.
    // The loss the trees are grown against. `binary_logistic` is churn's — scores map to
    // probabilities through a sigmoid and may be calibrated. `squared_error` is what a forecast
    // needs (#984, M-4): the residual IS the gradient, the Hessian is constant, and the model's
    // output is the prediction rather than something to squash.
    //
    // Nothing else in the learner changes — histogram building, split finding and the allreduce
    // are objective-agnostic. That is why M-4 needed an objective, not a second learner.
    std::string objective = "binary_logistic";  // squared_error or lambdamart
    double holdout_fraction = 0.0;

    // The JSON form (only the fields above; all optional with the defaults).
    nlohmann::json to_json() const;

    // Parse + validate at the boundary; the non-throwing inverse of to_json
    // (std::optional). std::nullopt unless `value` is an object whose present
    // fields are the right type and in range (positive counts, learning_rate > 0,
    // the regularizers >= 0), with no unknown keys. Omitted fields keep defaults.
    static std::optional<GbdtParams> from_json(const nlohmann::json& value);
};

// The GbdtParams JSON Schema (draft-07) — the code-owned source of truth;
// schemas/gbdt_params.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& gbdt_params_schema();

// --- The trained model -------------------------------------------------------

// One node of a binary regression tree. An internal node splits left when
// x[feature] < threshold, else right (the strict `<` makes the numeric threshold
// reproduce the training-time bin partition exactly). A leaf (feature < 0)
// contributes `value` to the raw score.
//
// `cover` is the node's training **weight** — the sum of Hessians of the rows that
// reached it at the boosting round that grew this tree (XGBoost's "cover", a global
// quantity identical on every cluster node since it comes off the allreduced
// histogram). It defines the background branch probabilities — `cover[child] /
// cover[node]` — that exact path-dependent TreeSHAP integrates over to explain a
// prediction (see tree_shap.hpp). It plays no part in `predict`.
struct TreeNode {
    int feature = -1;       // split feature index; < 0 marks a leaf
    double threshold = 0.0; // split boundary (the left bin group's upper edge)
    int left = -1;          // child node indices (internal only)
    int right = -1;
    double value = 0.0;     // leaf raw-score contribution
    double cover = 0.0;     // training weight (Σ Hessian) reaching this node — for SHAP
    // Where a missing value (NaN) goes. False sends it right, which is what the
    // strict `<` does with NaN and how every model trained here behaves. A model
    // brought from elsewhere (model_import.hpp) records its source's own choice:
    // XGBoost's default direction, LightGBM's missing type, ONNX's
    // nodes_missing_value_tracks_true (lake#681).
    bool missing_left = false;

    bool is_leaf() const noexcept { return feature < 0; }
    // The child a feature value takes at this internal node.
    int child_for(double value) const noexcept;
};

// A single regression tree: nodes[0] is the root.
struct RegressionTree {
    std::vector<TreeNode> nodes;

    // The leaf raw-score output for a feature row (values indexed by feature
    // index, in the model's feature ordering). An empty tree contributes 0. A
    // split on a feature the row does not carry reads 0.0, the padding TreeSHAP
    // uses (#1390), and a walk that leaves the tree stops: no index is read
    // unchecked, whatever the row or the tree.
    double predict(const std::vector<double>& features) const;
};

// A trained histogram GBDT: a base score plus an ordered ensemble of trees over a
// fixed feature ordering. This struct *is* the serialized weights body (it
// round-trips through to_json / from_json), carrying a validated JSON boundary
// (schemas/gbdt_model.schema.json, the pboettch draft-07 validator — like the Run
// record — because the body is read back from disk / a peer).
struct GbdtModel {
    // binary_logistic or squared_error
    std::string objective = "binary_logistic";
    double base_score = 0.0;       // initial raw score (logit of the base rate)
    double learning_rate = 0.3;    // recorded for provenance (already baked into leaves)
    std::vector<std::string> feature_names;     // the fixed feature ordering
    std::vector<RegressionTree> trees;

    // An optional fitted probability calibration (Platt / isotonic, calibration.hpp).
    // When present, `predict_proba` maps the raw score through it instead of a plain
    // sigmoid, so a calibrated model carries its calibration in its own weights body
    // (and `ranking` is unchanged — the map is monotone). Absent = uncalibrated.
    std::optional<Calibrator> calibration;

    std::size_t tree_count() const noexcept { return trees.size(); }
    std::size_t feature_count() const noexcept { return feature_names.size(); }

    // The raw score for a feature row: base_score + Σ tree outputs.
    double predict_raw(const std::vector<double>& features) const;
    // The predicted probability: the calibrated map of predict_raw when a
    // `calibration` is attached, else sigmoid(predict_raw).
    double predict_proba(const std::vector<double>& features) const;

    // The structural rules the JSON Schema cannot express (#1390): in every
    // tree, every split reads a feature in [0, feature_count()), and every
    // internal node's two children are distinct indices in the tree that come
    // after it, so a walk from the root always ends at a leaf. false with a
    // reason in `error` on the first violation. from_json and every model
    // importer (model_import.hpp) refuse a model that fails it, so prediction
    // and TreeSHAP only ever see a model that passes.
    bool validate_structure(std::string* error = nullptr) const;

    // Serialize / parse the weights body. from_json validates against the GBDT
    // model JSON Schema and validate_structure before returning, so a malformed
    // body fails at the boundary with a path-qualified reason in `error` instead
    // of corrupting state.
    nlohmann::json to_json() const;
    static std::optional<GbdtModel> from_json(const nlohmann::json& value,
                                              std::string* error = nullptr);
};

// The GBDT model JSON Schema (draft-07), exposed for tests/introspection.
const nlohmann::json& gbdt_model_schema();

// --- Evaluation --------------------------------------------------------------

// Binary classification metrics over predicted probabilities + {0,1} labels.
struct GbdtMetrics {
    double auc = 0.0;        // area under the ROC curve (Mann-Whitney)
    double log_loss = 0.0;   // mean binary cross-entropy
    double accuracy = 0.0;   // fraction correct at a 0.5 threshold

    // The JSON form: { "auc", "log_loss", "accuracy" }. The shape stored in a
    // Model's free-form `metrics`.
    nlohmann::json to_json() const;
};

// Compute AUC (Mann-Whitney U, ties at 0.5), mean log-loss (probabilities clamped
// off 0/1), and accuracy@0.5. AUC is 0.5 when one class is absent (undefined ROC).
GbdtMetrics evaluate_binary(const std::vector<double>& probabilities,
                            const std::vector<double>& labels);

// --- Training ----------------------------------------------------------------

// The cross-node combine the distributed trainer needs: reduce `local` elementwise
// across every participant and return the global result (Sum / Min / Max). The
// single-node path passes an identity (this node's shard is the whole dataset).
// Returns an empty vector to signal a collective failure — the trainer then fails
// with a status rather than training on partial data.
enum class GbdtReduce { Sum, Min, Max };
using AllReduce =
    std::function<std::vector<double>(const std::string& tag,
                                      const std::vector<double>& local,
                                      GbdtReduce op)>;

// Optional resume + per-round hook for **checkpoint / restart** (RULE-5 / RSK-9,
// #468). Lets the distributed train job checkpoint the partial model after each
// boosting round and **resume from the last checkpoint** after a spot eviction,
// without re-growing the trees already committed. The default {} is "train from
// scratch, no hook" — exactly the unchanged loop, so existing callers are unaffected.
// See docs/compute/job-checkpoint.md.
struct TrainHooks {
    // Optional objective callback evaluated before every boosting round. It
    // receives this shard's current raw scores and labels and fills one
    // gradient/Hessian pair per row. Ranking uses this to supply group-local
    // LambdaMART derivatives while reusing the distributed GBDT core (#1048).
    std::function<compute::ComputeStatus(
        const std::vector<double>& raw, const std::vector<double>& labels,
        std::vector<double>& gradient, std::vector<double>& hessian)>
        objective_gradient;

    // Seed the ensemble from a previously-checkpointed partial model + its
    // accumulated per-feature gains: training resumes at tree index
    // resume_model->trees.size() (the running predictions are recomputed from those
    // trees). The model's feature ordering / objective must match the fresh run; an
    // empty / absent resume_model trains from scratch.
    std::optional<GbdtModel> resume_model;
    std::vector<double> resume_feature_importances;

    // Called after each tree is committed: (trees built so far, the partial model,
    // the current per-feature importances). The natural place to checkpoint the
    // partial state. Return false to stop early (returning the partial model with a
    // success status); true (or an empty hook) to continue. NOTE for a distributed
    // job: the per-round histogram allreduce is lock-step across nodes, so a stop
    // must be agreed by all participants — run_train_job only ever stops
    // all-or-nothing; an individual node never drains a collective job unilaterally.
    std::function<bool(int trees_built, const GbdtModel& partial,
                       const std::vector<double>& importances)>
        on_round;

    // Called after each committed tree with globally reduced training metrics.
    // All participants must enable this together (non-root hooks may be no-ops).
    // No hook means no metric computation or collective. DP callers force
    // suppress_metrics so private training data never enters this path (#1137).
    // Returning false stops training with an error so a lost metric batch is not
    // silently reported as a fully tracked run. The status names only the tree
    // it stopped after: the hook knows why (a failed write, a pruned or
    // cancelled trial, a spent budget) and its owner reports that reason.
    std::function<bool(int step, const std::map<std::string, double>& values)>
        on_metrics;
    bool suppress_metrics = false;
};

// The outcome of training: the model, the per-feature total split gain (the
// model's drivers / importances, in feature order), and the status.
struct GbdtTrainResult {
    GbdtModel model;
    std::vector<double> feature_importances;  // total split gain per feature
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// Train a histogram GBDT over this node's shard. The objective is selected by
// GbdtParams: binary logistic classification or squared-error regression.
//   columns[f][r] = feature f's value for local row r (host memory),
//   feature_names = the feature ordering (same on every node),
//   labels[r]     = the classification or regression target for local row r.
// `allreduce` combines per-node statistics (global bin edges, the base label rate,
// the gradient/Hessian histograms at each tree level) across the cluster; `tag`
// namespaces those rounds. With an identity `allreduce` this is exactly the
// single-node trainer.
//
// Non-throwing. Fails (a failing ComputeStatus, empty model) when columns is
// ragged, feature_names does not match the column count, labels length disagrees,
// there are no rows globally, only one label class is present, or a collective
// round fails.
// `hooks` (default {}) enables checkpoint / restart: seed the ensemble from a
// resumed partial model and/or checkpoint after each committed tree (see TrainHooks).
GbdtTrainResult train_gbdt(const std::vector<std::vector<double>>& columns,
                           const std::vector<std::string>& feature_names,
                           const std::vector<double>& labels,
                           const GbdtParams& params, const AllReduce& allreduce,
                           const std::string& tag = "gbdt",
                           const TrainHooks& hooks = {});

// Single-node convenience: train over the complete dataset with an identity
// reduction (no cluster). The numerical reference the distributed path matches.
GbdtTrainResult train_gbdt(const std::vector<std::vector<double>>& columns,
                           const std::vector<std::string>& feature_names,
                           const std::vector<double>& labels,
                           const GbdtParams& params);

// --- Calibration -------------------------------------------------------------

// Fit a probability calibration for a trained model on a **held-out** set and
// attach it (so `model.predict_proba` returns calibrated probabilities). The
// held-out rows must NOT be the rows the model trained on — calibrating on the
// training data is leaky (isotonic in particular fits it perfectly). `holdout`
// columns are `[feature][row]` in the model's `feature_names` order;
// `holdout_labels` the {0,1} targets. Non-throwing: fails (a failing ComputeStatus,
// no calibration attached) when the shapes disagree or the held-out labels are a
// single class. AUC is preserved (the map is monotone); reliability (Brier / ECE)
// improves. See docs/ml/calibration.md.
compute::ComputeStatus calibrate_model(
    GbdtModel& model, const std::vector<std::vector<double>>& holdout_columns,
    const std::vector<double>& holdout_labels, CalibrationMethod method);

}  // namespace zetta::ml
