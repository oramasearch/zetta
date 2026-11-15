// SPDX-License-Identifier: MIT

#include "zetta/data/dataset_columns.hpp"

#include <utility>

namespace zetta::data {

std::string_view json_type_family(const nlohmann::json& value) {
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    return "null";
}

std::string merge_type_family(std::string_view left, std::string_view right) {
    if (left == right) return std::string(left);
    if (left == "null") return std::string(right);
    if (right == "null") return std::string(left);
    const auto numeric = [](std::string_view family) {
        return family == "integer" || family == "number";
    };
    if (numeric(left) && numeric(right)) return "number";
    return "mixed";
}

std::string type_family_of_parquet(std::string_view parquet_type) {
    if (parquet_type == "boolean") return "boolean";
    if (parquet_type == "int32" || parquet_type == "int64" || parquet_type == "int96") {
        return "integer";
    }
    if (parquet_type == "float" || parquet_type == "double") return "number";
    if (parquet_type == "byte_array" || parquet_type == "fixed_len_byte_array") {
        return "string";
    }
    // A JSON Lines body's schema already speaks in JSON families.
    if (parquet_type == "integer" || parquet_type == "number" ||
        parquet_type == "string" || parquet_type == "object" ||
        parquet_type == "array" || parquet_type == "mixed" || parquet_type == "null") {
        return std::string(parquet_type);
    }
    return "mixed";
}

namespace {

// A string field of a catalog entry, or `fallback` when it is absent or not a
// string (the entry crossed the replication boundary; never throw on it).
std::string string_field(const nlohmann::json& entry, const char* key,
                         const char* fallback) {
    const auto found = entry.find(key);
    return found != entry.end() && found->is_string() ? found->get<std::string>()
                                                      : std::string(fallback);
}

}  // namespace

void ColumnProfile::add_row(const nlohmann::json& row) {
    ++rows_;
    if (!row.is_object()) return;
    for (auto field = row.begin(); field != row.end(); ++field) {
        Counts& counts = counts_[field.key()];
        counts.typed = true;
        const std::string_view family = json_type_family(field.value());
        if (family == "null") continue;
        ++counts.present;
        counts.type = merge_type_family(counts.type, family);
    }
}

void ColumnProfile::add_partition_stats(const nlohmann::json& stats, std::uint64_t rows) {
    rows_ += rows;
    if (!stats.is_object()) {
        rows_without_counts_ += rows;
        rows_without_types_ += rows;
        return;
    }
    if (const auto column_stats = stats.find("column_stats");
        column_stats != stats.end() && column_stats->is_array()) {
        for (const nlohmann::json& entry : *column_stats) {
            if (!entry.is_object() || !entry.contains("name") ||
                !entry["name"].is_string()) {
                continue;
            }
            Counts& counts = counts_[entry["name"].get<std::string>()];
            counts.typed = true;
            counts.type = merge_type_family(counts.type, string_field(entry, "type", "null"));
            // The stats come from the replicated catalog: a value of another
            // shape is read as no nulls rather than thrown on.
            const auto found = entry.find("nulls");
            const std::uint64_t nulls =
                found != entry.end() && found->is_number_unsigned()
                    ? found->get<std::uint64_t>()
                    : 0;
            counts.present += nulls >= rows ? 0 : rows - nulls;
        }
        return;
    }
    rows_without_counts_ += rows;
    const auto schema = stats.find("schema");
    if (schema == stats.end() || !schema->is_array()) {
        rows_without_types_ += rows;
        // The names alone (the `columns` list): known to exist, type unknown.
        if (const auto names = stats.find("columns");
            names != stats.end() && names->is_array()) {
            for (const nlohmann::json& name : *names) {
                if (name.is_string()) counts_[name.get<std::string>()];
            }
        }
        return;
    }
    for (const nlohmann::json& entry : *schema) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
            continue;
        }
        Counts& counts = counts_[entry["name"].get<std::string>()];
        counts.typed = true;
        counts.type = merge_type_family(
            counts.type, type_family_of_parquet(string_field(entry, "type", "null")));
        const auto optional = entry.find("optional");
        if (optional == entry.end() || !optional->is_boolean() || optional->get<bool>()) {
            counts.nullable = true;
        }
    }
}

std::vector<DatasetColumn> ColumnProfile::columns(const std::vector<std::string>& names) const {
    std::vector<DatasetColumn> out;
    out.reserve(names.size());
    const bool counted = rows_without_counts_ == 0;
    for (const std::string& name : names) {
        DatasetColumn column;
        column.name = name;
        const auto found = counts_.find(name);
        if (found == counts_.end() || !found->second.typed) {
            column.type = rows_without_types_ > 0 ? "unknown" : "null";
        } else {
            column.type = found->second.type;
        }
        const std::uint64_t present = found == counts_.end() ? 0 : found->second.present;
        if (counted) {
            column.nullable = present < rows_;
            column.null_fraction =
                rows_ == 0 ? 0.0
                           : static_cast<double>(rows_ - present) / static_cast<double>(rows_);
        } else {
            column.nullable = found == counts_.end() || found->second.nullable ||
                              rows_without_types_ > 0;
        }
        out.push_back(std::move(column));
    }
    return out;
}

nlohmann::json ColumnProfile::stats(const std::vector<std::string>& names) const {
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& name : names) {
        const auto found = counts_.find(name);
        const std::uint64_t present = found == counts_.end() ? 0 : found->second.present;
        out.push_back({{"name", name},
                       {"type", found == counts_.end() ? std::string("null")
                                                       : found->second.type},
                       {"nulls", rows_ - present}});
    }
    return out;
}

}  // namespace zetta::data
