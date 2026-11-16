// SPDX-License-Identifier: MIT

#include "zetta/ddf/as_of.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::ddf {

using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DataType;
using compute::ReduceOp;

namespace {

// --- ReduceOp <-> name (the JSON boundary uses these) ------------------------
// compute::ReduceOp has no to_string of its own (it is an operator parameter, not
// a stored identifier), so the canonical names live here at the one boundary that
// serializes them — matching the "sum"/"product"/"minimum"/"maximum" spelling
// CollectiveOp already uses on the wire.

std::string_view reduce_op_to_string(ReduceOp op) {
    switch (op) {
        case ReduceOp::Sum:     return "sum";
        case ReduceOp::Product: return "product";
        case ReduceOp::Minimum: return "minimum";
        case ReduceOp::Maximum: return "maximum";
    }
    return "sum";
}

std::optional<ReduceOp> reduce_op_from_string(std::string_view name) {
    if (name == "sum")     return ReduceOp::Sum;
    if (name == "product") return ReduceOp::Product;
    if (name == "minimum") return ReduceOp::Minimum;
    if (name == "maximum") return ReduceOp::Maximum;
    return std::nullopt;
}

bool is_integer_dtype(DataType dtype) {
    switch (dtype) {
        case DataType::Int32:
        case DataType::Int64:
        case DataType::UInt32:
        case DataType::UInt64:
        case DataType::UInt8:
            return true;
        case DataType::Float32:
        case DataType::Float64:
            return false;
    }
    return false;
}

// Download a column's native-typed chunks, casting each element to `Target`. The
// one place dtype is dispatched: Column::to_host requires the host type to match
// the column's dtype exactly, so we read into the native vector then cast.
template <class Native, class Target>
ComputeStatus read_casting(const ComputeDevice& device, const Column& column,
                           std::vector<Target>& out) {
    std::vector<Native> native;
    const ComputeStatus status = column.to_host<Native>(device, native);
    if (!status.ok()) return status;
    out.assign(native.begin(), native.end());
    return ComputeStatus::success();
}

// Read any numeric column to host as double (lossless enough for time / value
// aggregation; integer columns widen into the double mantissa).
ComputeStatus read_as_doubles(const ComputeDevice& device, const Column& column,
                              std::vector<double>& out) {
    switch (column.dtype()) {
        case DataType::Float32: return read_casting<float, double>(device, column, out);
        case DataType::Float64: return read_casting<double, double>(device, column, out);
        case DataType::Int32:   return read_casting<std::int32_t, double>(device, column, out);
        case DataType::Int64:   return read_casting<std::int64_t, double>(device, column, out);
        case DataType::UInt32:  return read_casting<std::uint32_t, double>(device, column, out);
        case DataType::UInt64:  return read_casting<std::uint64_t, double>(device, column, out);
        case DataType::UInt8:   return read_casting<std::uint8_t, double>(device, column, out);
    }
    return ComputeStatus::invalid_argument("as_of_aggregate: unknown column dtype");
}

// Read an integer column to host as int64 — the exact group key (no float, so
// equality grouping is exact). Precondition: is_integer_dtype(column.dtype()).
ComputeStatus read_as_int64(const ComputeDevice& device, const Column& column,
                            std::vector<std::int64_t>& out) {
    switch (column.dtype()) {
        case DataType::Int32:  return read_casting<std::int32_t, std::int64_t>(device, column, out);
        case DataType::Int64:  return read_casting<std::int64_t, std::int64_t>(device, column, out);
        case DataType::UInt32: return read_casting<std::uint32_t, std::int64_t>(device, column, out);
        case DataType::UInt64: return read_casting<std::uint64_t, std::int64_t>(device, column, out);
        case DataType::UInt8:  return read_casting<std::uint8_t, std::int64_t>(device, column, out);
        case DataType::Float32:
        case DataType::Float64:
            return ComputeStatus::invalid_argument(
                "as_of_aggregate: entity column must be an integer dtype");
    }
    return ComputeStatus::invalid_argument("as_of_aggregate: unknown entity dtype");
}

// The reduction identity for an empty group — what `values[i]` carries when no
// row qualified, so the combine below is seeded correctly.
double reduce_identity(ReduceOp op) {
    switch (op) {
        case ReduceOp::Sum:     return 0.0;
        case ReduceOp::Product: return 1.0;
        case ReduceOp::Minimum: return std::numeric_limits<double>::infinity();
        case ReduceOp::Maximum: return -std::numeric_limits<double>::infinity();
    }
    return 0.0;
}

double reduce_combine(ReduceOp op, double accumulator, double value) {
    switch (op) {
        case ReduceOp::Sum:     return accumulator + value;
        case ReduceOp::Product: return accumulator * value;
        case ReduceOp::Minimum: return std::min(accumulator, value);
        case ReduceOp::Maximum: return std::max(accumulator, value);
    }
    return accumulator;
}

}  // namespace

