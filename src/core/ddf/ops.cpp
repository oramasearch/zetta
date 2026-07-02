#include "zetta/ddf/ops.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zetta::ddf {

using compute::BinaryOp;
using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DataType;
using compute::DeviceBuffer;
using compute::UnaryOp;

namespace {

// --- type dispatch -----------------------------------------------------------
// A column's element type is a runtime DataType tag; this turns it into a static
// C++ type for a templated visitor, mirroring CpuDevice's visit_numeric.

template <class Visitor>
ComputeStatus visit_numeric(DataType dtype, Visitor&& visitor) {
    switch (dtype) {
        case DataType::Float32: return visitor.template operator()<float>();
        case DataType::Float64: return visitor.template operator()<double>();
        case DataType::Int32:   return visitor.template operator()<std::int32_t>();
        case DataType::Int64:   return visitor.template operator()<std::int64_t>();
        case DataType::UInt32:  return visitor.template operator()<std::uint32_t>();
        case DataType::UInt64:  return visitor.template operator()<std::uint64_t>();
        case DataType::UInt8:   return visitor.template operator()<std::uint8_t>();
    }
    return ComputeStatus::invalid_argument("ddf: unknown data type");
}

// --- op-name tables (the JSON boundary for project's computed columns) --------

std::string_view unary_op_name(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate:      return "negate";
        case UnaryOp::Absolute:    return "absolute";
        case UnaryOp::Square:      return "square";
        case UnaryOp::SquareRoot:  return "sqrt";
        case UnaryOp::Exponential: return "exp";
        case UnaryOp::Logarithm:   return "log";
    }
    return "unknown";
}

std::optional<UnaryOp> unary_op_from_string(std::string_view text) {
    if (text == "negate")   return UnaryOp::Negate;
    if (text == "absolute") return UnaryOp::Absolute;
    if (text == "square")   return UnaryOp::Square;
    if (text == "sqrt")     return UnaryOp::SquareRoot;
    if (text == "exp")      return UnaryOp::Exponential;
    if (text == "log")      return UnaryOp::Logarithm;
    return std::nullopt;
}

std::string_view binary_op_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:      return "add";
        case BinaryOp::Subtract: return "subtract";
        case BinaryOp::Multiply: return "multiply";
        case BinaryOp::Divide:   return "divide";
        case BinaryOp::Maximum:  return "maximum";
        case BinaryOp::Minimum:  return "minimum";
    }
    return "unknown";
}

std::optional<BinaryOp> binary_op_from_string(std::string_view text) {
    if (text == "add")      return BinaryOp::Add;
    if (text == "subtract") return BinaryOp::Subtract;
    if (text == "multiply") return BinaryOp::Multiply;
    if (text == "divide")   return BinaryOp::Divide;
    if (text == "maximum")  return BinaryOp::Maximum;
    if (text == "minimum")  return BinaryOp::Minimum;
    return std::nullopt;
}

std::optional<Comparison> comparison_from_string(std::string_view text) {
    if (text == "eq") return Comparison::Equal;
    if (text == "ne") return Comparison::NotEqual;
    if (text == "lt") return Comparison::Less;
    if (text == "le") return Comparison::LessEqual;
    if (text == "gt") return Comparison::Greater;
    if (text == "ge") return Comparison::GreaterEqual;
    return std::nullopt;
}

// --- filter helpers ----------------------------------------------------------

bool compare(double lhs, double rhs, Comparison comparison) {
    switch (comparison) {
        case Comparison::Equal:        return lhs == rhs;
        case Comparison::NotEqual:     return lhs != rhs;
        case Comparison::Less:         return lhs < rhs;
        case Comparison::LessEqual:    return lhs <= rhs;
        case Comparison::Greater:      return lhs > rhs;
        case Comparison::GreaterEqual: return lhs >= rhs;
    }
    return false;
}

