#include "zetta/ddf/data_frame.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/data/categorical_encoding.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ddf {

using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DataType;
using compute::DeviceBuffer;

namespace {

// Parse a canonical dtype name ("float32", "int64", …) back to the enum — the
// inverse of compute::to_string(DataType), needed only by the schema boundary.
std::optional<DataType> data_type_from_string(std::string_view text) {
    constexpr DataType kinds[] = {DataType::Float32, DataType::Float64,
                                  DataType::Int32,   DataType::Int64,
                                  DataType::UInt32,  DataType::UInt64,
                                  DataType::UInt8};
    for (const DataType kind : kinds) {
        if (compute::to_string(kind) == text) return kind;
    }
    return std::nullopt;
}

// Extract one row's `field` as a value of host type `Element`, validating at the
// boundary: the field must exist and be a number, an integer column rejects a real
// number, an unsigned column rejects a negative, and any integer is range-checked
// against the column's dtype. A clear, path-qualified reason on failure.
template <class Element>
ComputeStatus extract(const nlohmann::json& doc, const std::string& field,
                      std::size_t row, bool timestamp,
                      const std::shared_ptr<const data::EntityKeyMapping>& entity_keys,
                      Element& out) {
    const std::string where =
        "load_data_frame: row " + std::to_string(row) + " field '" + field + "'";
    if (!doc.is_object() || !doc.contains(field)) {
        return ComputeStatus::invalid_argument(where + " is missing");
    }
    const nlohmann::json& value = doc.at(field);
    if constexpr (std::is_same_v<Element, std::int64_t>) {
        if (entity_keys && value.is_string()) {
            const std::optional<std::int64_t> derived =
                entity_keys->derived(value.get_ref<const std::string&>());
            if (!derived) {
                return ComputeStatus::invalid_argument(
                    where + " is absent from the resolved entity-key mapping");
            }
            out = *derived;
            return ComputeStatus::success();
        }
    }
    if constexpr (std::is_floating_point_v<Element>) {
        if (timestamp) {
            const std::optional<double> seconds =
                zetta::util::parse_time_seconds(value);
            if (!seconds) {
                return ComputeStatus::invalid_argument(
                    where + " is not a finite epoch number or ISO-8601 timestamp");
            }
            out = static_cast<Element>(*seconds);
            return ComputeStatus::success();
        }
        if (value.is_boolean()) {
            out = value.get<bool>() ? static_cast<Element>(1)
                                    : static_cast<Element>(0);
            return ComputeStatus::success();
        }
        if (!value.is_number()) {
            return ComputeStatus::invalid_argument(where + " is not a number");
        }
        out = static_cast<Element>(value.get<double>());
        return ComputeStatus::success();
    } else {
        if (value.is_boolean()) {
            out = value.get<bool>() ? static_cast<Element>(1)
                                    : static_cast<Element>(0);
            return ComputeStatus::success();
        }
        if (!value.is_number()) {
            return ComputeStatus::invalid_argument(where + " is not a number");
        }
        if (!value.is_number_integer()) {
            return ComputeStatus::invalid_argument(
                where + " expects an integer but found a real number");
        }
        const std::string dtype_name{
            compute::to_string(compute::data_type_of<Element>)};
        if constexpr (std::is_signed_v<Element>) {
            const std::int64_t raw = value.get<std::int64_t>();
            if (raw < static_cast<std::int64_t>(std::numeric_limits<Element>::min()) ||
                raw > static_cast<std::int64_t>(std::numeric_limits<Element>::max())) {
                return ComputeStatus::invalid_argument(where + " is out of range for " +
                                                       dtype_name);
            }
            out = static_cast<Element>(raw);
        } else {
            std::uint64_t raw = 0;
            if (value.is_number_unsigned()) {
                raw = value.get<std::uint64_t>();
            } else {
                const std::int64_t signed_raw = value.get<std::int64_t>();
                if (signed_raw < 0) {
                    return ComputeStatus::invalid_argument(
                        where + " is negative but the column is unsigned (" +
                        dtype_name + ")");
                }
                raw = static_cast<std::uint64_t>(signed_raw);
            }
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<Element>::max())) {
                return ComputeStatus::invalid_argument(where + " is out of range for " +
                                                       dtype_name);
            }
            out = static_cast<Element>(raw);
        }
        return ComputeStatus::success();
    }
}

