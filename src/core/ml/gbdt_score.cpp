#include "zetta/ml/gbdt_score.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/model.hpp"

namespace zetta::ml {

using compute::ComputeStatus;

ComputeStatus predict_risk(const GbdtModel& model,
                           const std::vector<std::vector<double>>& columns,
                           const std::vector<std::string>& feature_names,
                           std::vector<double>& risks) {
    risks.clear();
    if (columns.size() != feature_names.size()) {
        return ComputeStatus::invalid_argument(
            "score: columns / feature_names length mismatch");
    }

    // Row count = the (uniform) length of the provided columns.
    const std::size_t rows = columns.empty() ? 0 : columns.front().size();
    for (const std::vector<double>& column : columns) {
        if (column.size() != rows) {
            return ComputeStatus::invalid_argument("score: ragged feature columns");
        }
    }

    // Map each provided column to its name, then resolve the model's feature
    // ordering to those columns (scoring input may carry columns in any order, or
    // extra columns the model does not use).
    std::unordered_map<std::string, std::size_t> column_index;
    for (std::size_t index = 0; index < feature_names.size(); ++index) {
        column_index.emplace(feature_names[index], index);
    }
    std::vector<const std::vector<double>*> model_columns(model.feature_count());
    for (std::size_t feature = 0; feature < model.feature_count(); ++feature) {
        const auto found = column_index.find(model.feature_names[feature]);
        if (found == column_index.end()) {
            return ComputeStatus::invalid_argument(
                "score: no column for model feature '" +
                model.feature_names[feature] + "'");
        }
        model_columns[feature] = &columns[found->second];
    }

    risks.resize(rows);
    std::vector<double> row(model.feature_count());
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t feature = 0; feature < model.feature_count(); ++feature) {
            row[feature] = (*model_columns[feature])[r];
        }
        risks[r] = model.predict_proba(row);
    }
    return ComputeStatus::success();
}

ScoreResult score_gbdt(const GbdtModel& model,
                       const std::vector<std::vector<double>>& columns,
                       const std::vector<std::string>& feature_names,
                       const std::vector<std::int64_t>& entities,
                       std::size_t top_k) {
    ScoreResult out;
    std::vector<double> risks;
    out.status = predict_risk(model, columns, feature_names, risks);
    if (!out.status.ok()) return out;
    if (entities.size() != risks.size()) {
        out.status =
            ComputeStatus::invalid_argument("score: entities / row count mismatch");
        return out;
    }

    out.ranked.reserve(risks.size());
    for (std::size_t r = 0; r < risks.size(); ++r) {
        out.ranked.push_back(EntityScore{.entity = entities[r], .risk = risks[r]});
    }
    // Rank by risk descending; ties broken by entity ascending so the at-risk list
    // is deterministic regardless of input order (the single-node == multi-node
    // guarantee the distributed path relies on).
    std::sort(out.ranked.begin(), out.ranked.end(),
              [](const EntityScore& lhs, const EntityScore& rhs) {
                  if (lhs.risk != rhs.risk) return lhs.risk > rhs.risk;
                  return lhs.entity < rhs.entity;
              });
    if (top_k > 0 && out.ranked.size() > top_k) {
        out.ranked.resize(top_k);
    }
    return out;
}

std::optional<GbdtModel> load_gbdt_model(const Model& model, std::string* error) {
    const auto set_error = [&](const std::string& message) {
        if (error != nullptr) *error = message;
    };
    const std::optional<std::string> body = model.read_weights();
    if (!body.has_value()) {
        set_error("score: model weights body not present locally");
        return std::nullopt;
    }
    const nlohmann::json value =
        nlohmann::json::parse(*body, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        set_error("score: model weights body is not valid JSON");
        return std::nullopt;
    }
    return GbdtModel::from_json(value, error);  // validates at the schema boundary
}

}  // namespace zetta::ml