// Read a whole column back to host as doubles, concatenating its chunks in row
// order (the predicate is evaluated host-side; see the header). Widens any numeric
// dtype to double for a uniform comparison.
ComputeStatus read_column_as_doubles(ComputeDevice& device, const Column& column,
                                     std::vector<double>& out) {
    out.clear();
    out.reserve(column.row_count());
    for (const DeviceBuffer& chunk : column.chunks()) {
        const std::size_t count = chunk.element_count();
        const ComputeStatus status =
            visit_numeric(column.dtype(), [&]<class Element>() -> ComputeStatus {
                std::vector<Element> host(count);
                const ComputeStatus copy = device.copy_to_host(
                    chunk, host.data(), count * sizeof(Element));
                if (!copy.ok()) return copy;
                for (const Element value : host)
                    out.push_back(static_cast<double>(value));
                return ComputeStatus::success();
            });
        if (!status.ok()) return status;
    }
    return ComputeStatus::success();
}

// Build one output column holding the `selected` global rows of `column`, gathered
// on-device per source chunk and re-chunked into regular `chunk_rows` buffers.
// dtype-agnostic — gather and the transfers move raw element bytes.
ComputeStatus gather_selected(ComputeDevice& device, const Column& column,
                              const std::vector<std::int64_t>& selected,
                              std::size_t chunk_rows,
                              std::vector<DeviceBuffer>& out_chunks) {
    const DataType dtype = column.dtype();
    const std::size_t element_size = compute::byte_size(dtype);
    const std::size_t out_rows = selected.size();

    // The selected rows' bytes, in row order, built by gathering each source chunk.
    std::vector<std::byte> compacted;
    compacted.reserve(out_rows * element_size);

    std::size_t cursor = 0;  // walk `selected` (ascending) chunk by chunk
    const std::size_t chunk_count = column.chunk_count();
    for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const std::size_t chunk_begin = chunk_index * chunk_rows;
        const DeviceBuffer& source_chunk = column.chunk(chunk_index);
        const std::size_t chunk_end = chunk_begin + source_chunk.element_count();

        std::vector<std::int64_t> local;  // selected indices relative to the chunk
        while (cursor < selected.size() &&
               static_cast<std::size_t>(selected[cursor]) < chunk_end) {
            local.push_back(selected[cursor] -
                            static_cast<std::int64_t>(chunk_begin));
            ++cursor;
        }
        if (local.empty()) continue;

        DeviceBuffer indices =
            device.upload<std::int64_t>(std::span<const std::int64_t>(local));
        if (indices.empty()) {
            return ComputeStatus::out_of_memory(
                "filter: failed to allocate gather indices for column '" +
                column.name() + "'");
        }
        DeviceBuffer gathered = device.allocate(dtype, local.size());
        if (gathered.empty()) {
            return ComputeStatus::out_of_memory(
                "filter: failed to allocate gather output for column '" +
                column.name() + "'");
        }
        if (const ComputeStatus status =
                device.gather(source_chunk, indices, gathered);
            !status.ok()) {
            return status;
        }
        const std::size_t offset = compacted.size();
        compacted.resize(offset + local.size() * element_size);
        if (const ComputeStatus status = device.copy_to_host(
                gathered, compacted.data() + offset, local.size() * element_size);
            !status.ok()) {
            return status;
        }
    }

    // Re-chunk the compacted bytes into regular output chunks.
    for (std::size_t start = 0; start < out_rows; start += chunk_rows) {
        const std::size_t length = std::min(chunk_rows, out_rows - start);
        DeviceBuffer buffer = device.allocate(dtype, length);
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "filter: failed to allocate output chunk for column '" +
                column.name() + "'");
        }
        if (const ComputeStatus status = device.copy_from_host(
                compacted.data() + start * element_size, buffer,
                length * element_size);
            !status.ok()) {
            return status;
        }
        out_chunks.push_back(std::move(buffer));
    }
    return ComputeStatus::success();
}

// --- project helpers ---------------------------------------------------------

