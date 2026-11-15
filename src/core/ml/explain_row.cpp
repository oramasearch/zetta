// SPDX-License-Identifier: MIT

#include "zetta/ml/explain_row.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/score_table.hpp"
#include "zetta/ml/tree_shap.hpp"
#include "zetta/node.hpp"

namespace zetta::ml {
namespace {

using ddf::ColumnSchema;
using ddf::DataFrameSchema;
using DataType = compute::DataType;

// The frame column the entity is read into — one canonical name inside the pipeline,
// matching the training and scoring paths.
constexpr std::string_view kEntityColumn = "entity";

RowExplanation failure(std::string reason) {
    RowExplanation result;
    result.error = std::move(reason);
    return result;
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) out += ", ";
        out += items[index];
    }
    return out;
}

// The shared half: given the model's feature values in the model's own order, run
// TreeSHAP and shape the result.
RowExplanation explain_values(const GbdtModel& gbdt,
                              const std::vector<std::string>& features,
                              const std::vector<double>& values) {
    const ShapExplanation shap = tree_shap(gbdt, values);

    RowExplanation result;
    result.base_value = shap.base_value;
    result.raw = shap.predicted;
    // Logistic link, matching predict_proba. Reported beside the contributions, never
    // as their sum: probabilities are not additive, so contributions that summed to a
    // probability would be arithmetic nobody could check.
    result.risk = 1.0 / (1.0 + std::exp(-shap.predicted));
    result.residual = shap.efficiency_residual();

    result.contributions.reserve(features.size());
    for (std::size_t index = 0; index < features.size(); ++index) {
        result.contributions.push_back(
            FeatureContribution{.feature = features[index],
                                .value = index < values.size() ? values[index] : 0.0,
                                .contribution = index < shap.values.size()
                                                    ? shap.values[index]
                                                    : 0.0});
    }
    // Strongest influence first, whichever direction it pushed. A stable tie-break on
    // the model's feature order keeps the output deterministic when two features
    // contributed identically — which happens whenever both contributed nothing.
    std::stable_sort(result.contributions.begin(), result.contributions.end(),
                     [](const FeatureContribution& left, const FeatureContribution& right) {
                         return std::abs(left.contribution) > std::abs(right.contribution);
                     });
    return result;
}

}  // namespace

RowExplanation explain_row(const Model& model, const nlohmann::json& row) {
    if (!row.is_object()) return failure("explain: the row is not an object");

    std::string weights_error;
    const std::optional<GbdtModel> gbdt = load_gbdt_model(model, &weights_error);
    if (!gbdt) {
        return failure("explain: model '" + model.id() + "' weights unavailable: " +
                       weights_error);
    }
    // The MODEL's feature order is the truth. A model trained on
    // [failed_payments, logins_30d] explained against some other ordering would
    // attribute each contribution to the wrong column — a wrong answer that looks
    // exactly like a right one.
    const std::vector<std::string> features = model_feature_columns(model);
    if (features.empty()) {
        return failure("explain: model '" + model.id() +
                       "' records no feature columns, so nothing says what to feed it");
    }

    // Text categories by their original column (#1203), encoded with the levels
    // fitted at training time; an unseen level encodes as all zeros.
    nlohmann::json encoded = row;
    if (const std::string reason = encode_model_row(model, encoded); !reason.empty()) {
        return failure("explain: " + reason);
    }
    std::vector<double> values(features.size());
    for (std::size_t index = 0; index < features.size(); ++index) {
        const auto found = encoded.find(features[index]);
        // Hard failure, not a zero-fill: zero is a real value to a GBDT, so a missing
        // column would be explained as "this account has none of these" rather than
        // reported as missing.
        if (found == encoded.end() || found->is_null()) {
            return failure("explain: the row has no '" + features[index] +
                           "'; the model needs " + join(model_input_names(model)));
        }
        if (found->is_boolean()) {
            values[index] = found->get<bool>() ? 1.0 : 0.0;
        } else if (found->is_number()) {
            values[index] = found->get<double>();
        } else {
            return failure("explain: the row has a non-numeric '" + features[index] + "'");
        }
    }
    return explain_values(*gbdt, features, values);
}

RowExplanation explain_entity(Node& node, const Model& model, const std::string& table_id,
                              const zetta::data::DatasetContract& contract,
                              compute::ComputeDevice& device, std::int64_t entity) {
    if (table_id.empty()) return failure("explain: no dataset table given");
    if (contract.entity_column.empty()) {
        return failure("explain: the dataset contract names no entity column");
    }

    std::string weights_error;
    const std::optional<GbdtModel> gbdt = load_gbdt_model(model, &weights_error);
    if (!gbdt) {
        return failure("explain: model '" + model.id() + "' weights unavailable: " +
                       weights_error);
    }
    const std::vector<std::string> features = model_feature_columns(model);
    if (features.empty()) {
        return failure("explain: model '" + model.id() +
                       "' records no feature columns, so nothing says what to feed it");
    }

    DataFrameSchema schema;
    schema.columns.push_back(ColumnSchema{.name = std::string(kEntityColumn),
                                          .field = contract.entity_column,
                                          .dtype = DataType::Int64,
                                          .entity_keys = contract.entity_keys});
    std::string encoding_error;
    const std::vector<data::CategoricalEncoding> encodings =
        model_categorical_encodings(model.value().feature_schema, &encoding_error);
    if (!encoding_error.empty()) {
        return failure("explain: model '" + model.id() +
                       "' has an invalid categorical encoding: " + encoding_error);
    }
    for (const std::string& feature : features) {
        schema.columns.push_back(model_feature_column_schema(feature, encodings));
    }

    // Gated onto the owner thread (#624, #956). This reads the partition catalog and
    // the blob store off the Node tree, which is owner-thread only — through the API
    // the caller is not the owner, and an ungated call never returns.
    ddf::LocalShardResult shard;
    node.run_on_owner([&] {
        shard = ddf::load_local_partitions(node, table_id, schema, device);
    });
    if (!shard.ok()) {
        return failure("explain: could not read '" + table_id +
                       "' with the model's feature columns (" + join(features) +
                       "): " + shard.status.message);
    }
    if (shard.frame.row_count() == 0) {
        return failure("explain: this node holds no partitions of '" + table_id + "'");
    }

    std::vector<std::int64_t> entities(shard.frame.row_count());
    if (const compute::ComputeStatus status =
            shard.frame.to_host<std::int64_t>(kEntityColumn, entities);
        !status.ok()) {
        return failure("explain: read the entity column: " + status.message);
    }
    const auto found = std::find(entities.begin(), entities.end(), entity);
    if (found == entities.end()) {
        // Named as a placement fact rather than "no such account": on a cluster the
        // entity may exist and simply live on another node's partitions, and a bare
        // not-found would send someone looking for a data problem that isn't there.
        return failure("explain: entity " + std::to_string(entity) + " is not in '" +
                       table_id + "' on this node — it may be owned by another node's "
                       "partitions, or not be in the dataset at all");
    }
    const std::size_t row = static_cast<std::size_t>(found - entities.begin());

    std::vector<double> values(features.size());
    for (std::size_t index = 0; index < features.size(); ++index) {
        std::vector<double> column(shard.frame.row_count());
        if (const compute::ComputeStatus status =
                shard.frame.to_host<double>(features[index], column);
            !status.ok()) {
            return failure("explain: read feature '" + features[index] + "': " +
                           status.message);
        }
        values[index] = column[row];
    }

    RowExplanation result = explain_values(*gbdt, features, values);
    result.entity = entity;
    return result;
}

}  // namespace zetta::ml
