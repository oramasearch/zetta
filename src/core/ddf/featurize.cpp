#include "zetta/ddf/featurize.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/as_of.hpp"
#include "zetta/ddf/join.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/storage/json_store.hpp"

namespace zetta::ddf {

namespace {

using compute::ComputeStatus;
using compute::DataType;
using compute::ReduceOp;

// --- column readers (any numeric dtype -> double / int64) --------------------

// Read a numeric column of native type `Native` and widen each element to double.
template <class Native>
ComputeStatus read_native_as_double(const DataFrame& frame, const std::string& name,
                                    std::vector<double>& out) {
    std::vector<Native> native;
    const ComputeStatus status = frame.to_host<Native>(name, native);
    if (!status.ok()) return status;
    out.assign(native.begin(), native.end());
    return ComputeStatus::success();
}

// Read any numeric column of `frame` to host as double (lossless enough for keys /
// values / timestamps; integer columns widen into the double mantissa).
ComputeStatus read_doubles(const DataFrame& frame, const std::string& name,
                           std::vector<double>& out) {
    const Column* column = frame.column(name);
    if (column == nullptr) {
        return ComputeStatus::invalid_argument("featurize: no column named '" + name +
                                               "'");
    }
    switch (column->dtype()) {
        case DataType::Float32: return read_native_as_double<float>(frame, name, out);
        case DataType::Float64: return read_native_as_double<double>(frame, name, out);
        case DataType::Int32:   return read_native_as_double<std::int32_t>(frame, name, out);
        case DataType::Int64:   return read_native_as_double<std::int64_t>(frame, name, out);
        case DataType::UInt32:  return read_native_as_double<std::uint32_t>(frame, name, out);
        case DataType::UInt64:  return read_native_as_double<std::uint64_t>(frame, name, out);
        case DataType::UInt8:   return read_native_as_double<std::uint8_t>(frame, name, out);
    }
    return ComputeStatus::invalid_argument("featurize: unknown column dtype");
}

template <class Native>
ComputeStatus read_native_as_int64(const DataFrame& frame, const std::string& name,
                                   std::vector<std::int64_t>& out) {
    std::vector<Native> native;
    const ComputeStatus status = frame.to_host<Native>(name, native);
    if (!status.ok()) return status;
    out.assign(native.begin(), native.end());
    return ComputeStatus::success();
}

// Read an integer entity-key column to host as int64 — the exact group/join key (no
// float, so equality grouping is exact). Fails if the column is a floating dtype.
ComputeStatus read_int64(const DataFrame& frame, const std::string& name,
                         std::vector<std::int64_t>& out) {
    const Column* column = frame.column(name);
    if (column == nullptr) {
        return ComputeStatus::invalid_argument("featurize: no column named '" + name +
                                               "'");
    }
    switch (column->dtype()) {
        case DataType::Int32:  return read_native_as_int64<std::int32_t>(frame, name, out);
        case DataType::Int64:  return read_native_as_int64<std::int64_t>(frame, name, out);
        case DataType::UInt32: return read_native_as_int64<std::uint32_t>(frame, name, out);
        case DataType::UInt64: return read_native_as_int64<std::uint64_t>(frame, name, out);
        case DataType::UInt8:  return read_native_as_int64<std::uint8_t>(frame, name, out);
        case DataType::Float32:
        case DataType::Float64:
            return ComputeStatus::invalid_argument(
                "featurize: entity column '" + name + "' must be an integer dtype");
    }
    return ComputeStatus::invalid_argument("featurize: unknown entity dtype");
}

// --- per-feature computation -------------------------------------------------

// Map a featurize aggregate to the as-of reduction it runs through (sum/min/max).
ReduceOp as_of_reduce_op(AggregateOp agg) {
    switch (agg) {
        case AggregateOp::Sum: return ReduceOp::Sum;
        case AggregateOp::Min: return ReduceOp::Minimum;
        case AggregateOp::Max: return ReduceOp::Maximum;
        case AggregateOp::Count:
        case AggregateOp::Mean:
            return ReduceOp::Sum;  // unused (count/mean go through group_by)
    }
    return ReduceOp::Sum;
}

// Compute one windowed feature into `out` (entity key -> feature value), over the
// (already type-filtered) `events` frame. sum/min/max run through the as-of op; count
// / mean through a windowed group-by. Every path keeps only `time <= as_of` (and >=
// window_start when set), so the feature can never read a future event.
ComputeStatus compute_windowed_feature(const DataFrame& events,
                                       const std::string& entity_column,
                                       const std::string& time_column, double as_of,
                                       const WindowedFeature& feature,
                                       std::map<std::int64_t, double>& out) {
    out.clear();
    if (feature.agg == AggregateOp::Sum || feature.agg == AggregateOp::Min ||
        feature.agg == AggregateOp::Max) {
        AsOfAggregate spec;
        spec.entity_column = entity_column;
        spec.time_column = time_column;
        spec.value_column = feature.value_column;
        spec.as_of = as_of;
        spec.window_start = feature.window_start;
        spec.op = as_of_reduce_op(feature.agg);
        const AsOfResult result = as_of_aggregate(events, spec);
        if (!result.ok()) return result.status;
        for (std::size_t index = 0; index < result.column.size(); ++index) {
            // count == 0 means no in-window row: a min/max identity (+/-inf) becomes
            // 0, matching the COALESCE(agg, 0) of churn_features.sql.
            out[result.column.entities[index]] =
                result.column.counts[index] > 0 ? result.column.values[index] : 0.0;
        }
        return ComputeStatus::success();
    }

    // count / mean: window the rows with the filter op (time <= as_of, and >=
    // window_start), then a group-by over the surviving rows.
    DataFrame windowed_storage;
    const DataFrame* windowed = &events;
    {
        DataFrameResult upper = filter(
            *windowed, FilterPredicate{.column = time_column,
                                       .comparison = Comparison::LessEqual,
                                       .value = as_of});
        if (!upper.ok()) return upper.status;
        windowed_storage = std::move(upper.frame);
        windowed = &windowed_storage;
    }
    if (feature.window_start.has_value()) {
        DataFrameResult lower = filter(
            *windowed, FilterPredicate{.column = time_column,
                                       .comparison = Comparison::GreaterEqual,
                                       .value = *feature.window_start});
        if (!lower.ok()) return lower.status;
        windowed_storage = std::move(lower.frame);
        windowed = &windowed_storage;
    }

    GroupBySpec spec;
    spec.key = entity_column;
    Aggregate aggregate;
    aggregate.op = feature.agg;
    aggregate.column = feature.value_column;
    aggregate.output = feature.name;
    spec.aggregates = {aggregate};

    const GroupByOutcome outcome = group_by(*windowed, spec);
    if (!outcome.ok()) return outcome.status;
    const std::vector<double>* values = outcome.result.value(feature.name);
    if (values == nullptr) {
        return ComputeStatus::backend_error(
            "featurize: group-by produced no column for feature '" + feature.name +
            "'");
    }
    for (std::size_t group = 0; group < outcome.result.keys.size(); ++group) {
        out[static_cast<std::int64_t>(outcome.result.keys[group])] = (*values)[group];
    }
    return ComputeStatus::success();
}

}  // namespace

// --- WindowedFeature / FeaturizeSpec JSON boundary ---------------------------

nlohmann::json FeaturizeSpec::to_json() const {
    nlohmann::json value;
    value["entity_column"] = entity_column;
    // time_column + as_of are the point-in-time cutoff; a static featurization with no
    // time dimension omits them (a row filter / windowing is "where applicable").
    if (!time_column.empty()) {
        value["time_column"] = time_column;
        value["as_of"] = as_of;
    }
    if (!event_features.empty()) {
        nlohmann::json features = nlohmann::json::array();
        for (const WindowedFeature& feature : event_features) {
            nlohmann::json entry;
            entry["name"] = feature.name;
            entry["agg"] = to_string(feature.agg);
            if (!feature.value_column.empty()) entry["value_column"] = feature.value_column;
            if (feature.window_start.has_value()) entry["window_start"] = *feature.window_start;
            if (!feature.type_column.empty()) entry["type_column"] = feature.type_column;
            if (feature.type_value.has_value()) entry["type_value"] = *feature.type_value;
            features.push_back(std::move(entry));
        }
        value["event_features"] = std::move(features);
    }
    if (!account_features.empty()) value["account_features"] = account_features;
    if (!label_column.empty()) value["label_column"] = label_column;
    return value;
}

std::optional<FeaturizeSpec> FeaturizeSpec::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    for (const auto& [key, _] : value.items()) {
        if (key != "entity_column" && key != "time_column" && key != "as_of" &&
            key != "event_features" && key != "account_features" &&
            key != "label_column") {
            return std::nullopt;  // unknown key
        }
    }