// --- AsOfAggregate JSON boundary ---------------------------------------------

nlohmann::json AsOfAggregate::to_json() const {
    nlohmann::json value{
        {"entity_column", entity_column},
        {"time_column", time_column},
        {"value_column", value_column},
        {"as_of", as_of},
        {"op", reduce_op_to_string(op)},
    };
    if (window_start) value["window_start"] = *window_start;
    return value;
}

// The AsOfAggregate JSON Schema (draft-07) — the code-owned source of truth;
// schemas/as_of_aggregate.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& as_of_aggregate_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/as_of_aggregate.schema.json",
  "title": "AsOfAggregate",
  "description": "A DDF window / as-of (point-in-time) aggregation over a numeric DataFrame (ARC-12, #384; part of the native distributed accelerated compute pass for UC-1 churn — #264). The leakage-free featurization primitive: for each entity, reduce the values of only the rows whose timestamp falls inside the time window — at or before `as_of`, and (when set) at or after `window_start` — so a feature computed as of t0 never sees a future event. Mirrored by zetta::ddf::AsOfAggregate in include/zetta/ddf/as_of.hpp and enforced by AsOfAggregate::from_json() in src/core/ddf/as_of.cpp — keep them in sync.",
  "type": "object",
  "additionalProperties": false,
  "required": ["entity_column", "time_column", "value_column", "as_of", "op"],
  "properties": {
    "entity_column": {
      "description": "The integer-dtype column whose value groups rows into entities (the exact group key).",
      "type": "string",
      "minLength": 1
    },
    "time_column": {
      "description": "The numeric column carrying each row's timestamp (e.g. epoch seconds, or any monotonic encoding of time).",
      "type": "string",
      "minLength": 1
    },
    "value_column": {
      "description": "The numeric column reduced per entity over the window.",
      "type": "string",
      "minLength": 1
    },
    "as_of": {
      "description": "The upper cutoff (inclusive): a row qualifies iff its timestamp is <= as_of. The point-in-time at which the feature is computed.",
      "type": "number"
    },
    "window_start": {
      "description": "Optional lower cutoff (inclusive): a row also qualifies iff its timestamp is >= window_start, giving a trailing window [window_start, as_of]. Omitted for the unbounded-below (-inf, as_of] point-in-time case. Must not exceed as_of (that would be an empty window).",
      "type": "number"
    },
    "op": {
      "description": "How qualifying values combine into the per-entity aggregate.",
      "type": "string",
      "enum": ["sum", "product", "minimum", "maximum"]
    }
  }
}
)JSONSCHEMA");
    return schema;
}

std::optional<AsOfAggregate> AsOfAggregate::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    // Reject unknown keys (additionalProperties: false in the schema).
    static constexpr std::string_view kKnownKeys[] = {
        "entity_column", "time_column", "value_column",
        "as_of",         "window_start", "op"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), it.key()) ==
            std::end(kKnownKeys)) {
            return std::nullopt;
        }
    }

    const auto string_field = [&](const char* key,
                                  std::string& out) -> bool {
        if (!value.contains(key) || !value[key].is_string()) return false;
        out = value[key].get<std::string>();
        return !out.empty();
    };

    AsOfAggregate spec;
    if (!string_field("entity_column", spec.entity_column)) return std::nullopt;
    if (!string_field("time_column", spec.time_column)) return std::nullopt;
    if (!string_field("value_column", spec.value_column)) return std::nullopt;

    if (!value.contains("as_of") || !value["as_of"].is_number()) return std::nullopt;
    spec.as_of = value["as_of"].get<double>();

    if (!value.contains("op") || !value["op"].is_string()) return std::nullopt;
    const std::optional<ReduceOp> op =
        reduce_op_from_string(value["op"].get<std::string>());
    if (!op) return std::nullopt;
    spec.op = *op;

    if (value.contains("window_start")) {
        if (!value["window_start"].is_number()) return std::nullopt;
        const double window_start = value["window_start"].get<double>();
        if (window_start > spec.as_of) return std::nullopt;  // empty window
        spec.window_start = window_start;
    }
    return spec;
}

