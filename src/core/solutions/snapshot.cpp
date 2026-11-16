// SPDX-License-Identifier: MIT

#include "zetta/solutions/snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/ast.hpp"
#include "zetta/query/distributed_select.hpp"
#include "zetta/storage/engine.hpp"

namespace zetta::solutions {
namespace {

using nlohmann::json;

void set_error(std::string* error, std::string reason) {
    if (error != nullptr) {
        *error = std::move(reason);
    }
}

bool read_decimal(std::string_view text, std::size_t offset,
                  std::size_t length, int& value) {
    if (offset + length > text.size()) {
        return false;
    }
    int parsed = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
        const char character = text[index];
        if (character < '0' || character > '9') {
            return false;
        }
        parsed = parsed * 10 + (character - '0');
    }
    value = parsed;
    return true;
}

std::optional<TimePoint> parse_iso8601(std::string_view text,
                                       std::string* error) {
    if (text.size() != 10 && text.size() < 19) {
        set_error(error, "time must be YYYY-MM-DD or an ISO-8601 UTC date-time");
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-') {
        set_error(error, "time must use the ISO-8601 YYYY-MM-DD form");
        return std::nullopt;
    }

    int year_value = 0;
    int month_value = 0;
    int day_value = 0;
    if (!read_decimal(text, 0, 4, year_value) ||
        !read_decimal(text, 5, 2, month_value) ||
        !read_decimal(text, 8, 2, day_value)) {
        set_error(error, "time contains a non-numeric date component");
        return std::nullopt;
    }

    const std::chrono::year_month_day date{
        std::chrono::year{year_value},
        std::chrono::month{static_cast<unsigned>(month_value)},
        std::chrono::day{static_cast<unsigned>(day_value)}};
    if (!date.ok()) {
        set_error(error, "time contains an invalid calendar date");
        return std::nullopt;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    double fractional = 0.0;
    if (text.size() > 10) {
        if (text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
            !read_decimal(text, 11, 2, hour) ||
            !read_decimal(text, 14, 2, minute) ||
            !read_decimal(text, 17, 2, second)) {
            set_error(error, "time must use the ISO-8601 HH:MM:SS form");
            return std::nullopt;
        }
        if (hour > 23 || minute > 59 || second > 59) {
            set_error(error, "time contains an invalid clock value");
            return std::nullopt;
        }

        std::size_t suffix = 19;
        if (suffix < text.size() && text[suffix] == '.') {
            ++suffix;
            const std::size_t fraction_start = suffix;
            double scale = 0.1;
            while (suffix < text.size() && text[suffix] >= '0' &&
                   text[suffix] <= '9') {
                fractional += static_cast<double>(text[suffix] - '0') * scale;
                scale *= 0.1;
                ++suffix;
            }
            if (suffix == fraction_start) {
                set_error(error, "time has an empty fractional-second component");
                return std::nullopt;
            }
        }
        if (suffix < text.size() && text[suffix] == 'Z') {
            ++suffix;
        }
        if (suffix != text.size()) {
            set_error(error, "time must be UTC and may only end with Z");
            return std::nullopt;
        }
    }

    const auto days = std::chrono::sys_days{date}.time_since_epoch().count();
    const double seconds = static_cast<double>(days) * 86400.0 +
                           static_cast<double>(hour * 3600 + minute * 60 + second) +
                           fractional;
    return TimePoint{TimePointKind::Iso8601, seconds};
}

std::string scalar_key(const json& value) {
    if (value.is_string()) {
        return "s:" + value.dump();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "b:1" : "b:0";
    }
    if (value.is_number_unsigned()) {
        return "n:" + value.dump();
    }
    if (value.is_number_integer()) {
        return "n:" + value.dump();
    }
    if (value.is_number_float()) {
        const double numeric = value.get<double>();
        if (!std::isfinite(numeric)) {
            return {};
        }
        std::ostringstream out;
        out << std::setprecision(17) << numeric;
        return "f:" + out.str();
    }
    return {};
}

std::string snapshot_key(const json& entity, const TimePoint& as_of) {
    std::ostringstream out;
    out << scalar_key(entity) << '|'
        << (as_of.kind == TimePointKind::Iso8601 ? "i:" : "n:")
        << std::setprecision(17) << as_of.value;
    return out.str();
}

bool require_string(const json& value, const char* name, std::string& output,
                    std::string* error) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_string() ||
        found->get_ref<const std::string&>().empty()) {
        set_error(error, std::string("/") + name + ": expected a non-empty string");
        return false;
    }
    output = found->get<std::string>();
    return true;
}

