// SPDX-License-Identifier: MIT

#include "zetta/ddf/group_by.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::ddf {

using compute::ComputeStatus;
using compute::DataType;

namespace {

// Read column `name` back from `frame` as doubles, regardless of its dtype, going
// through the frame's device download seam (so it works for any backend). A failing
// status if the column is absent or a transfer fails.
ComputeStatus read_as_double(const DataFrame& frame, const std::string& name,
                             std::vector<double>& out) {
    const Column* column = frame.column(name);
    if (column == nullptr) {
        return ComputeStatus::invalid_argument("group_by: no column named '" + name +
                                               "'");
    }
    switch (column->dtype()) {
        case DataType::Float64:
            return frame.to_host<double>(name, out);
        case DataType::Float32: {
            std::vector<float> tmp;
            const ComputeStatus status = frame.to_host<float>(name, tmp);
            if (!status.ok()) return status;
            out.assign(tmp.begin(), tmp.end());
            return ComputeStatus::success();
        }
        case DataType::Int32: {
            std::vector<std::int32_t> tmp;
            const ComputeStatus status = frame.to_host<std::int32_t>(name, tmp);
            if (!status.ok()) return status;
            out.assign(tmp.begin(), tmp.end());
            return ComputeStatus::success();
        }
        case DataType::Int64: {
            std::vector<std::int64_t> tmp;
            const ComputeStatus status = frame.to_host<std::int64_t>(name, tmp);
            if (!status.ok()) return status;
            out.assign(tmp.begin(), tmp.end());
            return ComputeStatus::success();
        }
        case DataType::UInt32: {
            std::vector<std::uint32_t> tmp;
            const ComputeStatus status = frame.to_host<std::uint32_t>(name, tmp);
            if (!status.ok()) return status;
            out.assign(tmp.begin(), tmp.end());
            return ComputeStatus::success();
        }
        case DataType::UInt64: {
            std::vector<std::uint64_t> tmp;
            const ComputeStatus status = frame.to_host<std::uint64_t>(name, tmp);
            if (!status.ok()) return status;
            out.assign(tmp.begin(), tmp.end());
            return ComputeStatus::success();
        }
        case DataType::UInt8: {
            std::vector<std::uint8_t> tmp;
            const ComputeStatus status = frame.to_host<std::uint8_t>(name, tmp);
            if (!status.ok()) return status;
            out.assign(tmp.begin(), tmp.end());
            return ComputeStatus::success();
        }
    }
    return ComputeStatus::invalid_argument("group_by: column '" + name +
                                           "' has an unknown dtype");
}

// Is `value` a non-negative whole number that fits in a std::size_t? Wire headers
// (aggregate count, group count) are doubles that must round-trip an integer.
bool is_index(double value, std::size_t& out) {
    if (!(value >= 0.0) || std::floor(value) != value) return false;
    if (value > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

}  // namespace

// --- AggregateOp -------------------------------------------------------------

std::string_view to_string(AggregateOp op) {
    switch (op) {
        case AggregateOp::Count: return "count";
        case AggregateOp::Sum:   return "sum";
        case AggregateOp::Min:   return "min";
        case AggregateOp::Max:   return "max";
        case AggregateOp::Mean:  return "mean";
    }
    return "unknown";
}

std::optional<AggregateOp> aggregate_op_from_string(std::string_view name) {
    constexpr AggregateOp kinds[] = {AggregateOp::Count, AggregateOp::Sum,
                                     AggregateOp::Min, AggregateOp::Max,
                                     AggregateOp::Mean};
    for (const AggregateOp kind : kinds) {
        if (to_string(kind) == name) return kind;
    }
    return std::nullopt;
}

bool aggregate_needs_column(AggregateOp op) { return op != AggregateOp::Count; }

std::string Aggregate::output_name() const {
    if (!output.empty()) return output;
    if (op == AggregateOp::Count) return "count";
    return std::string{to_string(op)} + "_" + column;
}

// --- GroupBySpec JSON boundary ----------------------------------------------

nlohmann::json GroupBySpec::to_json() const {
    nlohmann::json aggregates_json = nlohmann::json::array();
    for (const Aggregate& aggregate : aggregates) {
        nlohmann::json entry{{"op", to_string(aggregate.op)}};
        if (!aggregate.column.empty()) entry["column"] = aggregate.column;
        if (!aggregate.output.empty()) entry["output"] = aggregate.output;
        aggregates_json.push_back(std::move(entry));
    }
    return nlohmann::json{{"key", key},
                          {"aggregates", std::move(aggregates_json)}};
}

// The GroupBySpec JSON Schema (draft-07) — the code-owned source of truth;
// schemas/group_by_spec.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& group_by_spec_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/group_by_spec.schema.json",
  "title": "GroupBySpec",
  "description": "The specification of a DDF groupby-aggregate (ARC-12): the numeric key column to group by, and the set of aggregates (count/sum/min/max/mean) to compute over each group's columns.",
  "type": "object",
  "additionalProperties": false,
  "required": ["key", "aggregates"],
  "properties": {
    "key": {
      "type": "string",
      "minLength": 1,
      "description": "The frame column whose distinct values define the groups."
    },
    "aggregates": {
      "type": "array",
      "minItems": 1,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["op"],
        "properties": {
          "op": {
            "type": "string",
            "enum": ["count", "sum", "min", "max", "mean"],
            "description": "The aggregate to compute over each group."
          },
          "column": {
            "type": "string",
            "minLength": 1,
            "description": "The frame column aggregated; required for sum/min/max/mean, ignored for count."
          },
          "output": {
            "type": "string",
            "minLength": 1,
            "description": "The result column's name; defaults to \"<op>_<column>\" (or \"count\") when omitted."
          }
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

std::optional<GroupBySpec> GroupBySpec::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("key") || !value["key"].is_string()) return std::nullopt;
    if (!value.contains("aggregates") || !value["aggregates"].is_array()) {
        return std::nullopt;
    }
    const nlohmann::json& aggregates_json = value["aggregates"];
    if (aggregates_json.empty()) return std::nullopt;  // minItems: 1

    GroupBySpec spec;
    spec.key = value["key"].get<std::string>();
    if (spec.key.empty()) return std::nullopt;

    for (const nlohmann::json& entry : aggregates_json) {
        if (!entry.is_object()) return std::nullopt;
        if (!entry.contains("op") || !entry["op"].is_string()) return std::nullopt;
        const std::optional<AggregateOp> op =
            aggregate_op_from_string(entry["op"].get<std::string>());
        if (!op) return std::nullopt;

        Aggregate aggregate;
        aggregate.op = *op;
        if (entry.contains("column")) {
            if (!entry["column"].is_string()) return std::nullopt;
            aggregate.column = entry["column"].get<std::string>();
            if (aggregate.column.empty()) return std::nullopt;
        }
        if (entry.contains("output")) {
            if (!entry["output"].is_string()) return std::nullopt;
            aggregate.output = entry["output"].get<std::string>();
            if (aggregate.output.empty()) return std::nullopt;
        }
        // Conditional rule (not expressible in the draft-07 shape): an aggregate
        // that reads a column must name one.
        if (aggregate_needs_column(aggregate.op) && aggregate.column.empty()) {
            return std::nullopt;
        }
        spec.aggregates.push_back(std::move(aggregate));
    }
    return spec;
}

// --- GroupByResult -----------------------------------------------------------

const std::vector<double>* GroupByResult::value(
    std::string_view aggregate_name) const {
    for (std::size_t index = 0; index < aggregate_names.size(); ++index) {
        if (aggregate_names[index] == aggregate_name) return &values[index];
    }
    return nullptr;
}

// --- GroupByPartial ----------------------------------------------------------

bool GroupByPartial::merge(const GroupByPartial& other) {
    if (ops != other.ops) return false;
    for (const auto& [key, accumulators] : other.groups) {
        const auto [iterator, inserted] = groups.try_emplace(key, accumulators);
        if (inserted) continue;  // copied wholesale
        std::vector<double>& mine = iterator->second;
        mine[0] += accumulators[0];  // count
        for (std::size_t index = 0; index < ops.size(); ++index) {
            const double incoming = accumulators[index + 1];
            double& slot = mine[index + 1];
            switch (ops[index]) {
                case AggregateOp::Count:
                case AggregateOp::Sum:
                case AggregateOp::Mean:
                    slot += incoming;
                    break;
                case AggregateOp::Min:
                    slot = std::min(slot, incoming);
                    break;
                case AggregateOp::Max:
                    slot = std::max(slot, incoming);
                    break;
            }
        }
    }
    return true;
}

std::vector<double> GroupByPartial::to_wire() const {
    const std::size_t width = ops.size() + 1;  // count + one accumulator per op
    std::vector<double> wire;
    wire.reserve(2 + groups.size() * (width + 1));
    wire.push_back(static_cast<double>(ops.size()));
    wire.push_back(static_cast<double>(groups.size()));
    for (const auto& [key, accumulators] : groups) {  // ascending key (std::map)
        wire.push_back(key);
        for (const double slot : accumulators) wire.push_back(slot);
    }
    return wire;
}

std::optional<GroupByPartial> GroupByPartial::from_wire(
    std::vector<AggregateOp> ops, const std::vector<double>& wire) {
    if (wire.size() < 2) return std::nullopt;
    std::size_t aggregate_count = 0;
    std::size_t group_count = 0;
    if (!is_index(wire[0], aggregate_count) || aggregate_count != ops.size()) {
        return std::nullopt;
    }
    if (!is_index(wire[1], group_count)) return std::nullopt;

    const std::size_t width = ops.size() + 1;             // count + accumulators
    const std::size_t stride = width + 1;                 // key + the above
    if (wire.size() != 2 + group_count * stride) return std::nullopt;

    GroupByPartial partial;
    partial.ops = std::move(ops);
    std::size_t offset = 2;
    for (std::size_t group = 0; group < group_count; ++group) {
        const double key = wire[offset];
        if (std::isnan(key)) return std::nullopt;
        std::vector<double> accumulators(wire.begin() + offset + 1,
                                         wire.begin() + offset + 1 + width);
        if (!partial.groups.emplace(key, std::move(accumulators)).second) {
            return std::nullopt;  // duplicate key
        }
        offset += stride;
    }
    return partial;
}

GroupByResult GroupByPartial::finalize(const GroupBySpec& spec) const {
    GroupByResult result;
    result.key_name = spec.key;
    if (spec.aggregates.size() != ops.size()) return result;  // misaligned -> empty

    result.aggregate_names.reserve(spec.aggregates.size());
    for (const Aggregate& aggregate : spec.aggregates) {
        result.aggregate_names.push_back(aggregate.output_name());
    }
    result.values.resize(ops.size());
    result.keys.reserve(groups.size());
    for (const auto& [key, accumulators] : groups) {  // ascending key
        result.keys.push_back(key);
        const double count = accumulators[0];
        for (std::size_t index = 0; index < ops.size(); ++index) {
            const double accumulator = accumulators[index + 1];
            double output = accumulator;
            switch (ops[index]) {
                case AggregateOp::Count:
                    output = count;
                    break;
                case AggregateOp::Mean:
                    output = count > 0.0 ? accumulator / count : 0.0;
                    break;
                case AggregateOp::Sum:
                case AggregateOp::Min:
                case AggregateOp::Max:
                    output = accumulator;
                    break;
            }
            result.values[index].push_back(output);
        }
    }
    return result;
}

// --- Local + single-node group-by --------------------------------------------

GroupByPartialOutcome local_partial(const DataFrame& frame,
                                    const GroupBySpec& spec) {
    if (spec.key.empty()) {
        return {GroupByPartial{},
                ComputeStatus::invalid_argument(
                    "group_by: the spec has no key column")};
    }
    for (const Aggregate& aggregate : spec.aggregates) {
        if (aggregate_needs_column(aggregate.op) && aggregate.column.empty()) {
            return {GroupByPartial{},
                    ComputeStatus::invalid_argument(
                        "group_by: aggregate '" +
                        std::string{to_string(aggregate.op)} +
                        "' needs a column but none was named")};
        }
    }

    GroupByPartial partial;
    partial.ops.reserve(spec.aggregates.size());
    for (const Aggregate& aggregate : spec.aggregates) {
        partial.ops.push_back(aggregate.op);
    }
    const std::size_t aggregate_count = spec.aggregates.size();

    std::vector<double> keys;
    ComputeStatus status = read_as_double(frame, spec.key, keys);
    if (!status.ok()) return {GroupByPartial{}, status};

    // The values for each aggregate's column (left empty for Count).
    std::vector<std::vector<double>> columns(aggregate_count);
    for (std::size_t index = 0; index < aggregate_count; ++index) {
        if (!aggregate_needs_column(spec.aggregates[index].op)) continue;
        status = read_as_double(frame, spec.aggregates[index].column, columns[index]);
        if (!status.ok()) return {GroupByPartial{}, status};
    }

    constexpr double kPositiveInfinity = std::numeric_limits<double>::infinity();
    const std::size_t row_count = frame.row_count();
    for (std::size_t row = 0; row < row_count; ++row) {
        const double key = keys[row];
        if (std::isnan(key)) {
            return {GroupByPartial{},
                    ComputeStatus::invalid_argument(
                        "group_by: NaN group key at row " + std::to_string(row))};
        }
        const auto [iterator, inserted] = partial.groups.try_emplace(key);
        std::vector<double>& accumulators = iterator->second;
        if (inserted) {
            accumulators.assign(aggregate_count + 1, 0.0);
            for (std::size_t index = 0; index < aggregate_count; ++index) {
                if (partial.ops[index] == AggregateOp::Min) {
                    accumulators[index + 1] = kPositiveInfinity;
                } else if (partial.ops[index] == AggregateOp::Max) {
                    accumulators[index + 1] = -kPositiveInfinity;
                }
            }
        }
        accumulators[0] += 1.0;  // count
        for (std::size_t index = 0; index < aggregate_count; ++index) {
            double& slot = accumulators[index + 1];
            switch (partial.ops[index]) {
                case AggregateOp::Count:
                    slot += 1.0;
                    break;
                case AggregateOp::Sum:
                case AggregateOp::Mean:
                    slot += columns[index][row];
                    break;
                case AggregateOp::Min:
                    slot = std::min(slot, columns[index][row]);
                    break;
                case AggregateOp::Max:
                    slot = std::max(slot, columns[index][row]);
                    break;
            }
        }
    }
    return {std::move(partial), ComputeStatus::success()};
}

GroupByOutcome group_by(const DataFrame& frame, const GroupBySpec& spec) {
    const GroupByPartialOutcome local = local_partial(frame, spec);
    if (!local.ok()) return {GroupByResult{}, local.status};
    return {local.partial.finalize(spec), ComputeStatus::success()};
}

}  // namespace zetta::ddf