ComputeStatus build_passthrough(ComputeDevice& device, const DataFrame& frame,
                                const ProjectColumn& spec, Column& out) {
    const Column* source = frame.column(spec.source);
    if (source == nullptr) {
        return ComputeStatus::invalid_argument(
            "project: no column named '" + spec.source + "' for output '" +
            spec.name + "'");
    }
    const std::size_t element_size = compute::byte_size(source->dtype());
    std::vector<DeviceBuffer> chunks;
    chunks.reserve(source->chunk_count());
    std::vector<std::byte> staging;
    for (const DeviceBuffer& chunk : source->chunks()) {
        const std::size_t bytes = chunk.element_count() * element_size;
        staging.resize(bytes);
        if (const ComputeStatus status =
                device.copy_to_host(chunk, staging.data(), bytes);
            !status.ok()) {
            return status;
        }
        DeviceBuffer buffer =
            device.allocate(source->dtype(), chunk.element_count());
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "project: failed to allocate output column '" + spec.name + "'");
        }
        if (const ComputeStatus status =
                device.copy_from_host(staging.data(), buffer, bytes);
            !status.ok()) {
            return status;
        }
        chunks.push_back(std::move(buffer));
    }
    out = Column{spec.name, source->dtype(), std::move(chunks),
                 source->row_count()};
    return ComputeStatus::success();
}

ComputeStatus build_unary(ComputeDevice& device, const DataFrame& frame,
                          const ProjectColumn& spec, Column& out) {
    const Column* source = frame.column(spec.source);
    if (source == nullptr) {
        return ComputeStatus::invalid_argument(
            "project: no column named '" + spec.source + "' for output '" +
            spec.name + "'");
    }
    std::vector<DeviceBuffer> chunks;
    chunks.reserve(source->chunk_count());
    for (const DeviceBuffer& chunk : source->chunks()) {
        DeviceBuffer buffer =
            device.allocate(source->dtype(), chunk.element_count());
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "project: failed to allocate output column '" + spec.name + "'");
        }
        if (const ComputeStatus status =
                device.map(spec.unary_op, chunk, buffer);
            !status.ok()) {
            return status;
        }
        chunks.push_back(std::move(buffer));
    }
    out = Column{spec.name, source->dtype(), std::move(chunks),
                 source->row_count()};
    return ComputeStatus::success();
}

ComputeStatus build_binary(ComputeDevice& device, const DataFrame& frame,
                           const ProjectColumn& spec, Column& out) {
    const Column* left = frame.column(spec.source);
    const Column* right = frame.column(spec.right);
    if (left == nullptr) {
        return ComputeStatus::invalid_argument(
            "project: no column named '" + spec.source + "' for output '" +
            spec.name + "'");
    }
    if (right == nullptr) {
        return ComputeStatus::invalid_argument(
            "project: no column named '" + spec.right + "' for output '" +
            spec.name + "'");
    }
    // Every column in a frame shares the chunk layout, so this is defensive.
    if (left->chunk_count() != right->chunk_count()) {
        return ComputeStatus::invalid_argument(
            "project: operands of '" + spec.name + "' have mismatched chunking");
    }
    std::vector<DeviceBuffer> chunks;
    chunks.reserve(left->chunk_count());
    for (std::size_t chunk_index = 0; chunk_index < left->chunk_count();
         ++chunk_index) {
        const DeviceBuffer& left_chunk = left->chunk(chunk_index);
        const DeviceBuffer& right_chunk = right->chunk(chunk_index);
        DeviceBuffer buffer =
            device.allocate(left->dtype(), left_chunk.element_count());
        if (buffer.empty()) {
            return ComputeStatus::out_of_memory(
                "project: failed to allocate output column '" + spec.name + "'");
        }
        // elementwise validates that both operands and the output share a dtype,
        // so a binary op over columns of different dtypes is rejected here.
        if (const ComputeStatus status = device.elementwise(
                spec.binary_op, left_chunk, right_chunk, buffer);
            !status.ok()) {
            return status;
        }
        chunks.push_back(std::move(buffer));
    }
    out = Column{spec.name, left->dtype(), std::move(chunks), left->row_count()};
    return ComputeStatus::success();
}

}  // namespace

// --- scan --------------------------------------------------------------------

DataFrameResult scan(const zetta::storage::StorageEngine& store, ComputeDevice& device,
                     std::size_t chunk_rows) {
    return load_data_frame(store, infer_schema(store), device, chunk_rows);
}

DataFrameResult scan(const zetta::storage::StorageEngine& store, const DataFrameSchema& schema,
                     ComputeDevice& device, std::size_t chunk_rows) {
    return load_data_frame(store, schema, device, chunk_rows);
}