    const auto string_field = [&](const char* key) -> std::optional<std::string> {
        if (!value.contains(key)) return std::nullopt;
        const nlohmann::json& field = value.at(key);
        if (!field.is_string()) return std::nullopt;
        std::string text = field.get<std::string>();
        if (text.empty()) return std::nullopt;
        return text;
    };

    FeaturizeSpec spec;
    if (auto entity = string_field("entity_column")) spec.entity_column = *entity;
    else return std::nullopt;

    // time_column is optional: a static featurization (no windowed event features)
    // over already-engineered columns has no time dimension. When present it must be a
    // non-empty string and carry an as_of cutoff (the point-in-time row filter / the
    // per-feature window both compare against it).
    if (value.contains("time_column")) {
        if (auto time = string_field("time_column")) spec.time_column = *time;
        else return std::nullopt;
    }
    if (value.contains("as_of")) {
        if (!value.at("as_of").is_number()) return std::nullopt;
        spec.as_of = value.at("as_of").get<double>();
    }
    if (!spec.time_column.empty() && !value.contains("as_of")) {
        return std::nullopt;  // a time_column needs an as_of cutoff
    }

    // event_features is optional: empty (or absent) => a static carry-through
    // featurization of the account_features columns. When present it must be an array;
    // each feature is windowed, so a non-empty list requires a time_column (checked
    // after parsing).
    std::set<std::string> output_names;  // every output column name must be unique
    const nlohmann::json empty_features = nlohmann::json::array();
    if (value.contains("event_features") && !value.at("event_features").is_array()) {
        return std::nullopt;
    }
    const nlohmann::json& event_features =
        value.contains("event_features") ? value.at("event_features") : empty_features;
    for (const nlohmann::json& entry : event_features) {
        if (!entry.is_object()) return std::nullopt;
        for (const auto& [key, _] : entry.items()) {
            if (key != "name" && key != "agg" && key != "value_column" &&
                key != "window_start" && key != "type_column" && key != "type_value") {
                return std::nullopt;  // unknown key
            }
        }
        if (!entry.contains("name") || !entry.at("name").is_string() ||
            entry.at("name").get<std::string>().empty()) {
            return std::nullopt;
        }
        if (!entry.contains("agg") || !entry.at("agg").is_string()) return std::nullopt;
        const std::optional<AggregateOp> agg =
            aggregate_op_from_string(entry.at("agg").get<std::string>());
        if (!agg) return std::nullopt;

        WindowedFeature feature;
        feature.name = entry.at("name").get<std::string>();
        feature.agg = *agg;

        if (entry.contains("value_column")) {
            if (!entry.at("value_column").is_string() ||
                entry.at("value_column").get<std::string>().empty()) {
                return std::nullopt;
            }
            feature.value_column = entry.at("value_column").get<std::string>();
        }
        // sum/min/max/mean reduce a column and must name one; count must not need it.
        if (aggregate_needs_column(*agg) && feature.value_column.empty()) {
            return std::nullopt;
        }

        if (entry.contains("window_start")) {
            if (!entry.at("window_start").is_number()) return std::nullopt;
            const double window_start = entry.at("window_start").get<double>();
            if (window_start > spec.as_of) return std::nullopt;  // empty window
            feature.window_start = window_start;
        }

        const bool has_type_column = entry.contains("type_column");
        const bool has_type_value = entry.contains("type_value");
        if (has_type_column != has_type_value) return std::nullopt;  // together or not
        if (has_type_column) {
            if (!entry.at("type_column").is_string() ||
                entry.at("type_column").get<std::string>().empty()) {
                return std::nullopt;
            }
            if (!entry.at("type_value").is_number()) return std::nullopt;
            feature.type_column = entry.at("type_column").get<std::string>();
            feature.type_value = entry.at("type_value").get<double>();
        }

        if (!output_names.insert(feature.name).second) return std::nullopt;  // dup name
        spec.event_features.push_back(std::move(feature));
    }
    if (!spec.event_features.empty() && spec.time_column.empty()) {
        return std::nullopt;  // windowed event features need a time_column
    }

