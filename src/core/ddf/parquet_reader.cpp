// SPDX-License-Identifier: MIT

#include "zetta/ddf/parquet_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json-schema.hpp>

#include "zetta/parquet/column_decoder.hpp"
#include "zetta/parquet/metadata.hpp"

namespace zetta::ddf {

using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DataType;
using compute::DeviceBuffer;
using parquet::DecodedColumn;
using parquet::PhysicalType;

namespace {

// Assign an integer source value into a typed host element, validating at the
// boundary: a floating element widens it; an integer element range-checks it (and
// an unsigned element rejects a negative). Mirrors load_data_frame's `extract`.
template <class Element>
ComputeStatus assign_integer(std::int64_t value, const std::string& where,
                             Element& out) {
    if constexpr (std::is_floating_point_v<Element>) {
        out = static_cast<Element>(value);
        return ComputeStatus::success();
    } else {
        const std::string dtype_name{compute::to_string(compute::data_type_of<Element>)};
        if constexpr (std::is_signed_v<Element>) {
            if (value < static_cast<std::int64_t>(std::numeric_limits<Element>::min()) ||
                value > static_cast<std::int64_t>(std::numeric_limits<Element>::max())) {
                return ComputeStatus::invalid_argument(where + " is out of range for " +
                                                       dtype_name);
            }
            out = static_cast<Element>(value);
        } else {
            if (value < 0) {
                return ComputeStatus::invalid_argument(
                    where + " is negative but the column is unsigned (" + dtype_name + ")");
            }
            if (static_cast<std::uint64_t>(value) >
                static_cast<std::uint64_t>(std::numeric_limits<Element>::max())) {
                return ComputeStatus::invalid_argument(where + " is out of range for " +
                                                       dtype_name);
            }
            out = static_cast<Element>(value);
        }
        return ComputeStatus::success();
    }
}

// Assign a real source value into a typed host element: a floating element takes
// it; an integer element rejects it (an integer column cannot hold a real number,
// matching load_data_frame's real-into-integer rejection).
template <class Element>
ComputeStatus assign_real(double value, const std::string& where, Element& out) {
    if constexpr (std::is_floating_point_v<Element>) {
        out = static_cast<Element>(value);
        return ComputeStatus::success();
    } else {
        const std::string dtype_name{compute::to_string(compute::data_type_of<Element>)};
        return ComputeStatus::invalid_argument(
            where + " is a real number but the column is integer (" + dtype_name + ")");
    }
}

// Convert a decoded leaf column into a dense host vector of the target element
// type. A DataFrame column is dense, so a null in the rows read fails; a
// byte_array (string) column cannot become a numeric column.
template <class Element>
ComputeStatus to_host_column(const DecodedColumn& column, const std::string& name,
                             std::vector<Element>& out) {
    const std::string where = "load_parquet_data_frame: column '" + name + "'";
    if (column.valid_count() != column.num_rows) {
        return ComputeStatus::invalid_argument(
            where + " has null values but a DataFrame column must be dense");
    }
    const std::size_t row_count = static_cast<std::size_t>(column.num_rows);
    out.resize(row_count);
    const parquet::ColumnValues& source = column.values;
    for (std::size_t row = 0; row < row_count; ++row) {
        const std::string at = where + " row " + std::to_string(row);
        ComputeStatus status;
        switch (column.type) {
            case PhysicalType::Boolean:
                status = assign_integer<Element>(
                    static_cast<std::int64_t>(source.bool_values[row]), at, out[row]);
                break;
            case PhysicalType::Int32:
                status = assign_integer<Element>(
                    static_cast<std::int64_t>(source.int32_values[row]), at, out[row]);
                break;
            case PhysicalType::Int64:
                status = assign_integer<Element>(source.int64_values[row], at, out[row]);
                break;
            case PhysicalType::Float:
                status = assign_real<Element>(
                    static_cast<double>(source.float_values[row]), at, out[row]);
                break;
            case PhysicalType::Double:
                status = assign_real<Element>(source.double_values[row], at, out[row]);
                break;
            case PhysicalType::ByteArray:
            case PhysicalType::FixedLenByteArray:
                return ComputeStatus::invalid_argument(
                    where + " is a byte_array and cannot load into a numeric column");
            default:
                return ComputeStatus::invalid_argument(where +
                                                       " has an unsupported physical type");
        }
        if (!status.ok()) {
            out.clear();
            return status;
        }
    }
    return ComputeStatus::success();
}

// Upload a dense host vector into row-aligned device buffers of `chunk_rows` rows.
template <class Element>
ComputeStatus upload_chunks(const std::vector<Element>& host, const std::string& name,
                            ComputeDevice& device, std::size_t chunk_rows,
                            std::vector<DeviceBuffer>& out) {
    const std::size_t row_count = host.size();
    for (std::size_t start = 0; start < row_count; start += chunk_rows) {
        const std::size_t length = std::min(chunk_rows, row_count - start);
        DeviceBuffer buffer = device.allocate(compute::data_type_of<Element>, length);
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "load_parquet_data_frame: failed to allocate a device buffer for column '" +
                name + "'");
        }
        const ComputeStatus status =
            device.copy_from_host(host.data() + start, buffer, length * sizeof(Element));
        if (!status.ok()) return status;
        out.push_back(std::move(buffer));
    }
    return ComputeStatus::success();
}