// --- filter ------------------------------------------------------------------

std::string_view to_string(Comparison comparison) {
    switch (comparison) {
        case Comparison::Equal:        return "eq";
        case Comparison::NotEqual:     return "ne";
        case Comparison::Less:         return "lt";
        case Comparison::LessEqual:    return "le";
        case Comparison::Greater:      return "gt";
        case Comparison::GreaterEqual: return "ge";
    }
    return "unknown";
}

nlohmann::json FilterPredicate::to_json() const {
    return nlohmann::json{{"column", column},
                          {"comparison", to_string(comparison)},
                          {"value", value}};
}

std::optional<FilterPredicate> FilterPredicate::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("column") || !value["column"].is_string()) {
        return std::nullopt;
    }
    if (!value.contains("comparison") || !value["comparison"].is_string()) {
        return std::nullopt;
    }
    if (!value.contains("value") || !value["value"].is_number()) {
        return std::nullopt;
    }
    FilterPredicate predicate;
    predicate.column = value["column"].get<std::string>();
    if (predicate.column.empty()) return std::nullopt;
    const std::optional<Comparison> comparison =
        comparison_from_string(value["comparison"].get<std::string>());
    if (!comparison) return std::nullopt;
    predicate.comparison = *comparison;
    predicate.value = value["value"].get<double>();
    return predicate;
}

DataFrameResult filter(const DataFrame& frame, const FilterPredicate& predicate) {
    ComputeDevice* device = frame.device();
    if (device == nullptr) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "filter: an empty frame has no device")};
    }
    const Column* key = frame.column(predicate.column);
    if (key == nullptr) {
        return {DataFrame{},
                ComputeStatus::invalid_argument(
                    "filter: no column named '" + predicate.column + "'")};
    }

    // Evaluate the predicate host-side into the selected global row indices.
    std::vector<double> values;
    if (const ComputeStatus status =
            read_column_as_doubles(*device, *key, values);
        !status.ok()) {
        return {DataFrame{}, status};
    }
    std::vector<std::int64_t> selected;  // ascending global row indices
    selected.reserve(values.size());
    for (std::size_t row = 0; row < values.size(); ++row) {
        if (compare(values[row], predicate.value, predicate.comparison)) {
            selected.push_back(static_cast<std::int64_t>(row));
        }
    }

    const std::size_t out_rows = selected.size();
    const std::size_t chunk_rows = frame.chunk_rows();

    std::vector<Column> out_columns;
    out_columns.reserve(frame.column_count());
    for (const Column& column : frame.columns()) {
        std::vector<DeviceBuffer> out_chunks;
        if (const ComputeStatus status =
                gather_selected(*device, column, selected, chunk_rows, out_chunks);
            !status.ok()) {
            return {DataFrame{}, status};
        }
        out_columns.push_back(Column{column.name(), column.dtype(),
                                     std::move(out_chunks), out_rows});
    }

    DataFrame out(device, std::move(out_columns), out_rows, chunk_rows);
    return {std::move(out), ComputeStatus::success()};
}

// --- project -----------------------------------------------------------------

ProjectColumn ProjectColumn::passthrough(std::string name, std::string source) {
    ProjectColumn column;
    column.name = std::move(name);
    column.kind = Kind::Passthrough;
    column.source = std::move(source);
    return column;
}

ProjectColumn ProjectColumn::unary(std::string name, UnaryOp op,
                                   std::string source) {
    ProjectColumn column;
    column.name = std::move(name);
    column.kind = Kind::Unary;
    column.unary_op = op;
    column.source = std::move(source);
    return column;
}

ProjectColumn ProjectColumn::binary(std::string name, BinaryOp op,
                                    std::string left, std::string right) {
    ProjectColumn column;
    column.name = std::move(name);
    column.kind = Kind::Binary;
    column.binary_op = op;
    column.source = std::move(left);
    column.right = std::move(right);
    return column;
}