std::optional<std::vector<json>> table_rows(Node& node,
                                            const data::Table& table,
                                            std::string* error) {
    if (table.partition_only()) {
        query::SelectStmt statement;
        statement.all = true;
        const query::SelectOutcome outcome = query::select_over_partitions(
            node, node.distributed_select(), statement, table.id());
        if (!outcome.ok()) {
            set_error(error, outcome.error);
            return std::nullopt;
        }
        return outcome.rows;
    }

    std::vector<json> rows;
    rows.reserve(table.store().size());
    for (std::size_t index = 0; index < table.store().size(); ++index) {
        const auto row = table.store().at(index);
        if (!row) {
            set_error(error, "could not read row " + std::to_string(index) +
                                 " from table '" + table.name() + "': " +
                                 row.error());
            return std::nullopt;
        }
        rows.push_back(*row);
    }
    return rows;
}

struct SelectedRow {
    json entity;
    json source_time;
    TimePoint parsed_time;
    json row;
};

struct ForwardObservation {
    TimePoint time;
    bool positive = false;
    json row;
};

std::optional<bool> binary_label(const json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (!value.is_number()) {
        return std::nullopt;
    }
    const double numeric = value.get<double>();
    if (!std::isfinite(numeric)) {
        return std::nullopt;
    }
    return numeric > 0.5;
}

std::optional<std::vector<json>> select_snapshot_rows(
    const std::vector<json>& source_rows, const SnapshotSpec& spec,
    const TimePoint& cutoff, SnapshotResult& result) {
    std::map<std::string, SelectedRow> selected;
    for (std::size_t index = 0; index < source_rows.size(); ++index) {
        const json& row = source_rows[index];
        if (!row.is_object()) {
            result.error = "source row " + std::to_string(index) +
                           " is not an object";
            return std::nullopt;
        }
        const auto entity = row.find(spec.entity_column);
        if (entity == row.end() || scalar_key(*entity).empty()) {
            result.error = "source row " + std::to_string(index) +
                           " has no scalar entity column '" +
                           spec.entity_column + "'";
            return std::nullopt;
        }
        const auto observed = row.find(spec.time_column);
        if (observed == row.end()) {
            result.error = "source row " + std::to_string(index) +
                           " has no time column '" + spec.time_column + "'";
            return std::nullopt;
        }
        std::string time_error;
        const std::optional<TimePoint> observed_time =
            parse_time_point(*observed, &time_error);
        if (!observed_time) {
            result.error = "source row " + std::to_string(index) +
                           ", column '" + spec.time_column + "': " + time_error;
            return std::nullopt;
        }
        if (observed_time->kind != cutoff.kind) {
            result.error = "source row " + std::to_string(index) +
                           " uses a different time representation than as_of";
            return std::nullopt;
        }
        if (observed_time->value > cutoff.value) {
            ++result.future_rows_excluded;
            continue;
        }

        const std::string key = scalar_key(*entity);
        SelectedRow candidate{*entity, *observed, *observed_time, row};
        const auto current = selected.find(key);
        if (current == selected.end() ||
            candidate.parsed_time.value > current->second.parsed_time.value ||
            (candidate.parsed_time.value == current->second.parsed_time.value &&
             candidate.row.dump() > current->second.row.dump())) {
            selected[key] = std::move(candidate);
        }
    }

    std::vector<json> materialized;
    materialized.reserve(selected.size());
    const std::string source_name = spec.database + "." + spec.source_table;
    for (const auto& [key, selected_row] : selected) {
        (void)key;
        json row = selected_row.row;
        row["entity_id"] = selected_row.entity;
        row["as_of"] = spec.as_of;
        row["_source_as_of"] = selected_row.source_time;
        row["_source_table"] = source_name;
        materialized.push_back(std::move(row));
    }
    return materialized;
}