// Decode one leaf column and build the typed frame Column for it (the dtype
// dispatch over the convert + upload steps).
template <class Element>
ComputeStatus build_typed_column(const DecodedColumn& decoded, const ColumnSchema& spec,
                                 ComputeDevice& device, std::size_t chunk_rows,
                                 Column& out) {
    std::vector<Element> host;
    ComputeStatus status = to_host_column<Element>(decoded, spec.name, host);
    if (!status.ok()) return status;
    std::vector<DeviceBuffer> chunks;
    status = upload_chunks<Element>(host, spec.name, device, chunk_rows, chunks);
    if (!status.ok()) return status;
    out = Column{spec.name, spec.dtype, std::move(chunks), host.size()};
    return ComputeStatus::success();
}

ComputeStatus build_column(const DecodedColumn& decoded, const ColumnSchema& spec,
                           ComputeDevice& device, std::size_t chunk_rows, Column& out) {
    switch (spec.dtype) {
        case DataType::Float32:
            return build_typed_column<float>(decoded, spec, device, chunk_rows, out);
        case DataType::Float64:
            return build_typed_column<double>(decoded, spec, device, chunk_rows, out);
        case DataType::Int32:
            return build_typed_column<std::int32_t>(decoded, spec, device, chunk_rows, out);
        case DataType::Int64:
            return build_typed_column<std::int64_t>(decoded, spec, device, chunk_rows, out);
        case DataType::UInt32:
            return build_typed_column<std::uint32_t>(decoded, spec, device, chunk_rows, out);
        case DataType::UInt64:
            return build_typed_column<std::uint64_t>(decoded, spec, device, chunk_rows, out);
        case DataType::UInt8:
            return build_typed_column<std::uint8_t>(decoded, spec, device, chunk_rows, out);
    }
    return ComputeStatus::invalid_argument("load_parquet_data_frame: unknown dtype");
}

// The natural DataFrame dtype for a numeric leaf's physical type (std::nullopt for
// a non-numeric leaf, which schema inference skips).
std::optional<DataType> numeric_dtype(PhysicalType type) {
    switch (type) {
        case PhysicalType::Int32:   return DataType::Int32;
        case PhysicalType::Int64:   return DataType::Int64;
        case PhysicalType::Float:   return DataType::Float32;
        case PhysicalType::Double:  return DataType::Float64;
        case PhysicalType::Boolean: return DataType::UInt8;
        default:                    return std::nullopt;
    }
}

// --- embedded JSON Schema for a materialized-DataFrame golden ----------------

