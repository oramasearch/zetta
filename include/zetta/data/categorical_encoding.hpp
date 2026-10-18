#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// **Categorical features for numeric trainers** (#1203, item 2 of #957).
//
// A churn table landed from a CSV carries `plan` and `region` as text, and those are
// the first features most people expect a churn model to use. The GBDT reads numeric
// columns only, so a low-cardinality text column is one-hot encoded: one 0/1 feature
// per level, named `<column>=<level>`.
//
// The rules, in one place so the contract inference, the feature plan, the trainer's
// frame schema, the model record and every predict path read the same ones:
//
//   * **Which columns.** Every sampled value is non-empty text (nulls skipped), the
//     column has 2 to `kCategoricalMaxLevels` distinct values, at most half of the
//     values are distinct (otherwise the column identifies rows), and the values are
//     not dates or timestamps. Anything else stays excluded, with a reason.
//   * **Fitted on the training rows only.** The level list is read from the rows the
//     model trains on (after the `as_of` cutoff when a time column is given), never
//     from the rows it is later asked to score. One-hot encoding reads no label, so
//     the levels carry no target information into a held-out fold; no target
//     encoding is done.
//   * **Stored with the model.** The fitted encoding is written to the Model's
//     `feature_schema.categorical_encoding`, and predict applies that stored list,
//     never one refitted from the rows it is given.
//   * **Unseen and missing values encode as all zeros.** A level the model never saw,
//     a null, or an empty string sets no indicator. A row that does not carry the
//     column at all is an error, like a missing numeric feature.
namespace zetta::data {

// The most distinct values a text column may have in the training rows to be
// one-hot encoded. Above it the column is treated as an identifier or free text.
inline constexpr std::size_t kCategoricalMaxLevels = 32;

// One text column, one-hot encoded.
struct CategoricalEncoding {
    std::string column;               // the source column, by its original name
    std::vector<std::string> levels;  // the fitted levels, sorted, unique

    // `<column>=<level>`: the feature the trainer reads for one level.
    std::string feature_name(std::string_view level) const;
    // One feature per level, in `levels` order.
    std::vector<std::string> feature_names() const;

    // {column, levels, features, unseen}. `features` and `unseen` are descriptive;
    // the parser recomputes the features from the levels.
    nlohmann::json to_json() const;

    bool operator==(const CategoricalEncoding& other) const = default;
};

std::optional<CategoricalEncoding> categorical_encoding_from_json(
    const nlohmann::json& value, std::string* error = nullptr);

std::vector<CategoricalEncoding> categorical_encodings_from_json(
    const nlohmann::json& value, std::string* error = nullptr);
nlohmann::json to_json(const std::vector<CategoricalEncoding>& encodings);

// The JSON Schema of CategoricalEncoding::to_json().
const nlohmann::json& categorical_encoding_schema();

// The result of fitting a column: an encoding, or the reason it is not a category.
struct CategoricalFit {
    std::optional<CategoricalEncoding> encoding;
    std::string reason;  // set when `encoding` is empty

    bool ok() const noexcept { return encoding.has_value(); }
};

// Fit `column` from `rows` (JSON objects). Rows that do not carry the column, or
// carry null or an empty string, are skipped. Pure and non-throwing.
CategoricalFit fit_categorical_encoding(const std::string& column,
                                        const std::vector<nlohmann::json>& rows);

// The indicator value of `level` for one row's raw value: 1 when the value is that
// text, 0 otherwise (another level, an unseen level, null, or a non-text value).
double categorical_indicator(const nlohmann::json& value, std::string_view level);

// Write every encoding's indicator features into `row`, from the row's original
// column. When the row does not carry the column but already carries every encoded
// feature, the row is left as it is. Returns an empty string, or why the row cannot
// be encoded (it has neither the column nor its encoded features, or the column holds
// a number or an object rather than text).
std::string encode_categorical_row(const std::vector<CategoricalEncoding>& encodings,
                                   nlohmann::json& row);

// The encoding that produces `feature`, and the level it indicates, if any.
struct CategoricalFeature {
    const CategoricalEncoding* encoding = nullptr;
    std::string level;
};
std::optional<CategoricalFeature> find_categorical_feature(
    const std::vector<CategoricalEncoding>& encodings, std::string_view feature);

}  // namespace zetta::data
