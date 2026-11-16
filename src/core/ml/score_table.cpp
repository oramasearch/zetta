// SPDX-License-Identifier: MIT

#include "zetta/ml/score_table.hpp"

#include <algorithm>
#include <utility>

#include "zetta/data/entity_key_table.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/node.hpp"

namespace zetta::ml {
namespace {

using ddf::ColumnSchema;
using ddf::DataFrameSchema;
using DataType = compute::DataType;

// The frame column the entity is read into. One canonical name inside the pipeline,
// whatever the customer's own spelling is — matching the training path.
constexpr std::string_view kEntityColumn = "entity";

TableScoreResult failure(std::string reason) {
    TableScoreResult result;
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

}  // namespace

std::vector<std::string> model_feature_columns(const Model& model) {
    std::vector<std::string> columns;
    const nlohmann::json& schema = model.value().feature_schema;
    if (!schema.is_object()) return columns;
    const auto features = schema.find("features");
    if (features == schema.end() || !features->is_array()) return columns;
    for (const nlohmann::json& feature : *features) {
        if (feature.is_string() && !feature.get<std::string>().empty()) {
            columns.push_back(feature.get<std::string>());
        }
    }
    return columns;
}

std::vector<data::CategoricalEncoding> model_categorical_encodings(
    const nlohmann::json& feature_schema, std::string* error) {
    if (!feature_schema.is_object()) return {};
    const auto found = feature_schema.find("categorical_encoding");
    if (found == feature_schema.end()) return {};
    return data::categorical_encodings_from_json(*found, error);
}

nlohmann::json model_input_contract(const nlohmann::json& feature_schema) {
    nlohmann::json inputs = nlohmann::json::array();
    const std::vector<data::CategoricalEncoding> encodings =
        model_categorical_encodings(feature_schema);
    const auto features = feature_schema.is_object() ? feature_schema.find("features")
                                                     : feature_schema.end();
    if (!feature_schema.is_object() || features == feature_schema.end() ||
        !features->is_array()) {
        return inputs;
    }
    std::vector<std::string> listed;
    for (const nlohmann::json& feature : *features) {
        if (!feature.is_string()) continue;
        const std::string name = feature.get<std::string>();
        if (const auto categorical = data::find_categorical_feature(encodings, name)) {
            const data::CategoricalEncoding& encoding = *categorical->encoding;
            if (std::ranges::contains(listed, encoding.column)) continue;
            listed.push_back(encoding.column);
            nlohmann::json entry = encoding.to_json();
            entry.erase("column");
            entry["name"] = encoding.column;
            entry["type"] = "string";
            inputs.push_back(std::move(entry));
            continue;
        }
        inputs.push_back({{"name", name}, {"type", "number"}});
    }
    return inputs;
}

nlohmann::json with_model_inputs(nlohmann::json feature_schema,
                                 const std::vector<data::CategoricalEncoding>& encodings) {
    if (!feature_schema.is_object()) feature_schema = nlohmann::json::object();
    if (!encodings.empty()) {
        feature_schema["categorical_encoding"] = data::to_json(encodings);
    }
    feature_schema["inputs"] = model_input_contract(feature_schema);
    return feature_schema;
}

std::vector<std::string> model_input_names(const Model& model) {
    std::vector<std::string> names;
    for (const nlohmann::json& input : model_input_contract(model.value().feature_schema)) {
        names.push_back(input.at("name").get<std::string>());
    }
    return names;
}

std::string encode_model_row(const Model& model, nlohmann::json& row) {
    std::string error;
    const std::vector<data::CategoricalEncoding> encodings =
        model_categorical_encodings(model.value().feature_schema, &error);
    if (!error.empty()) return "the model's categorical encoding is invalid: " + error;
    if (encodings.empty()) return {};
    return data::encode_categorical_row(encodings, row);
}

ddf::ColumnSchema model_feature_column_schema(
    const std::string& feature, const std::vector<data::CategoricalEncoding>& encodings) {
    if (const auto categorical = data::find_categorical_feature(encodings, feature)) {
        return ColumnSchema{.name = feature,
                            .field = categorical->encoding->column,
                            .dtype = DataType::Float64,
                            .category_level = categorical->level};
    }
    return ColumnSchema{.name = feature, .field = feature, .dtype = DataType::Float64};
}

TableScoreResult score_table(Node& node, const Model& model, const std::string& table_id,
                             const zetta::data::DatasetContract& contract,
                             compute::ComputeDevice& device, std::size_t top_k) {
    if (table_id.empty()) return failure("score: no dataset table given");
    if (contract.entity_column.empty()) {
        return failure("score: the dataset contract names no entity column");
    }

    std::string weights_error;
    const std::optional<GbdtModel> gbdt = load_gbdt_model(model, &weights_error);
    if (!gbdt) {
        return failure("score: model '" + model.id() + "' weights unavailable: " +
                       weights_error);
    }

    // The MODEL's feature order, not the contract's — see the header note.
    const std::vector<std::string> features = model_feature_columns(model);
    if (features.empty()) {
        return failure("score: model '" + model.id() +
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
        return failure("score: model '" + model.id() +
                       "' has an invalid categorical encoding: " + encoding_error);
    }
    for (const std::string& feature : features) {
        schema.columns.push_back(model_feature_column_schema(feature, encodings));
    }

    // Gated onto the owner thread (#624, #956). This reads the partition catalog
    // and the blob store off the Node tree, which is owner-thread only. In a test
    // the caller IS the owner and it works; through the API it is not, and the
    // call never returns — a `score` over 15,000 rows was killed at ten minutes
    // while the same work takes 40 ms in process. Identical to the gate the churn
    // executor's shard load needed, for identical reasons.
    ddf::LocalShardResult shard;
    node.run_on_owner([&] {
        shard = ddf::load_local_partitions(node, table_id, schema, device);
    });
    if (!shard.ok()) {
        // A column the model needs and the table does not have surfaces here. Say
        // which columns were expected: the remedy is a query that selects them.
        return failure("score: could not read '" + table_id +
                       "' with the model's feature columns (" + join(features) +
                       "): " + shard.status.message);
    }

    TableScoreResult result;
    result.entity_keys = contract.entity_keys;
    result.feature_columns = features;
    result.drivers = ranked_drivers(model.value().feature_schema);
    result.partitions_read = shard.partitions_read;
    result.rows_scored = shard.rows_read;
    if (shard.frame.row_count() == 0) {
        // Not an error: a node holding none of the table's partitions has nothing to
        // score, and an empty ranking says exactly that.
        return result;
    }

    std::vector<std::int64_t> entities(shard.frame.row_count());
    if (const compute::ComputeStatus status =
            shard.frame.to_host<std::int64_t>(kEntityColumn, entities);
        !status.ok()) {
        return failure("score: read the entity column: " + status.message);
    }

    std::vector<std::vector<double>> columns(features.size());
    for (std::size_t index = 0; index < features.size(); ++index) {
        columns[index].resize(shard.frame.row_count());
        if (const compute::ComputeStatus status =
                shard.frame.to_host<double>(features[index], columns[index]);
            !status.ok()) {
            return failure("score: read feature '" + features[index] + "': " +
                           status.message);
        }
    }

    ScoreResult scored = score_gbdt(*gbdt, columns, features, entities, top_k);
    if (!scored.ok()) return failure("score: " + scored.status.message);
    result.ranked = std::move(scored.ranked);
    return result;
}

TableScoreResult score_rows(const Model& model, const std::vector<nlohmann::json>& rows,
                            const std::vector<std::int64_t>& entities,
                            std::size_t top_k,
                            std::shared_ptr<const data::EntityKeyMapping> entity_keys) {
    if (rows.empty()) return failure("score: no rows given");
    if (!entities.empty() && entities.size() != rows.size()) {
        return failure("score: " + std::to_string(entities.size()) + " entity keys for " +
                       std::to_string(rows.size()) + " rows");
    }

    std::string weights_error;
    const std::optional<GbdtModel> gbdt = load_gbdt_model(model, &weights_error);
    if (!gbdt) {
        return failure("score: model '" + model.id() + "' weights unavailable: " +
                       weights_error);
    }
    const std::vector<std::string> features = model_feature_columns(model);
    if (features.empty()) {
        return failure("score: model '" + model.id() +
                       "' records no feature columns, so nothing says what to feed it");
    }

    // Text categories by their original column (#1203), encoded with the levels
    // fitted at training time. The encodings are parsed once for the batch, and a
    // row is copied only when the model has categories to write into it.
    std::string encoding_error;
    const std::vector<data::CategoricalEncoding> encodings =
        model_categorical_encodings(model.value().feature_schema, &encoding_error);
    if (!encoding_error.empty()) {
        return failure("score: model '" + model.id() +
                       "' has an invalid categorical encoding: " + encoding_error);
    }

    std::vector<std::vector<double>> columns(features.size());
    for (std::vector<double>& column : columns) column.resize(rows.size());
    nlohmann::json encoded_row;
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (!rows[row].is_object()) {
            return failure("score: row " + std::to_string(row) + " is not an object");
        }
        const nlohmann::json* source = &rows[row];
        if (!encodings.empty()) {
            encoded_row = rows[row];
            if (const std::string reason = data::encode_categorical_row(encodings, encoded_row);
                !reason.empty()) {
                return failure("score: row " + std::to_string(row) + ": " + reason);
            }
            source = &encoded_row;
        }
        const nlohmann::json& encoded = *source;
        for (std::size_t index = 0; index < features.size(); ++index) {
            const auto found = encoded.find(features[index]);
            // A missing feature is a hard failure. Zero-filling it would score the
            // row as if the value were measured and happened to be zero — a real
            // value to a GBDT, and a confidently wrong answer rather than a visible
            // one.
            if (found == encoded.end() || found->is_null()) {
                return failure("score: row " + std::to_string(row) + " has no '" +
                               features[index] + "'; the model needs " +
                               join(model_input_names(model)));
            }
            if (found->is_boolean()) {
                columns[index][row] = found->get<bool>() ? 1.0 : 0.0;
            } else if (found->is_number()) {
                columns[index][row] = found->get<double>();
            } else {
                return failure("score: row " + std::to_string(row) + " has a "
                               "non-numeric '" + features[index] + "'");
            }
        }
    }

    std::vector<std::int64_t> keys = entities;
    if (keys.empty()) {
        keys.reserve(rows.size());
        for (std::size_t row = 0; row < rows.size(); ++row) {
            keys.push_back(static_cast<std::int64_t>(row));
        }
    }

    TableScoreResult result;
    result.entity_keys = std::move(entity_keys);
    if (!result.entity_keys) {
        // Only an inline mapping (a record from before #1255) can be read here; a
        // reference to the entity-key table is resolved by the caller, which
        // holds the node.
        if (const auto mapping = model.feature_schema().find("entity_key_mapping");
            mapping != model.feature_schema().end()) {
            result.entity_keys = data::load_entity_key_mapping(nullptr, *mapping);
        }
    }
    result.feature_columns = features;
    result.drivers = ranked_drivers(model.value().feature_schema);
    result.rows_scored = rows.size();
    ScoreResult scored = score_gbdt(*gbdt, columns, features, keys, top_k);
    if (!scored.ok()) return failure("score: " + scored.status.message);
    result.ranked = std::move(scored.ranked);
    return result;
}

}  // namespace zetta::ml