    if (value.contains("account_features")) {
        if (!value.at("account_features").is_array()) return std::nullopt;
        for (const nlohmann::json& column : value.at("account_features")) {
            if (!column.is_string() || column.get<std::string>().empty()) {
                return std::nullopt;
            }
            const std::string name = column.get<std::string>();
            if (!output_names.insert(name).second) return std::nullopt;  // dup / collision
            spec.account_features.push_back(name);
        }
    }

    if (value.contains("label_column")) {
        if (auto label = string_field("label_column")) spec.label_column = *label;
        else return std::nullopt;
    }

    // A spec must produce at least one feature column (a windowed event feature or a
    // carried-through account feature); a label alone is not a feature table.
    if (spec.event_features.empty() && spec.account_features.empty()) {
        return std::nullopt;
    }

    return spec;
}

// --- FeatureTable ------------------------------------------------------------

const std::vector<double>* FeatureTable::feature(std::string_view name) const {
    for (std::size_t index = 0; index < feature_names.size(); ++index) {
        if (feature_names[index] == name) return &features[index];
    }
    return nullptr;
}

DataFrameResult FeatureTable::to_data_frame(compute::ComputeDevice& device,
                                            std::size_t chunk_rows) const {
    // Materialize the table back into a store + schema and reuse the storage loader,
    // so no new device-buffer-construction path is introduced.
    zetta::storage::JsonStore store;
    for (std::size_t row = 0; row < row_count(); ++row) {
        nlohmann::json doc;
        doc["entity"] = entities[row];
        for (std::size_t column = 0; column < feature_names.size(); ++column) {
            doc[feature_names[column]] = features[column][row];
        }
        if (has_label()) doc[label_name] = labels[row];
        store.add("row-" + std::to_string(row), std::move(doc));
    }

    DataFrameSchema schema;
    schema.columns.push_back(ColumnSchema{.name = "entity", .dtype = DataType::Int64});
    for (const std::string& name : feature_names) {
        schema.columns.push_back(ColumnSchema{.name = name, .dtype = DataType::Float64});
    }
    if (has_label()) {
        schema.columns.push_back(
            ColumnSchema{.name = label_name, .dtype = DataType::Float64});
    }
    return load_data_frame(store, schema, device, chunk_rows);
}