std::optional<std::map<std::string, json>> read_existing_snapshots(
    Node& node, const data::Table& target, SnapshotResult& result) {
    std::string read_error;
    const std::optional<std::vector<json>> rows =
        table_rows(node, target, &read_error);
    if (!rows) {
        result.error = std::move(read_error);
        return std::nullopt;
    }

    std::map<std::string, json> existing;
    for (std::size_t index = 0; index < rows->size(); ++index) {
        const json& row = rows->at(index);
        if (!row.is_object() || !row.contains("entity_id") ||
            !row.contains("as_of")) {
            result.error = "target row " + std::to_string(index) +
                           " is not a snapshot row keyed by entity_id and as_of";
            return std::nullopt;
        }
        const std::optional<TimePoint> row_time =
            parse_time_point(row.at("as_of"));
        if (!row_time || scalar_key(row.at("entity_id")).empty()) {
            result.error = "target row " + std::to_string(index) +
                           " has an invalid snapshot key";
            return std::nullopt;
        }
        const std::string key = snapshot_key(row.at("entity_id"), *row_time);
        if (!existing.emplace(key, row).second) {
            result.error = "target table contains a duplicate snapshot key";
            return std::nullopt;
        }
    }
    return existing;
}

std::optional<std::vector<json>> new_snapshot_rows(
    std::vector<json> materialized,
    const std::map<std::string, json>& existing, const TimePoint& cutoff,
    SnapshotResult& result) {
    std::vector<json> additions;
    additions.reserve(materialized.size());
    for (json& row : materialized) {
        const std::string key = snapshot_key(row.at("entity_id"), cutoff);
        const auto found = existing.find(key);
        if (found == existing.end()) {
            additions.push_back(std::move(row));
            continue;
        }
        if (found->second != row) {
            result.error =
                "snapshot key already exists with different data for entity " +
                row.at("entity_id").dump();
            return std::nullopt;
        }
        ++result.existing_rows;
    }
    return additions;
}

}  // namespace

std::optional<TimePoint> parse_time_point(const json& value,
                                          std::string* error) {
    if (value.is_number()) {
        const double numeric = value.get<double>();
        if (!std::isfinite(numeric)) {
            set_error(error, "time must be finite");
            return std::nullopt;
        }
        return TimePoint{TimePointKind::Numeric, numeric};
    }
    if (value.is_string()) {
        return parse_iso8601(value.get_ref<const std::string&>(), error);
    }
    set_error(error, "time must be a number or ISO-8601 string");
    return std::nullopt;
}

const json& snapshot_spec_schema() {
    static const json schema = json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/snapshot_spec.schema.json",
  "title": "SnapshotSpec",
  "description": "Materialize the latest source observation per entity at or before a point-in-time cutoff (#1103).",
  "type": "object",
  "additionalProperties": false,
  "required": ["database", "source_table", "target_table", "entity_column", "time_column", "as_of"],
  "properties": {
    "database": { "type": "string", "minLength": 1 },
    "source_table": { "type": "string", "minLength": 1 },
    "target_table": { "type": "string", "minLength": 1 },
    "entity_column": { "type": "string", "minLength": 1 },
    "time_column": { "type": "string", "minLength": 1 },
    "as_of": {
      "description": "Inclusive cutoff, as a finite number or ISO-8601 UTC date/date-time.",
      "oneOf": [
        { "type": "number" },
        { "type": "string", "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$" }
      ]
    }
  }
}
)JSONSCHEMA");
    return schema;
}

json SnapshotSpec::to_json() const {
    return {{"database", database},
            {"source_table", source_table},
            {"target_table", target_table},
            {"entity_column", entity_column},
            {"time_column", time_column},
            {"as_of", as_of}};
}

