// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zetta/compute/compute_status.hpp"
#include "zetta/ml/gbdt.hpp"

// Batch scoring / rank-at-risk over a trained GBDT `Model` (FEAT-6 / CMD-10, #392;
// part of the native distributed accelerated-compute pass for UC-1 churn, #264).
// This is the *inference* counterpart to the GBDT trainer (gbdt.hpp): run a set of
// entities through a trained model to produce, per entity, a **calibrated churn
// risk**, then rank them into an **at-risk list** (highest risk first) — the
// CMD-10 "rank-at-risk" deliverable retention teams act on.
//
// **Calibrated risk, for free.** The model is binary-logistic (trained on
// log-loss, a proper scoring rule), so its predicted probability
// `GbdtModel::predict_proba` (= sigmoid of the raw ensemble score) is already a
// calibrated risk in [0, 1] — no separate Platt / isotonic step is needed. The
// risk a customer churns *is* `predict_proba`.
//
// **Tree-predict, the GPU-native target.** Per zetta's AI-compute direction (see
// CLAUDE.md → "AI compute — GPU-native, CPU fallback, distributed") scoring runs
// through the `ComputeDevice` seam, so it executes on whatever device the feature
// data lives on. The **ranking** is already device-native: the distributed job
// (gbdt_score_job.hpp) ranks the global at-risk list with the device `sort_by_key`
// operator (the op the device header documents as "sort entities by risk score,
// carrying their ids") — GPU on a CUDA frame, CPU fallback otherwise. The
// tree-traversal numerics in this file are the **host-side reference** (the same
// convention the trainer ships with — read device-backed columns back, predict
// host-side); a device-native tree-predict kernel is the GPU-native optimization
// behind this same API.
//
// Non-throwing throughout (zetta-core's no-exceptions convention): every entry
// point returns a `compute::ComputeStatus`. See docs/ml/gbdt-score.md.
namespace zetta {
namespace ml { class Model; }
}

namespace zetta::ml {

// One scored entity: its key and the model's calibrated churn risk (the predicted
// probability in [0, 1]). The unit of the ranked at-risk list.
struct EntityScore {
    std::int64_t entity = 0;  // the entity key (e.g. the CRM account index)
    double risk = 0.0;        // calibrated churn risk = predict_proba in [0, 1]
};

// The outcome of scoring + ranking: the at-risk list and the status that explains
// any failure (mirrors GbdtTrainResult / DataFrameResult). `ranked` is populated
// only when `status.ok()`.
struct ScoreResult {
    std::vector<EntityScore> ranked;  // entities, highest risk first
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// Predict the calibrated risk for each input row (host-side reference). The
// provided `columns` are named by `feature_names`; this aligns them to the model's
// own feature ordering (`model.feature_names`) by name, builds each row's feature
// vector in that order, and writes `predict_proba` into `risks` (resized to the
// row count, row-aligned with the inputs — NOT ranked).
//
// Non-throwing. Fails with InvalidArgument when `columns` and `feature_names`
// disagree in length, a column is ragged (rows differ), or a feature the model
// needs has no matching named column.
compute::ComputeStatus predict_risk(
    const GbdtModel& model, const std::vector<std::vector<double>>& columns,
    const std::vector<std::string>& feature_names, std::vector<double>& risks);

// Batch-score a design matrix through a trained model and rank the entities by
// risk descending (the at-risk list), single-node. `entities[r]` is the key for
// the row whose features are `columns[*][r]`. Ties are broken by entity ascending,
// so the ranking is **deterministic** (independent of input order). When
// `top_k > 0` only the `top_k` highest-risk entities are returned; 0 returns all.
//
// The numerical reference the distributed `run_score_job` matches. Non-throwing —
// surfaces the same failures as predict_risk, plus an entity/row count mismatch.
ScoreResult score_gbdt(const GbdtModel& model,
                       const std::vector<std::vector<double>>& columns,
                       const std::vector<std::string>& feature_names,
                       const std::vector<std::int64_t>& entities,
                       std::size_t top_k = 0);

// Load a trained `GbdtModel` from a registered `Model` entity: read its serialized
// weights body and parse it through the GBDT model JSON Schema boundary
// (GbdtModel::from_json — the pboettch draft-07 validator, schemas/gbdt_model.schema.json).
// Because the body is read back from disk / a peer, re-validating here is the
// boundary that rejects a malformed body with a path-qualified reason in `error`
// instead of scoring against corrupt weights.
//
// Returns nullopt when the weights body is not present locally (transfer still in
// flight), the body is not valid JSON, or it fails schema validation.
std::optional<GbdtModel> load_gbdt_model(const Model& model,
                                         std::string* error = nullptr);

}  // namespace zetta::ml
