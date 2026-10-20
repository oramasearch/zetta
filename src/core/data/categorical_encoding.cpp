#include "zetta/data/categorical_encoding.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "zetta/util/timestamp.hpp"

namespace zetta::data {
namespace {

constexpr std::string_view kUnseen =
    "a level not in `levels`, a null, or an empty string encodes as all zeros";

std::string quoted_list(const std::set<std::string>& values, std::size_t limit) {
    std::string out;
    std::size_t shown = 0;
    for (const std::string& value : values) {
        if (shown == limit) {
            out += ", ...";
            break;
        }
        if (shown != 0) out += ", ";
        out += "'" + value + "'";
        ++shown;
    }
    return out;
}

}  // namespace

std::string CategoricalEncoding::feature_name(std::string_view level) const {
    return column + "=" + std::string(level);
}

std::vector<std::string> CategoricalEncoding::feature_names() const {
    std::vector<std::string> names;
    names.reserve(levels.size());
    for (const std::string& level : levels) names.push_back(feature_name(level));
    return names;
}

nlohmann::json CategoricalEncoding::to_json() const {
    return {{"column", column},
            {"encoding", "one_hot"},
            {"levels", levels},
            {"features", feature_names()},
            {"unseen", kUnseen}};
}

std::optional<CategoricalEncoding> categorical_encoding_from_json(
    const nlohmann::json& value, std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<CategoricalEncoding> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("categorical encoding: not a JSON object");
    const auto column = value.find("column");
    if (column == value.end() || !column->is_string() ||
        column->get_ref<const std::string&>().empty()) {
        return fail("categorical encoding: `column` must be a non-empty string");
    }
    if (const auto kind = value.find("encoding");
        kind != value.end() && (!kind->is_string() || *kind != "one_hot")) {
        return fail("categorical encoding: `encoding` must be \"one_hot\"");
    }
    const auto levels = value.find("levels");
    if (levels == value.end() || !levels->is_array() || levels->empty()) {
        return fail("categorical encoding: `levels` must be a non-empty array");
    }
    CategoricalEncoding encoding;
    encoding.column = column->get<std::string>();
    std::set<std::string> seen;
    for (const nlohmann::json& level : *levels) {
        if (!level.is_string() || level.get_ref<const std::string&>().empty()) {
            return fail("categorical encoding: `levels[]` must be non-empty strings");
        }
        if (!seen.insert(level.get<std::string>()).second) {
            return fail("categorical encoding: duplicate level '" + level.get<std::string>() +
                        "'");
        }
        encoding.levels.push_back(level.get<std::string>());
    }
    if (encoding.levels.size() > kCategoricalMaxLevels) {
        return fail("categorical encoding: more than " +
                    std::to_string(kCategoricalMaxLevels) + " levels");
    }
    return encoding;
}

std::vector<CategoricalEncoding> categorical_encodings_from_json(
    const nlohmann::json& value, std::string* error) {
    std::vector<CategoricalEncoding> encodings;
    if (!value.is_array()) {
        if (error != nullptr) *error = "categorical encodings: not a JSON array";
        return encodings;
    }
    std::set<std::string> columns;
    for (const nlohmann::json& item : value) {
        std::string reason;
        std::optional<CategoricalEncoding> encoding =
            categorical_encoding_from_json(item, &reason);
        if (!encoding) {
            if (error != nullptr) *error = reason;
            return {};
        }
        if (!columns.insert(encoding->column).second) {
            if (error != nullptr) {
                *error = "categorical encodings: column '" + encoding->column +
                         "' is encoded twice";
            }
            return {};
        }
        encodings.push_back(std::move(*encoding));
    }
    return encodings;
}

nlohmann::json to_json(const std::vector<CategoricalEncoding>& encodings) {
    nlohmann::json out = nlohmann::json::array();
    for (const CategoricalEncoding& encoding : encodings) out.push_back(encoding.to_json());
    return out;
}

const nlohmann::json& categorical_encoding_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["required"] = {"column", "levels"};
        document["additionalProperties"] = false;
        document["description"] =
            "a text column one-hot encoded for a numeric trainer: one 0/1 feature per "
            "level, named <column>=<level>, fitted on the training rows (#1203)";
        document["properties"]["column"] = {{"type", "string"}, {"minLength", 1}};
        document["properties"]["encoding"] = {{"type", "string"}, {"enum", {"one_hot"}}};
        document["properties"]["levels"] = {
            {"type", "array"},
            {"minItems", 1},
            {"maxItems", kCategoricalMaxLevels},
            {"uniqueItems", true},
            {"items", {{"type", "string"}, {"minLength", 1}}}};
        document["properties"]["features"] = {
            {"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "the encoded feature names, one per level"}};
        document["properties"]["unseen"] = {
            {"type", "string"},
            {"description", "how a value outside the fitted levels is encoded"}};
        return document;
    }();
    return schema;
}

CategoricalFit fit_categorical_encoding(const std::string& column,
                                        const std::vector<nlohmann::json>& rows) {
    CategoricalFit fit;
    std::set<std::string> distinct;
    std::size_t values = 0;
    bool all_times = true;
    for (const nlohmann::json& row : rows) {
        if (!row.is_object()) continue;
        const auto found = row.find(column);
        if (found == row.end() || found->is_null()) continue;
        if (!found->is_string()) {
            fit.reason = "mixes text with non-text values";
            return fit;
        }
        const std::string& text = found->get_ref<const std::string&>();
        if (text.empty()) continue;
        ++values;
        distinct.insert(text);
        if (all_times && !zetta::util::parse_time_seconds(*found)) all_times = false;
    }
    if (values == 0) {
        fit.reason = "no text values";
        return fit;
    }
    if (all_times) {
        fit.reason =
            "text dates or timestamps are not categories; derive a numeric age (for "
            "example days before the cutoff) in a clean step to use it";
        return fit;
    }
    if (distinct.size() == 1) {
        fit.reason = "a single value (" + quoted_list(distinct, 1) +
                     ") carries no signal";
        return fit;
    }
    if (distinct.size() > kCategoricalMaxLevels) {
        fit.reason = "text with " + std::to_string(distinct.size()) +
                     " distinct values, above the " +
                     std::to_string(kCategoricalMaxLevels) +
                     "-level cap for one-hot encoding: an identifier or free text, not a "
                     "category";
        return fit;
    }
    if (distinct.size() * 2 > values) {
        fit.reason = std::to_string(distinct.size()) + " of " + std::to_string(values) +
                     " values are distinct: the column identifies rows rather than "
                     "grouping them";
        return fit;
    }
    CategoricalEncoding encoding;
    encoding.column = column;
    encoding.levels.assign(distinct.begin(), distinct.end());
    fit.encoding = std::move(encoding);
    return fit;
}

double categorical_indicator(const nlohmann::json& value, std::string_view level) {
    return value.is_string() && value.get_ref<const std::string&>() == level ? 1.0 : 0.0;
}

std::string encode_categorical_row(const std::vector<CategoricalEncoding>& encodings,
                                   nlohmann::json& row) {
    if (!row.is_object()) return "the row is not an object";
    for (const CategoricalEncoding& encoding : encodings) {
        const auto found = row.find(encoding.column);
        if (found == row.end()) {
            const std::vector<std::string> names = encoding.feature_names();
            const bool pre_encoded = std::ranges::all_of(
                names, [&row](const std::string& name) { return row.contains(name); });
            if (pre_encoded) continue;
            return "the row has no '" + encoding.column + "' (a text column the model " +
                   "one-hot encodes; send its original value)";
        }
        if (!found->is_null() && !found->is_string()) {
            return "'" + encoding.column + "' must be text (one of the model's levels: " +
                   quoted_list(std::set<std::string>(encoding.levels.begin(),
                                                     encoding.levels.end()),
                               kCategoricalMaxLevels) +
                   ")";
        }
        const nlohmann::json value = *found;
        for (const std::string& level : encoding.levels) {
            row[encoding.feature_name(level)] = categorical_indicator(value, level);
        }
    }
    return {};
}

std::optional<CategoricalFeature> find_categorical_feature(
    const std::vector<CategoricalEncoding>& encodings, std::string_view feature) {
    for (const CategoricalEncoding& encoding : encodings) {
        for (const std::string& level : encoding.levels) {
            if (encoding.feature_name(level) == feature) {
                return CategoricalFeature{&encoding, level};
            }
        }
    }
    return std::nullopt;
}

}  // namespace zetta::data