std::optional<SnapshotSpec> SnapshotSpec::from_json(const json& value,
                                                    std::string* error) {
    if (!value.is_object()) {
        set_error(error, "/: expected an object");
        return std::nullopt;
    }
    static constexpr std::string_view fields[] = {
        "database", "source_table", "target_table", "entity_column",
        "time_column", "as_of"};
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::find(std::begin(fields), std::end(fields), field.key()) ==
            std::end(fields)) {
            set_error(error, "/" + field.key() + ": unknown property");
            return std::nullopt;
        }
    }

    SnapshotSpec spec;
    if (!require_string(value, "database", spec.database, error) ||
        !require_string(value, "source_table", spec.source_table, error) ||
        !require_string(value, "target_table", spec.target_table, error) ||
        !require_string(value, "entity_column", spec.entity_column, error) ||
        !require_string(value, "time_column", spec.time_column, error)) {
        return std::nullopt;
    }
    const auto cutoff = value.find("as_of");
    if (cutoff == value.end()) {
        set_error(error, "/as_of: required property is missing");
        return std::nullopt;
    }
    std::string time_error;
    if (!parse_time_point(*cutoff, &time_error)) {
        set_error(error, "/as_of: " + time_error);
        return std::nullopt;
    }
    if (spec.source_table == spec.target_table) {
        set_error(error, "/target_table: must differ from source_table");
        return std::nullopt;
    }
    spec.as_of = *cutoff;
    return spec;
}

SnapshotResult materialize_snapshot(Node& node, const SnapshotSpec& spec) {
    SnapshotResult result;
    std::string validation_error;
    const std::optional<SnapshotSpec> parsed =
        SnapshotSpec::from_json(spec.to_json(), &validation_error);
    if (!parsed) {
        result.error = std::move(validation_error);
        return result;
    }
    const std::optional<TimePoint> cutoff = parse_time_point(spec.as_of);
    if (!cutoff) {
        result.error = "/as_of: invalid cutoff";
        return result;
    }

    const std::shared_ptr<data::Database> database =
        node.find_database(spec.database);
    if (!database) {
        result.error = "database '" + spec.database + "' does not exist";
        return result;
    }
    const std::shared_ptr<data::Table> source =
        database->find_table(spec.source_table);
    if (!source) {
        result.error = "source table '" + spec.database + "." +
                       spec.source_table + "' does not exist";
        return result;
    }

    std::string read_error;
    const std::optional<std::vector<json>> source_rows =
        table_rows(node, *source, &read_error);
    if (!source_rows) {
        result.error = std::move(read_error);
        return result;
    }
    result.source_rows = source_rows->size();
    std::optional<std::vector<json>> materialized =
        select_snapshot_rows(*source_rows, spec, *cutoff, result);
    if (!materialized) {
        return result;
    }

    std::shared_ptr<data::Table> target =
        database->find_table(spec.target_table);
    if (target && target->partition_only()) {
        result.error = "target table '" + spec.database + "." +
                       spec.target_table + "' is partition-only";
        return result;
    }

    std::map<std::string, json> existing;
    if (target) {
        std::optional<std::map<std::string, json>> read_existing =
            read_existing_snapshots(node, *target, result);
        if (!read_existing) {
            return result;
        }
        existing = std::move(*read_existing);
    }

    std::optional<std::vector<json>> additions = new_snapshot_rows(
        std::move(*materialized), existing, *cutoff, result);
    if (!additions) {
        return result;
    }

    if (!target) {
        target = database->create_table(spec.target_table);
        if (!target) {
            result.error = "could not create target table '" + spec.database +
                           "." + spec.target_table + "'";
            return result;
        }
    }
    for (json& row : *additions) {
        target->add_row(std::move(row));
    }
    result.table = std::move(target);
    result.materialized_rows = additions->size();
    return result;
}