// --- point_in_time_featurize -------------------------------------------------

FeaturizeResult point_in_time_featurize(const DataFrame& events,
                                        const DataFrame& accounts,
                                        const FeaturizeSpec& spec) {
    FeaturizeResult out;

    if (spec.entity_column.empty()) {
        out.status =
            ComputeStatus::invalid_argument("featurize: spec needs an entity_column");
        return out;
    }
    if (spec.event_features.empty() && spec.account_features.empty()) {
        out.status = ComputeStatus::invalid_argument(
            "featurize: spec needs at least one event feature or account feature");
        return out;
    }
    if (!spec.event_features.empty() && spec.time_column.empty()) {
        out.status = ComputeStatus::invalid_argument(
            "featurize: windowed event features need a time_column");
        return out;
    }

    // Static (no event features): the accounts frame IS the row universe (each row is
    // one entity's engineered feature record), so the point-in-time row filter
    // (time <= as_of, leakage-free) applies *here* when a time_column is set — a row
    // after the cutoff is excluded entirely. With windowed event features the accounts
    // dimension is the full universe and the as_of windowing happens per feature over
    // the events frame instead (the churn shape).
    DataFrame static_filtered;
    const DataFrame* account_source = &accounts;
    if (spec.event_features.empty() && !spec.time_column.empty()) {
        DataFrameResult filtered =
            filter(accounts, FilterPredicate{.column = spec.time_column,
                                             .comparison = Comparison::LessEqual,
                                             .value = spec.as_of});
        if (!filtered.ok()) {
            out.status = filtered.status;
            return out;
        }
        static_filtered = std::move(filtered.frame);
        account_source = &static_filtered;
    }

    // The entity universe + the static account dimension come from the accounts frame
    // (left-join semantics: one output row per account). Keys are exact int64.
    std::vector<std::int64_t> account_keys;
    out.status = read_int64(*account_source, spec.entity_column, account_keys);
    if (!out.status.ok()) return out;

    std::vector<std::vector<double>> account_columns(spec.account_features.size());
    for (std::size_t index = 0; index < spec.account_features.size(); ++index) {
        out.status = read_doubles(*account_source, spec.account_features[index],
                                  account_columns[index]);
        if (!out.status.ok()) return out;
    }
    std::vector<double> label_values;
    if (!spec.label_column.empty()) {
        out.status = read_doubles(*account_source, spec.label_column, label_values);
        if (!out.status.ok()) return out;
    }

    // Dedup the accounts into the ascending entity universe (a dimension has one row
    // per account; first occurrence wins on the off chance of a duplicate).
    std::map<std::int64_t, std::size_t> universe;  // entity -> its accounts-frame row
    for (std::size_t row = 0; row < account_keys.size(); ++row) {
        universe.try_emplace(account_keys[row], row);
    }

    // Each windowed event feature -> per-entity value (densified to 0 below).
    std::vector<std::map<std::int64_t, double>> feature_maps(spec.event_features.size());
    for (std::size_t index = 0; index < spec.event_features.size(); ++index) {
        const WindowedFeature& feature = spec.event_features[index];

        // Optional event-type filter (the filter op) before the windowed aggregate.
        DataFrame typed_storage;
        const DataFrame* typed_events = &events;
        if (feature.has_type_filter()) {
            DataFrameResult typed = filter(
                events, FilterPredicate{.column = feature.type_column,
                                        .comparison = Comparison::Equal,
                                        .value = *feature.type_value});
            if (!typed.ok()) {
                out.status = typed.status;
                return out;
            }
            typed_storage = std::move(typed.frame);
            typed_events = &typed_storage;
        }

        out.status = compute_windowed_feature(*typed_events, spec.entity_column,
                                              spec.time_column, spec.as_of, feature,
                                              feature_maps[index]);
        if (!out.status.ok()) return out;
    }

    // Build the two entity-keyed relations — the static account dimension and the
    // windowed event features — both dense over the account universe, then attach
    // them with the equi-join op. Because both are dense + 1 row per entity, the
    // inner join is a clean 1:1 join (= the intended left join).
    HostRelation dimension;
    dimension.key_name = spec.entity_column;
    dimension.value_columns = spec.account_features;
    if (!spec.label_column.empty()) dimension.value_columns.push_back(spec.label_column);

    HostRelation event_features;
    event_features.key_name = spec.entity_column;
    for (const WindowedFeature& feature : spec.event_features) {
        event_features.value_columns.push_back(feature.name);
    }

    for (const auto& [entity, account_row] : universe) {  // ascending entity order
        dimension.keys.push_back(static_cast<double>(entity));
        for (const std::vector<double>& column : account_columns) {
            dimension.values.push_back(column[account_row]);
        }
        if (!spec.label_column.empty()) dimension.values.push_back(label_values[account_row]);

        event_features.keys.push_back(static_cast<double>(entity));
        for (const std::map<std::int64_t, double>& feature_map : feature_maps) {
            const auto found = feature_map.find(entity);
            event_features.values.push_back(found == feature_map.end() ? 0.0
                                                                       : found->second);
        }
    }

    const JoinedRows joined = hash_join(dimension, event_features);

    // Assemble the feature table. Join output columns are
    //   [entity, account_features…, (label), event_feature_names…].
    out.table.feature_names = spec.account_features;
    for (const WindowedFeature& feature : spec.event_features) {
        out.table.feature_names.push_back(feature.name);
    }
    out.table.label_name = spec.label_column;

    const std::size_t account_count = spec.account_features.size();
    const std::size_t has_label = spec.label_column.empty() ? 0 : 1;
    const std::size_t width = joined.width();
    out.table.features.resize(out.table.feature_names.size());

    // Rows in ascending entity order (the join preserves the left/dimension order,
    // which we built ascending; sort defensively so the contract holds regardless).
    std::vector<std::size_t> order(joined.row_count());
    for (std::size_t row = 0; row < order.size(); ++row) order[row] = row;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return joined.data[a * width] < joined.data[b * width];
    });

    for (std::size_t output_row : order) {
        const double* cells = joined.data.data() + output_row * width;
        out.table.entities.push_back(static_cast<std::int64_t>(cells[0]));
        // account features: columns [1, 1+account_count)
        for (std::size_t column = 0; column < account_count; ++column) {
            out.table.features[column].push_back(cells[1 + column]);
        }
        if (has_label != 0) {
            out.table.labels.push_back(cells[1 + account_count]);
        }
        // event features: columns after the (optional) label
        const std::size_t event_base = 1 + account_count + has_label;
        for (std::size_t column = 0; column < spec.event_features.size(); ++column) {
            out.table.features[account_count + column].push_back(cells[event_base + column]);
        }
    }

    out.status = ComputeStatus::success();
    return out;
}

}  // namespace zetta::ddf