nlohmann::json Projection::to_json() const {
    nlohmann::json columns_json = nlohmann::json::array();
    for (const ProjectColumn& column : columns) {
        nlohmann::json entry{{"name", column.name}};
        switch (column.kind) {
            case ProjectColumn::Kind::Passthrough:
                entry["source"] = column.source;
                break;
            case ProjectColumn::Kind::Unary:
                entry["source"] = column.source;
                entry["unary"] = unary_op_name(column.unary_op);
                break;
            case ProjectColumn::Kind::Binary:
                entry["binary"] = binary_op_name(column.binary_op);
                entry["left"] = column.source;
                entry["right"] = column.right;
                break;
        }
        columns_json.push_back(std::move(entry));
    }
    return nlohmann::json{{"columns", std::move(columns_json)}};
}

std::optional<Projection> Projection::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("columns") || !value["columns"].is_array()) {
        return std::nullopt;
    }
    const nlohmann::json& columns_json = value["columns"];
    if (columns_json.empty()) return std::nullopt;  // minItems: 1

    Projection projection;
    std::unordered_set<std::string> seen_names;
    for (const nlohmann::json& entry : columns_json) {
        if (!entry.is_object()) return std::nullopt;
        if (!entry.contains("name") || !entry["name"].is_string()) {
            return std::nullopt;
        }
        const std::string name = entry["name"].get<std::string>();
        if (name.empty()) return std::nullopt;
        if (!seen_names.insert(name).second) return std::nullopt;  // unique

        const bool has_unary = entry.contains("unary");
        const bool has_binary = entry.contains("binary");
        const bool has_source = entry.contains("source");
        const bool has_left = entry.contains("left");
        const bool has_right = entry.contains("right");

        if (has_binary) {
            // Binary: left + right + binary; no unary / source.
            if (has_unary || has_source) return std::nullopt;
            if (!has_left || !has_right) return std::nullopt;
            if (!entry["binary"].is_string() || !entry["left"].is_string() ||
                !entry["right"].is_string()) {
                return std::nullopt;
            }
            const std::optional<BinaryOp> op =
                binary_op_from_string(entry["binary"].get<std::string>());
            if (!op) return std::nullopt;
            const std::string left = entry["left"].get<std::string>();
            const std::string right = entry["right"].get<std::string>();
            if (left.empty() || right.empty()) return std::nullopt;
            projection.columns.push_back(
                ProjectColumn::binary(name, *op, left, right));
        } else if (has_unary) {
            // Unary: source + unary; no left / right.
            if (has_left || has_right || !has_source) return std::nullopt;
            if (!entry["unary"].is_string() || !entry["source"].is_string()) {
                return std::nullopt;
            }
            const std::optional<UnaryOp> op =
                unary_op_from_string(entry["unary"].get<std::string>());
            if (!op) return std::nullopt;
            const std::string source = entry["source"].get<std::string>();
            if (source.empty()) return std::nullopt;
            projection.columns.push_back(
                ProjectColumn::unary(name, *op, source));
        } else {
            // Passthrough: source only.
            if (has_left || has_right) return std::nullopt;
            if (!has_source || !entry["source"].is_string()) return std::nullopt;
            const std::string source = entry["source"].get<std::string>();
            if (source.empty()) return std::nullopt;
            projection.columns.push_back(
                ProjectColumn::passthrough(name, source));
        }
    }
    return projection;
}

DataFrameResult project(const DataFrame& frame, const Projection& projection) {
    ComputeDevice* device = frame.device();
    if (device == nullptr) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "project: an empty frame has no device")};
    }
    if (projection.empty()) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "project: projection has no columns")};
    }

    std::vector<Column> out_columns;
    out_columns.reserve(projection.column_count());
    for (const ProjectColumn& spec : projection.columns) {
        Column column;
        ComputeStatus status;
        switch (spec.kind) {
            case ProjectColumn::Kind::Passthrough:
                status = build_passthrough(*device, frame, spec, column);
                break;
            case ProjectColumn::Kind::Unary:
                status = build_unary(*device, frame, spec, column);
                break;
            case ProjectColumn::Kind::Binary:
                status = build_binary(*device, frame, spec, column);
                break;
        }
        if (!status.ok()) return {DataFrame{}, status};
        out_columns.push_back(std::move(column));
    }

    DataFrame out(device, std::move(out_columns), frame.row_count(),
                  frame.chunk_rows());
    return {std::move(out), ComputeStatus::success()};
}

}  // namespace zetta::ddf