ForwardLabelResult build_forward_labels(Node& node,
                                        const SnapshotSpec& snapshot,
                                        const ForwardLabelSpec& spec) {
    ForwardLabelResult result;
    if (spec.label_column.empty()) {
        result.error = "forward labels require a label column";
        return result;
    }
    if (spec.feature_columns.empty()) {
        result.error = "forward labels require at least one feature column";
        return result;
    }
    if (!std::isfinite(spec.blackout_gap_seconds) ||
        spec.blackout_gap_seconds < 0.0 ||
        !std::isfinite(spec.window_seconds) || spec.window_seconds <= 0.0) {
        result.error =
            "forward labels require a non-negative blackout and positive window";
        return result;
    }
    if (spec.rolling_windows < 2) {
        result.error = "forward labels require at least two rolling windows";
        return result;
    }

    const std::optional<TimePoint> prediction =
        parse_time_point(snapshot.as_of);
    if (!prediction) {
        result.error = "forward labels received an invalid prediction cutoff";
        return result;
    }
    const std::shared_ptr<data::Database> database =
        node.find_database(snapshot.database);
    if (!database) {
        result.error = "database '" + snapshot.database + "' does not exist";
        return result;
    }
    const std::shared_ptr<data::Table> source =
        database->find_table(snapshot.source_table);
    if (!source) {
        result.error = "source table '" + snapshot.database + "." +
                       snapshot.source_table + "' does not exist";
        return result;
    }
    std::string read_error;
    const std::optional<std::vector<json>> source_rows =
        table_rows(node, *source, &read_error);
    if (!source_rows) {
        result.error = std::move(read_error);
        return result;
    }
    if (source_rows->empty()) {
        result.error = "forward label source is empty";
        return result;
    }

    std::map<std::string, std::vector<ForwardObservation>> observations;
    std::set<double> candidate_cutoffs;
    candidate_cutoffs.insert(prediction->value);
    result.data_last_time = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < source_rows->size(); ++index) {
        const json& row = source_rows->at(index);
        if (!row.is_object()) {
            result.error = "forward label source row " +
                           std::to_string(index) + " is not an object";
            return result;
        }
        const auto entity = row.find(snapshot.entity_column);
        const std::string entity_key =
            entity == row.end() ? std::string{} : scalar_key(*entity);
        if (entity_key.empty()) {
            result.error = "forward label source row " +
                           std::to_string(index) +
                           " has no scalar entity column '" +
                           snapshot.entity_column + "'";
            return result;
        }
        const auto time_value = row.find(snapshot.time_column);
        if (time_value == row.end()) {
            result.error = "forward label source row " +
                           std::to_string(index) + " has no time column '" +
                           snapshot.time_column + "'";
            return result;
        }
        std::string time_error;
        const std::optional<TimePoint> time =
            parse_time_point(*time_value, &time_error);
        if (!time || time->kind != prediction->kind) {
            result.error = "forward label source row " +
                           std::to_string(index) +
                           " has an incompatible time: " + time_error;
            return result;
        }
        const auto label_value = row.find(spec.label_column);
        const std::optional<bool> positive =
            label_value == row.end() ? std::nullopt
                                     : binary_label(*label_value);
        if (!positive) {
            result.error = "forward label source row " +
                           std::to_string(index) +
                           " has no finite boolean or numeric label column '" +
                           spec.label_column + "'";
            return result;
        }
        for (const std::string& feature : spec.feature_columns) {
            if (!row.contains(feature)) {
                result.error = "forward label source row " +
                               std::to_string(index) +
                               " has no feature column '" + feature + "'";
                return result;
            }
        }
        observations[entity_key].push_back(
            ForwardObservation{*time, *positive, row});
        result.data_last_time =
            std::max(result.data_last_time, time->value);
        if (time->value <= prediction->value) {
            candidate_cutoffs.insert(time->value);
        }
    }
    for (auto& [entity, rows] : observations) {
        (void)entity;
        std::ranges::stable_sort(
            rows, [](const ForwardObservation& left,
                     const ForwardObservation& right) {
                if (left.time.value != right.time.value) {
                    return left.time.value < right.time.value;
                }
                return left.row.dump() < right.row.dump();
            });
    }

    result.declared_window_end = prediction->value +
                                 spec.blackout_gap_seconds +
                                 spec.window_seconds;
    result.declared_window_fully_observed =
        result.data_last_time >= result.declared_window_end;

    std::vector<double> eligible_cutoffs;
    for (const double cutoff : candidate_cutoffs) {
        const double window_end = cutoff + spec.blackout_gap_seconds +
                                  spec.window_seconds;
        if (cutoff <= prediction->value &&
            window_end <= result.data_last_time) {
            eligible_cutoffs.push_back(cutoff);
        }
    }
    if (eligible_cutoffs.size() > spec.rolling_windows) {
        eligible_cutoffs.erase(
            eligible_cutoffs.begin(),
            eligible_cutoffs.end() -
                static_cast<std::ptrdiff_t>(spec.rolling_windows));
    }

    std::int64_t sample_id = 1;
    for (const double cutoff : eligible_cutoffs) {
        ForwardLabelWindow window;
        window.cutoff = cutoff;
        const double label_start = cutoff + spec.blackout_gap_seconds;
        const double label_end = label_start + spec.window_seconds;
        for (const auto& [entity, rows] : observations) {
            (void)entity;
            const auto feature_end = std::upper_bound(
                rows.begin(), rows.end(), cutoff,
                [](double value, const ForwardObservation& observation) {
                    return value < observation.time.value;
                });
            if (feature_end == rows.begin()) {
                continue;
            }
            const ForwardObservation& feature_row = *std::prev(feature_end);
            if (feature_row.positive) {
                ++window.inactive;
                ++result.inactive;
                continue;
            }
            const bool positive = std::ranges::any_of(
                rows, [label_start, label_end](const ForwardObservation& row) {
                    return row.time.value > label_start &&
                           row.time.value <= label_end && row.positive;
                });
            if (!positive && rows.back().time.value < label_end) {
                ++window.censored;
                ++result.censored;
                continue;
            }

            json training_row = feature_row.row;
            training_row["_s1_sample_id"] = sample_id;
            training_row["_s1_cutoff"] = cutoff;
            training_row[spec.label_column] = positive ? 1 : 0;
            result.rows.push_back(std::move(training_row));
            ++sample_id;
            ++window.rows;
            if (positive) {
                ++window.positives;
            } else {
                ++window.negatives;
            }
        }
        if (window.has_both_classes()) {
            ++result.windows_with_both_classes;
        }
        result.windows.push_back(window);
    }

    if (!result.windows.empty()) {
        const ForwardLabelWindow& validation = result.windows.back();
        result.validation_rows = validation.rows;
        result.validation_positives = validation.positives;
        result.validation_negatives = validation.negatives;
        for (auto window = result.windows.begin();
             window != std::prev(result.windows.end()); ++window) {
            result.training_rows += window->rows;
            result.training_positives += window->positives;
            result.training_negatives += window->negatives;
        }
    }
    return result;
}