// Read `field` from every row of `store` into row-aligned device buffers of host
// type `Element`, one per chunk of `chunk_rows` rows. Stops + returns on the first
// boundary or device error.
template <class Element>
ComputeStatus fill_chunks(const zetta::storage::StorageEngine& store, const std::string& field,
                          bool timestamp,
                          const std::shared_ptr<const data::EntityKeyMapping>& entity_keys,
                          ComputeDevice& device, std::size_t chunk_rows,
                          std::vector<DeviceBuffer>& out) {
    const std::size_t row_count = store.size();
    std::vector<Element> host;
    for (std::size_t start = 0; start < row_count; start += chunk_rows) {
        const std::size_t length = std::min(chunk_rows, row_count - start);
        host.clear();
        host.reserve(length);
        for (std::size_t row = start; row < start + length; ++row) {
            Element value{};
            const zetta::storage::ParquetLazyPtr handle = store.at(row);
            if (!handle) {
                return ComputeStatus::backend_error(
                    "load_data_frame: row " + std::to_string(row) +
                    " could not be read: " + handle.error());
            }
            const ComputeStatus status =
                extract<Element>(*handle, field, row, timestamp, entity_keys,
                                 value);
            if (!status.ok()) return status;
            host.push_back(value);
        }
        DeviceBuffer buffer = device.allocate(compute::data_type_of<Element>, length);
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "load_data_frame: failed to allocate a device buffer for column '" +
                field + "'");
        }
        const ComputeStatus status =
            device.copy_from_host(host.data(), buffer, length * sizeof(Element));
        if (!status.ok()) return status;
        out.push_back(std::move(buffer));
    }
    return ComputeStatus::success();
}

// One-hot indicator column (#1203): 1 where the row's field is `level`, else 0.
// A missing field or null is a 0 — a connector table is schemaless at the row
// level, and "not this level" is exactly what an absent value means here.
ComputeStatus fill_indicator_chunks(const zetta::storage::StorageEngine& store,
                                    const std::string& field, const std::string& level,
                                    ComputeDevice& device, std::size_t chunk_rows,
                                    std::vector<DeviceBuffer>& out) {
    const std::size_t row_count = store.size();
    std::vector<double> host;
    for (std::size_t start = 0; start < row_count; start += chunk_rows) {
        const std::size_t length = std::min(chunk_rows, row_count - start);
        host.clear();
        host.reserve(length);
        for (std::size_t row = start; row < start + length; ++row) {
            const zetta::storage::ParquetLazyPtr handle = store.at(row);
            if (!handle) {
                return ComputeStatus::backend_error(
                    "load_data_frame: row " + std::to_string(row) +
                    " could not be read: " + handle.error());
            }
            const nlohmann::json& doc = *handle;
            double value = 0.0;
            if (doc.is_object()) {
                if (const auto found = doc.find(field); found != doc.end()) {
                    value = data::categorical_indicator(*found, level);
                }
            }
            host.push_back(value);
        }
        DeviceBuffer buffer = device.allocate(DataType::Float64, length);
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "load_data_frame: failed to allocate a device buffer for column '" +
                field + "=" + level + "'");
        }
        const ComputeStatus status =
            device.copy_from_host(host.data(), buffer, length * sizeof(double));
        if (!status.ok()) return status;
        out.push_back(std::move(buffer));
    }
    return ComputeStatus::success();
}