const nlohmann::json& data_frame_schema_doc() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "Parquet materialized DataFrame",
      "type": "object",
      "additionalProperties": false,
      "required": ["num_rows", "columns"],
      "properties": {
        "num_rows": { "type": "integer", "minimum": 0 },
        "columns": {
          "type": "array",
          "items": {
            "type": "object",
            "additionalProperties": false,
            "required": ["name", "dtype", "values"],
            "properties": {
              "name": { "type": "string", "minLength": 1 },
              "dtype": {
                "type": "string",
                "enum": ["float32", "float64", "int32", "int64",
                         "uint32", "uint64", "uint8"]
              },
              "values": {
                "type": "array",
                "items": { "type": ["integer", "number"] }
              }
            }
          }
        }
      }
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& data_frame_validator() {
    static nlohmann::json_schema::json_validator validator(data_frame_schema_doc());
    return validator;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

DataFrameResult load_parquet_data_frame(std::string_view file_bytes,
                                        const DataFrameSchema& schema,
                                        ComputeDevice& device, std::size_t chunk_rows) {
    if (chunk_rows == 0) {
        return {DataFrame{},
                ComputeStatus::invalid_argument(
                    "load_parquet_data_frame: chunk_rows must be greater than zero")};
    }

    const parquet::ParquetMetadataResult metadata =
        parquet::parse_parquet_metadata(file_bytes);
    if (!metadata.ok()) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "load_parquet_data_frame: " + metadata.error)};
    }

    // Map each leaf path to its column index, so a projection resolves by name —
    // and only the selected indices are decoded (column projection).
    const std::vector<parquet::LeafColumn> leaves = metadata.metadata.leaf_columns();
    std::unordered_map<std::string, std::size_t> index_of;
    index_of.reserve(leaves.size());
    for (std::size_t index = 0; index < leaves.size(); ++index) {
        index_of.emplace(leaves[index].path, index);
    }

    std::vector<Column> columns;
    columns.reserve(schema.columns.size());
    for (const ColumnSchema& spec : schema.columns) {
        const std::string& field = spec.source_field();
        const auto found = index_of.find(field);
        if (found == index_of.end()) {
            return {DataFrame{}, ComputeStatus::invalid_argument(
                                     "load_parquet_data_frame: no column '" + field +
                                     "' in the parquet file")};
        }
        const parquet::DecodedColumnResult decoded =
            parquet::decode_column(file_bytes, metadata.metadata, found->second);
        if (!decoded.ok()) {
            return {DataFrame{}, ComputeStatus::invalid_argument(
                                     "load_parquet_data_frame: column '" + field +
                                     "': " + decoded.error)};
        }
        Column column;
        const ComputeStatus status =
            build_column(decoded.column, spec, device, chunk_rows, column);
        if (!status.ok()) return {DataFrame{}, status};
        columns.push_back(std::move(column));
    }

    const std::size_t row_count = static_cast<std::size_t>(metadata.metadata.num_rows);
    DataFrame frame(&device, std::move(columns), row_count, chunk_rows);
    return {std::move(frame), ComputeStatus::success()};
}

ParquetSchemaResult infer_parquet_schema(std::string_view file_bytes) {
    ParquetSchemaResult result;
    const parquet::ParquetMetadataResult metadata =
        parquet::parse_parquet_metadata(file_bytes);
    if (!metadata.ok()) {
        result.error = metadata.error;
        return result;
    }
    for (const parquet::LeafColumn& leaf : metadata.metadata.leaf_columns()) {
        const std::optional<DataType> dtype = numeric_dtype(leaf.type);
        if (!dtype) continue;  // skip non-numeric (byte_array) leaves
        ColumnSchema column;
        column.name = leaf.path;
        column.dtype = *dtype;
        result.schema.columns.push_back(std::move(column));
    }
    return result;
}

const nlohmann::json& parquet_data_frame_schema() { return data_frame_schema_doc(); }

std::string validate_parquet_data_frame_json(const nlohmann::json& document) {
    collecting_error_handler handler;
    data_frame_validator().validate(document, handler);
    return handler.failed ? handler.reason : std::string();
}

}  // namespace zetta::ddf