const json& as_of_join_spec_schema() {
    static const json schema = json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/as_of_join_spec.schema.json",
  "title": "AsOfJoinSpec",
  "description": "Match each left row to the latest snapshot for its entity at or before the left row's decision time (#1103).",
  "type": "object",
  "additionalProperties": false,
  "required": ["left_entity_column", "left_time_column"],
  "properties": {
    "left_entity_column": { "type": "string", "minLength": 1 },
    "left_time_column": { "type": "string", "minLength": 1 },
    "right_entity_column": { "type": "string", "minLength": 1, "default": "entity_id" },
    "right_time_column": { "type": "string", "minLength": 1, "default": "as_of" },
    "right_prefix": { "type": "string", "default": "snapshot_" }
  }
}
)JSONSCHEMA");
    return schema;
}

json AsOfJoinSpec::to_json() const {
    return {{"left_entity_column", left_entity_column},
            {"left_time_column", left_time_column},
            {"right_entity_column", right_entity_column},
            {"right_time_column", right_time_column},
            {"right_prefix", right_prefix}};
}

std::optional<AsOfJoinSpec> AsOfJoinSpec::from_json(const json& value,
                                                    std::string* error) {
    if (!value.is_object()) {
        set_error(error, "/: expected an object");
        return std::nullopt;
    }
    static constexpr std::string_view fields[] = {
        "left_entity_column", "left_time_column", "right_entity_column",
        "right_time_column", "right_prefix"};
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::find(std::begin(fields), std::end(fields), field.key()) ==
            std::end(fields)) {
            set_error(error, "/" + field.key() + ": unknown property");
            return std::nullopt;
        }
    }

    AsOfJoinSpec spec;
    if (!require_string(value, "left_entity_column", spec.left_entity_column,
                        error) ||
        !require_string(value, "left_time_column", spec.left_time_column,
                        error)) {
        return std::nullopt;
    }
    if (value.contains("right_entity_column") &&
        !require_string(value, "right_entity_column", spec.right_entity_column,
                        error)) {
        return std::nullopt;
    }
    if (value.contains("right_time_column") &&
        !require_string(value, "right_time_column", spec.right_time_column,
                        error)) {
        return std::nullopt;
    }
    if (value.contains("right_prefix")) {
        if (!value.at("right_prefix").is_string()) {
            set_error(error, "/right_prefix: expected a string");
            return std::nullopt;
        }
        spec.right_prefix = value.at("right_prefix").get<std::string>();
    }
    return spec;
}

