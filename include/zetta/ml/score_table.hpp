#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/model.hpp"

// **Score a registered model over a dataset, after the run that trained it ended**
// (#948 steps 5 and 7).
//
// The at-risk list a churn run produces lives in the step executor's memory and dies
// with the run. So *"which accounts are most at risk?"* — the question the whole
// product exists to answer — had nowhere to read from the moment the run finished,
// and *"what would this account score?"* had no path at all.
//
// Nothing needs storing to fix that, because everything required is already durable:
//
//   * the **Model** entity carries its ordered `feature_schema.features`, its ranked
//     `drivers`, its `metrics`, and its serialized weights body;
//   * the **dataset** is a partitioned table in the catalog;
//   * the **contract** (`zetta::data::DatasetContract`) says which column is the
//     entity and which are the features.
//
// So the answer is RECOMPUTED from the live sources rather than kept as a copy that
// can drift — the same rule the `system.*` views follow. Scoring is cheap next to
// training: one near-data shard read and a forward pass per tree.
//
// **Compute-near-data.** The shard comes from this node's own partition bodies
// (`ddf::load_local_partitions`), so the rows never cross the wire.
//
// **The model's feature order is the truth, not the contract's.** A model trained on
// [failed_payments, logins_30d, nps] must be fed those columns in that order; feeding
// it the contract's current column list would silently score against a different
// matrix if the table has since gained a column. A feature the table no longer has is
// a hard failure, not a zero-fill — a zero is a real value to a GBDT, and scoring
// every account as if it never logged in would be confidently wrong rather than
// visibly broken.
//
// Non-throwing throughout. See docs/ml/scoring-a-dataset.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

// The outcome of scoring a dataset through a trained model.
struct TableScoreResult {
    // Entities ranked by risk, highest first (ties broken by entity ascending, so
    // the ranking is deterministic). Capped by `top_k` when one was given.
    std::vector<EntityScore> ranked;

    // The feature columns the model was trained on, in the order it expects them —
    // what the caller should show beside a score, and what `drivers` names.
    std::vector<std::string> feature_columns;

    // The model's ranked drivers (feature → importance, strongest first), read from
    // the Model record. Population-level, not per-row.
    std::vector<ModelDriver> drivers;
    std::shared_ptr<const data::EntityKeyMapping> entity_keys;

    // How much was actually read, so a caller can tell "scored nothing" from
    // "scored everything and nothing was at risk".
    std::size_t partitions_read = 0;
    std::uint64_t rows_scored = 0;

    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// Score every row of `table_id` (a `Table::id()`) through `model`, ranked by risk.
//
// `contract` supplies the entity column — the rest of the columns come from the
// MODEL's own feature list, for the reason in the header note. `top_k` caps the
// ranked list (0 = all).
//
// Fails with a path-qualified reason when: the model has no weights body present
// locally (a transfer still in flight), the body fails the GBDT schema boundary, the
// model records no feature list, the table has none of the model's features, or the
// shard cannot be read.
TableScoreResult score_table(Node& node, const Model& model, const std::string& table_id,
                             const zetta::data::DatasetContract& contract,
                             compute::ComputeDevice& device, std::size_t top_k = 0);

// Score rows supplied INLINE — an account that is not in the dataset at all.
//
// This is *"take an account not in the training data and ask what it would score"*
// (#948 step 7). The rows are JSON objects keyed by the model's feature column names;
// a row missing one of them fails rather than scoring against a zero-filled matrix.
// `entities[i]` labels `rows[i]`; when empty the rows are labelled by their index.
// `entity_keys` restores text ids in the answer: the caller resolves the model's
// mapping (Node::resolve_entity_key_mapping, which reads the entity-key table,
// #1255); when it is null only an inline mapping on the model is read.
TableScoreResult score_rows(const Model& model, const std::vector<nlohmann::json>& rows,
                            const std::vector<std::int64_t>& entities = {},
                            std::size_t top_k = 0,
                            std::shared_ptr<const data::EntityKeyMapping> entity_keys = {});

// The ordered feature columns `model` was trained on, read from its
// `feature_schema.features`. Empty when the model records none — which makes it
// unscorable, and is reported as that rather than guessed around.
std::vector<std::string> model_feature_columns(const Model& model);

// --- categorical inputs (#1203) ------------------------------------------------
//
// A model trained on a table with text categories records the fitted one-hot
// encodings in `feature_schema.categorical_encoding`. Every predict path (score,
// explain, the served predict endpoint) applies THAT list to the rows it is given:
// a row carries the original column (`plan: "pro"`), the model's encoded features
// (`plan=pro`) are derived from it, and an unseen level encodes as all zeros.

// The encodings `feature_schema` records; empty for a model trained on numeric
// columns only, or when the record is malformed (reported by `error`).
std::vector<data::CategoricalEncoding> model_categorical_encodings(
    const nlohmann::json& feature_schema, std::string* error = nullptr);

// The row contract predict reads, in the model's feature order: one entry per
// input column, `{name, type: "number"}` for a numeric feature and
// `{name, type: "string", encoding: "one_hot", levels, features, unseen}` for a
// text category, named by its original column. Written to `feature_schema.inputs`.
nlohmann::json model_input_contract(const nlohmann::json& feature_schema);

// Write `encodings` (when any) to `feature_schema.categorical_encoding` and the row
// contract to `feature_schema.inputs`. Every trainer that one-hot encodes text
// categories calls it on the model it registers.
nlohmann::json with_model_inputs(nlohmann::json feature_schema,
                                 const std::vector<data::CategoricalEncoding>& encodings);

// The names in model_input_contract(), in order: what an error says a row needs.
std::vector<std::string> model_input_names(const Model& model);

// Encode `row`'s categorical columns in place with the model's encodings. Returns
// an empty string, or why the row cannot be encoded.
std::string encode_model_row(const Model& model, nlohmann::json& row);

// The frame column a table read projects for one of the model's features: a
// Float64 read straight through, or a 0/1 indicator read from the original text
// column when the feature is a categorical level.
ddf::ColumnSchema model_feature_column_schema(
    const std::string& feature, const std::vector<data::CategoricalEncoding>& encodings);

}  // namespace zetta::ml