// Build one column by dispatching the chunk fill on the column's runtime dtype.
ComputeStatus build_column(const zetta::storage::StorageEngine& store, const ColumnSchema& spec,
                           ComputeDevice& device, std::size_t chunk_rows,
                           Column& out) {
    if (spec.category_level) {
        if (spec.dtype != DataType::Float64 || spec.timestamp) {
            return ComputeStatus::invalid_argument(
                "load_data_frame: categorical indicator column '" + spec.name +
                "' must be a non-timestamp float64");
        }
        std::vector<DeviceBuffer> chunks;
        const ComputeStatus status = fill_indicator_chunks(
            store, spec.source_field(), *spec.category_level, device, chunk_rows, chunks);
        if (!status.ok()) return status;
        out = Column{spec.name, spec.dtype, std::move(chunks), store.size()};
        return ComputeStatus::success();
    }
    if (spec.timestamp && spec.dtype != DataType::Float64) {
        return ComputeStatus::invalid_argument(
            "load_data_frame: timestamp column '" + spec.name +
            "' must use float64");
    }
    const std::string& field = spec.source_field();
    std::vector<DeviceBuffer> chunks;
    ComputeStatus status;
    switch (spec.dtype) {
        case DataType::Float32:
            status = fill_chunks<float>(store, field, spec.timestamp,
                                        spec.entity_keys, device, chunk_rows,
                                        chunks);
            break;
        case DataType::Float64:
            status = fill_chunks<double>(store, field, spec.timestamp,
                                         spec.entity_keys, device, chunk_rows,
                                         chunks);
            break;
        case DataType::Int32:
            status = fill_chunks<std::int32_t>(store, field, spec.timestamp,
                                               spec.entity_keys, device,
                                               chunk_rows, chunks);
            break;
        case DataType::Int64:
            status = fill_chunks<std::int64_t>(store, field, spec.timestamp,
                                               spec.entity_keys, device,
                                               chunk_rows, chunks);
            break;
        case DataType::UInt32:
            status = fill_chunks<std::uint32_t>(store, field, spec.timestamp,
                                                spec.entity_keys, device,
                                                chunk_rows, chunks);
            break;
        case DataType::UInt64:
            status = fill_chunks<std::uint64_t>(store, field, spec.timestamp,
                                                spec.entity_keys, device,
                                                chunk_rows, chunks);
            break;
        case DataType::UInt8:
            status = fill_chunks<std::uint8_t>(store, field, spec.timestamp,
                                               spec.entity_keys, device,
                                               chunk_rows, chunks);
            break;
    }
    if (!status.ok()) return status;
    out = Column{spec.name, spec.dtype, std::move(chunks), store.size()};
    return ComputeStatus::success();
}

}  // namespace

// --- DataFrameSchema JSON boundary ------------------------------------------

nlohmann::json DataFrameSchema::to_json() const {
    nlohmann::json columns_json = nlohmann::json::array();
    for (const ColumnSchema& column : columns) {
        nlohmann::json entry{{"name", column.name},
                             {"dtype", compute::to_string(column.dtype)}};
        if (!column.field.empty()) entry["field"] = column.field;
        if (column.timestamp) entry["timestamp"] = true;
        columns_json.push_back(std::move(entry));
    }
    return nlohmann::json{{"columns", std::move(columns_json)}};
}

// The DataFrameSchema JSON Schema (draft-07) — the code-owned source of truth;
// schemas/data_frame_schema.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& data_frame_schema_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/data_frame_schema.schema.json",
  "title": "DataFrameSchema",
  "description": "The typed, columnar schema describing how a table's rows are loaded into a zetta DDF DataFrame (ENT-33). Each column maps a numeric JSON field, or a declared ISO-8601 timestamp, to a typed device column.",
  "type": "object",
  "additionalProperties": false,
  "required": ["columns"],
  "properties": {
    "columns": {
      "type": "array",
      "minItems": 1,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "dtype"],
        "properties": {
          "name": {
            "type": "string",
            "minLength": 1,
            "description": "The column's name in the frame (and the row field to read, unless overridden by `field`)."
          },
          "field": {
            "type": "string",
            "minLength": 1,
            "description": "The JSON key in each row's doc to read; defaults to `name` when omitted."
          },
          "dtype": {
            "type": "string",
            "enum": ["float32", "float64", "int32", "int64", "uint32", "uint64", "uint8"],
            "description": "The numeric element type the column holds (the zetta::compute::DataType set)."
          },
          "timestamp": {
            "type": "boolean",
            "description": "When true on a float column, accept ISO-8601 strings or finite epoch numbers and normalize them to epoch seconds."
          }
        },
        "allOf": [
          {
            "if": { "properties": { "timestamp": { "const": true } }, "required": ["timestamp"] },
            "then": { "properties": { "dtype": { "const": "float64" } } }
          }
        ]
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

std::optional<DataFrameSchema> DataFrameSchema::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("columns") || !value["columns"].is_array()) {
        return std::nullopt;
    }
    const nlohmann::json& columns_json = value["columns"];
    if (columns_json.empty()) return std::nullopt;  // minItems: 1

    DataFrameSchema schema;
    std::unordered_set<std::string> seen_names;
    for (const nlohmann::json& entry : columns_json) {
        if (!entry.is_object()) return std::nullopt;
        if (!entry.contains("name") || !entry["name"].is_string()) return std::nullopt;
        if (!entry.contains("dtype") || !entry["dtype"].is_string()) return std::nullopt;

        ColumnSchema column;
        column.name = entry["name"].get<std::string>();
        if (column.name.empty()) return std::nullopt;
        if (!seen_names.insert(column.name).second) return std::nullopt;  // unique

        const std::optional<DataType> dtype =
            data_type_from_string(entry["dtype"].get<std::string>());
        if (!dtype) return std::nullopt;
        column.dtype = *dtype;

        if (entry.contains("field")) {
            if (!entry["field"].is_string()) return std::nullopt;
            column.field = entry["field"].get<std::string>();
            if (column.field.empty()) return std::nullopt;
        }
        if (entry.contains("timestamp")) {
            if (!entry["timestamp"].is_boolean()) return std::nullopt;
            column.timestamp = entry["timestamp"].get<bool>();
            if (column.timestamp && column.dtype != DataType::Float64) {
                return std::nullopt;
            }
        }
        schema.columns.push_back(std::move(column));
    }
    return schema;
}