AsOfJoinResult as_of_join(const std::vector<json>& left_rows,
                          const std::vector<json>& right_rows,
                          const AsOfJoinSpec& spec) {
    AsOfJoinResult result;
    std::string validation_error;
    if (!AsOfJoinSpec::from_json(spec.to_json(), &validation_error)) {
        result.error = std::move(validation_error);
        return result;
    }

    struct IndexedRight {
        TimePoint time;
        const json* row = nullptr;
    };
    std::map<std::string, std::vector<IndexedRight>> by_entity;
    std::map<std::string, json> keys;
    for (std::size_t index = 0; index < right_rows.size(); ++index) {
        const json& row = right_rows[index];
        if (!row.is_object() || !row.contains(spec.right_entity_column) ||
            !row.contains(spec.right_time_column)) {
            result.error = "right row " + std::to_string(index) +
                           " is missing its entity or time column";
            return result;
        }
        const std::string entity = scalar_key(row.at(spec.right_entity_column));
        const std::optional<TimePoint> time =
            parse_time_point(row.at(spec.right_time_column));
        if (entity.empty() || !time) {
            result.error = "right row " + std::to_string(index) +
                           " has an invalid entity or time value";
            return result;
        }
        const std::string key =
            snapshot_key(row.at(spec.right_entity_column), *time);
        if (!keys.emplace(key, row).second) {
            result.error = "right rows contain a duplicate entity/time key";
            return result;
        }
        by_entity[entity].push_back(IndexedRight{*time, &row});
    }
    for (auto& [entity, rows] : by_entity) {
        (void)entity;
        std::sort(rows.begin(), rows.end(), [](const IndexedRight& left,
                                               const IndexedRight& right) {
            return left.time.value < right.time.value;
        });
    }

    result.rows.reserve(left_rows.size());
    for (std::size_t index = 0; index < left_rows.size(); ++index) {
        const json& left = left_rows[index];
        if (!left.is_object() || !left.contains(spec.left_entity_column) ||
            !left.contains(spec.left_time_column)) {
            result.error = "left row " + std::to_string(index) +
                           " is missing its entity or decision-time column";
            result.rows.clear();
            return result;
        }
        const std::string entity = scalar_key(left.at(spec.left_entity_column));
        const std::optional<TimePoint> decision_time =
            parse_time_point(left.at(spec.left_time_column));
        if (entity.empty() || !decision_time) {
            result.error = "left row " + std::to_string(index) +
                           " has an invalid entity or decision time";
            result.rows.clear();
            return result;
        }

        json joined = left;
        const auto entity_rows = by_entity.find(entity);
        const json* match = nullptr;
        if (entity_rows != by_entity.end()) {
            for (const IndexedRight& candidate : entity_rows->second) {
                if (candidate.time.kind != decision_time->kind) {
                    result.error =
                        "left and right rows use different time representations";
                    result.rows.clear();
                    return result;
                }
                if (candidate.time.value <= decision_time->value) {
                    match = candidate.row;
                } else {
                    ++result.future_rows_excluded;
                }
            }
        }

        if (match == nullptr) {
            ++result.unmatched;
            result.rows.push_back(std::move(joined));
            continue;
        }
        ++result.matched;
        joined[spec.right_prefix + "entity_id"] =
            match->at(spec.right_entity_column);
        joined[spec.right_prefix + "as_of"] = match->at(spec.right_time_column);
        for (const auto& [name, value] : match->items()) {
            if (name == spec.right_entity_column ||
                name == spec.right_time_column) {
                continue;
            }
            joined[spec.right_prefix + name] = value;
        }
        result.rows.push_back(std::move(joined));
    }
    return result;
}

}  // namespace zetta::solutions