// --- The operator ------------------------------------------------------------

AsOfResult as_of_aggregate(const DataFrame& frame, const AsOfAggregate& spec) {
    if (spec.window_start && *spec.window_start > spec.as_of) {
        return {AsOfColumn{},
                ComputeStatus::invalid_argument(
                    "as_of_aggregate: window_start is greater than as_of (empty window)")};
    }

    const Column* entity = frame.column(spec.entity_column);
    if (entity == nullptr) {
        return {AsOfColumn{},
                ComputeStatus::invalid_argument(
                    "as_of_aggregate: no entity column '" + spec.entity_column + "'")};
    }
    const Column* time = frame.column(spec.time_column);
    if (time == nullptr) {
        return {AsOfColumn{},
                ComputeStatus::invalid_argument(
                    "as_of_aggregate: no time column '" + spec.time_column + "'")};
    }
    const Column* value = frame.column(spec.value_column);
    if (value == nullptr) {
        return {AsOfColumn{},
                ComputeStatus::invalid_argument(
                    "as_of_aggregate: no value column '" + spec.value_column + "'")};
    }
    if (!is_integer_dtype(entity->dtype())) {
        return {AsOfColumn{},
                ComputeStatus::invalid_argument(
                    "as_of_aggregate: entity column '" + spec.entity_column +
                    "' must be an integer dtype")};
    }
    if (frame.device() == nullptr) {
        return {AsOfColumn{},
                ComputeStatus::invalid_argument(
                    "as_of_aggregate: frame has no device to read columns from")};
    }
    const ComputeDevice& device = *frame.device();

    std::vector<std::int64_t> entity_keys;
    if (const ComputeStatus status = read_as_int64(device, *entity, entity_keys);
        !status.ok()) {
        return {AsOfColumn{}, status};
    }
    std::vector<double> times;
    if (const ComputeStatus status = read_as_doubles(device, *time, times);
        !status.ok()) {
        return {AsOfColumn{}, status};
    }
    std::vector<double> values;
    if (const ComputeStatus status = read_as_doubles(device, *value, values);
        !status.ok()) {
        return {AsOfColumn{}, status};
    }

    // One accumulator per entity. std::map keeps entities in ascending key order
    // for a deterministic, sorted result.
    struct Accumulator {
        double value;
        std::int64_t count = 0;
    };
    std::map<std::int64_t, Accumulator> groups;
    const double lower = spec.window_start ? *spec.window_start
                                           : -std::numeric_limits<double>::infinity();
    const double identity = reduce_identity(spec.op);

    const std::size_t row_count = entity_keys.size();
    for (std::size_t row = 0; row < row_count; ++row) {
        const std::int64_t key = entity_keys[row];
        // Register every entity present in the frame (dense output), seeded with
        // the reduction identity and a zero count.
        const auto it = groups.try_emplace(key, Accumulator{identity, 0}).first;
        const double when = times[row];
        if (when <= spec.as_of && when >= lower) {
            it->second.value = reduce_combine(spec.op, it->second.value, values[row]);
            ++it->second.count;
        }
    }

    AsOfColumn column;
    column.entities.reserve(groups.size());
    column.values.reserve(groups.size());
    column.counts.reserve(groups.size());
    for (const auto& [key, accumulator] : groups) {
        column.entities.push_back(key);
        column.values.push_back(accumulator.value);
        column.counts.push_back(accumulator.count);
    }
    return {std::move(column), ComputeStatus::success()};
}

}  // namespace zetta::ddf