// --- DataFrame accessors -----------------------------------------------------

std::vector<std::string> DataFrame::column_names() const {
    std::vector<std::string> names;
    names.reserve(columns_.size());
    for (const Column& column : columns_) names.push_back(column.name());
    return names;
}

const Column* DataFrame::column(std::string_view name) const {
    for (const Column& column : columns_) {
        if (column.name() == name) return &column;
    }
    return nullptr;
}

DataFrameSchema DataFrame::schema() const {
    DataFrameSchema schema;
    schema.columns.reserve(columns_.size());
    for (const Column& column : columns_) {
        schema.columns.push_back(ColumnSchema{column.name(), "", column.dtype()});
    }
    return schema;
}

// --- Loader + inference ------------------------------------------------------

DataFrameResult load_data_frame(const zetta::storage::StorageEngine& store,
                                const DataFrameSchema& schema,
                                ComputeDevice& device, std::size_t chunk_rows) {
    if (chunk_rows == 0) {
        return {DataFrame{},
                ComputeStatus::invalid_argument(
                    "load_data_frame: chunk_rows must be greater than zero")};
    }
    std::vector<Column> columns;
    columns.reserve(schema.columns.size());
    for (const ColumnSchema& spec : schema.columns) {
        Column column;
        const ComputeStatus status =
            build_column(store, spec, device, chunk_rows, column);
        if (!status.ok()) return {DataFrame{}, status};
        columns.push_back(std::move(column));
    }
    DataFrame frame(&device, std::move(columns), store.size(), chunk_rows);
    return {std::move(frame), ComputeStatus::success()};
}

DataFrameSchema infer_schema(const zetta::storage::StorageEngine& store) {
    DataFrameSchema schema;
    const std::size_t row_count = store.size();
    if (row_count == 0) return schema;
    const zetta::storage::ParquetLazyPtr first_row = store.at(0);
    if (!first_row) return schema;  // unreadable first row → no inferable schema
    const nlohmann::json& first = *first_row;
    if (!first.is_object()) return schema;

    // Seed candidate fields from the first row's numeric keys, recording whether
    // each is still all-integer. A field drops out the moment any row is missing it,
    // has it non-numeric — so survivors are dense and correctly typed.
    struct Candidate {
        std::string field;
        bool all_integer = true;
        bool alive = true;
    };
    std::vector<Candidate> candidates;
    for (auto it = first.begin(); it != first.end(); ++it) {
        if (it.value().is_number()) {
            candidates.push_back(
                Candidate{it.key(), it.value().is_number_integer(), true});
        }
    }
    for (std::size_t row = 1; row < row_count; ++row) {
        const zetta::storage::ParquetLazyPtr handle = store.at(row);
        // An unreadable row cannot vouch for any candidate: every survivor must
        // be dense across every row, so a failed read drops them all.
        if (!handle) {
            for (Candidate& candidate : candidates) candidate.alive = false;
            break;
        }
        const nlohmann::json& doc = *handle;
        for (Candidate& candidate : candidates) {
            if (!candidate.alive) continue;
            if (!doc.is_object() || !doc.contains(candidate.field) ||
                !doc.at(candidate.field).is_number()) {
                candidate.alive = false;
                continue;
            }
            if (!doc.at(candidate.field).is_number_integer()) {
                candidate.all_integer = false;
            }
        }
    }
    for (const Candidate& candidate : candidates) {
        if (!candidate.alive) continue;
        ColumnSchema column;
        column.name = candidate.field;
        column.dtype = candidate.all_integer ? DataType::Int64 : DataType::Float64;
        schema.columns.push_back(std::move(column));
    }
    return schema;
}

}  // namespace zetta::ddf
