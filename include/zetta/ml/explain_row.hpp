#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/model.hpp"

// **Why is THIS one at risk** (#948 step 6, #960 item 5).
//
// A run's population drivers say what the model leans on across everybody:
// escalations matter most, billing disputes second. They are the same list for every
// account, so they cannot answer the question a person actually asks when they open
// the at-risk list — *"why is the top one there?"* Two accounts with the same score
// can be there for opposite reasons, and the population ranking hides that.
//
// The engine already computes the per-row answer: exact path-dependent TreeSHAP runs
// for every entity during a run's `explain` step. Those explanations live in the step
// executor's memory and die with the run, which is why this exists.
//
// **Nothing is stored to fix that.** `tree_shap` is a pure function of (model,
// features) — no device, no I/O, no shared state — and one row costs a walk of the
// trees. So a per-row explanation is RECOMPUTED from the registered Model whenever it
// is asked for, exactly as `score_table` recomputes the ranking. A stored copy would
// be a second version of a number that can drift from the model that produced it.
//
// **The explanation is in raw-score (logit) space, and it is exact.** SHAP is additive
// in the model's additive output, so the contributions sum to the prediction:
//
//     raw  ==  base_value + Σ contributions          (within floating point)
//
// `residual` carries the leftover so a caller can check that rather than trust it.
// Probabilities are NOT additive, so there is no honest way to hand back
// contributions that sum to the risk — the risk is reported beside them instead.
//
// **A contribution is not a cause.** It says how much this account's value for that
// column moved the model's output relative to a typical account. Whether the column
// causes the outcome is a question the model was never asked.
//
// Non-throwing throughout. See docs/ml/explaining-one-row.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

// One feature's contribution to one row's prediction.
struct FeatureContribution {
    std::string feature;        // the model's feature name
    double value = 0.0;         // this row's value for it
    double contribution = 0.0;  // φ: signed, raw-score space; + pushed the score up
};

// One row's explanation.
struct RowExplanation {
    // The entity this explains, when it was read from a dataset. Absent for a row
    // supplied directly, which may not correspond to any entity at all.
    std::optional<std::int64_t> entity;

    double base_value = 0.0;  // the model's expected raw score — what an account with
                              // nothing notable about it would get
    double raw = 0.0;         // predict_raw for this row
    double risk = 0.0;        // the same prediction as a probability, for comparison
                              // with a ranked list. NOT the sum of contributions.
    double residual = 0.0;    // raw - (base_value + Σ contributions); ~0 when exact

    // Every feature, ordered by |contribution| descending — strongest influence
    // first, which is the order a reader wants and never the model's storage order.
    // Features that moved this row's prediction the least sort last rather than
    // being dropped: "this column did nothing here" is an answer.
    std::vector<FeatureContribution> contributions;

    std::string error;  // non-empty on failure; everything above is then unset
    bool ok() const noexcept { return error.empty(); }
};

// Explain one row supplied directly — an account that is not in the data at all, or a
// hypothetical. `row` must carry every column the MODEL names; a missing one is a hard
// failure rather than a zero-fill, because zero is a real value to a GBDT and would
// produce a confidently wrong explanation instead of a visibly broken one.
RowExplanation explain_row(const Model& model, const nlohmann::json& row);

// Explain one entity of a dataset, read compute-near-data from this node's own
// partitions of `table_id`. The entity's row never crosses the wire.
//
// **Only this node's partitions are searched**, matching `score_table`. On a cluster
// an entity owned by another node reports not-found here rather than silently
// returning a wrong or empty answer; the error names the limitation.
RowExplanation explain_entity(Node& node, const Model& model,
                              const std::string& table_id,
                              const zetta::data::DatasetContract& contract,
                              compute::ComputeDevice& device, std::int64_t entity);

}  // namespace zetta::ml
