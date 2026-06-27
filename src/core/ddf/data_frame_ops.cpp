#include "zetta/ddf/data_frame_ops.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <utility>

#include "data_frame_internal.hpp"

namespace zetta::ddf {

using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DataType;
using compute::DeviceBuffer;
using compute::SortOrder;
using internal::HostFrame;

namespace internal {

std::optional<std::size_t> HostFrame::column_index(std::string_view name) const {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) return index;
    }
    return std::nullopt;
}

bool HostFrame::same_schema(const HostFrame& other) const {
    return names == other.names && dtypes == other.dtypes;
}

// --- host <-> device staging -------------------------------------------------

ComputeStatus download_frame(const DataFrame& frame, HostFrame& out) {
    out = HostFrame{};
    out.rows = frame.row_count();
    const std::vector<Column>& columns = frame.columns();
    if (columns.empty()) return ComputeStatus::success();

    ComputeDevice* device = frame.device();
    if (device == nullptr) {
        return ComputeStatus::invalid_argument(
            "download_frame: a populated frame has no device");
    }

    out.names.reserve(columns.size());
    out.dtypes.reserve(columns.size());
    out.data.reserve(columns.size());
    for (const Column& column : columns) {
        out.names.push_back(column.name());
        out.dtypes.push_back(column.dtype());
        const std::size_t element_size = compute::byte_size(column.dtype());
        std::vector<std::byte> bytes(out.rows * element_size);
        std::size_t offset = 0;
        for (const DeviceBuffer& chunk : column.chunks()) {
            const std::size_t chunk_bytes = chunk.size_bytes();
            const ComputeStatus status = device->copy_to_host(
                chunk, bytes.data() + offset, chunk_bytes);
            if (!status.ok()) return status;
            offset += chunk_bytes;
        }
        out.data.push_back(std::move(bytes));
    }
    return ComputeStatus::success();
}

DataFrameResult upload_frame(ComputeDevice& device, const HostFrame& host,
                             std::size_t chunk_rows) {
    if (chunk_rows == 0) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "upload_frame: chunk_rows must be greater than zero")};
    }
    std::vector<Column> columns;
    columns.reserve(host.column_count());
    for (std::size_t index = 0; index < host.column_count(); ++index) {
        const DataType dtype = host.dtypes[index];
        const std::size_t element_size = compute::byte_size(dtype);
        const std::byte* source = host.data[index].data();
        std::vector<DeviceBuffer> chunks;
        for (std::size_t start = 0; start < host.rows; start += chunk_rows) {
            const std::size_t length = std::min(chunk_rows, host.rows - start);
            DeviceBuffer buffer = device.allocate(dtype, length);
            if (buffer.empty()) {
                return {DataFrame{}, ComputeStatus::out_of_memory(
                                         "upload_frame: device allocation failed")};
            }
            const ComputeStatus status = device.copy_from_host(
                source + start * element_size, buffer, length * element_size);
            if (!status.ok()) return {DataFrame{}, status};
            chunks.push_back(std::move(buffer));
        }
        columns.push_back(
            Column{host.names[index], dtype, std::move(chunks), host.rows});
    }
    DataFrame frame(&device, std::move(columns), host.rows, chunk_rows);
    return {std::move(frame), ComputeStatus::success()};
}

// --- pure host data movement -------------------------------------------------

HostFrame gather_rows(const HostFrame& host,
                      const std::vector<std::size_t>& permutation) {
    HostFrame out;
    out.names = host.names;
    out.dtypes = host.dtypes;
    out.rows = permutation.size();
    out.data.resize(host.column_count());
    for (std::size_t column = 0; column < host.column_count(); ++column) {
        const std::size_t element_size = compute::byte_size(host.dtypes[column]);
        const std::byte* source = host.data[column].data();
        std::vector<std::byte> bytes(out.rows * element_size);
        for (std::size_t row = 0; row < out.rows; ++row) {
            std::memcpy(bytes.data() + row * element_size,
                        source + permutation[row] * element_size, element_size);
        }
        out.data[column] = std::move(bytes);
    }
    return out;
}

HostFrame slice_rows(const HostFrame& host, std::size_t begin, std::size_t end) {
    begin = std::min(begin, host.rows);
    end = std::min(end, host.rows);
    if (end < begin) end = begin;
    const std::size_t length = end - begin;

    HostFrame out;
    out.names = host.names;
    out.dtypes = host.dtypes;
    out.rows = length;
    out.data.resize(host.column_count());
    for (std::size_t column = 0; column < host.column_count(); ++column) {
        const std::size_t element_size = compute::byte_size(host.dtypes[column]);
        const std::byte* source = host.data[column].data();
        out.data[column].assign(source + begin * element_size,
                                source + end * element_size);
    }
    return out;
}

std::optional<HostFrame> concat_host(const std::vector<const HostFrame*>& frames) {
    if (frames.empty()) return std::nullopt;
    const HostFrame& first = *frames.front();
    for (const HostFrame* frame : frames) {
        if (!first.same_schema(*frame)) return std::nullopt;
    }

    HostFrame out;
    out.names = first.names;
    out.dtypes = first.dtypes;
    out.rows = 0;
    for (const HostFrame* frame : frames) out.rows += frame->rows;
    out.data.resize(first.column_count());
    for (std::size_t column = 0; column < first.column_count(); ++column) {
        std::vector<std::byte>& destination = out.data[column];
        destination.reserve(out.rows * compute::byte_size(first.dtypes[column]));
        for (const HostFrame* frame : frames) {
            const std::vector<std::byte>& bytes = frame->data[column];
            destination.insert(destination.end(), bytes.begin(), bytes.end());
        }
    }
    return out;
}

// Read a typed column blob into doubles. The conversion is monotonic for every
// supported dtype (integer -> double is non-decreasing), so it preserves order for
// sampling / splitter comparison; it is never used to move actual row payloads
// (those keep their exact bytes).
template <class Element>
void append_as_double(const std::vector<std::byte>& bytes, std::size_t rows,
                      std::vector<double>& out) {
    const Element* values = reinterpret_cast<const Element*>(bytes.data());
    out.resize(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        out[row] = static_cast<double>(values[row]);
    }
}

std::optional<std::vector<double>> column_doubles(const HostFrame& host,
                                                  std::string_view name) {
    const std::optional<std::size_t> index = host.column_index(name);
    if (!index) return std::nullopt;
    const std::vector<std::byte>& bytes = host.data[*index];
    std::vector<double> out;
    switch (host.dtypes[*index]) {
        case DataType::Float32: append_as_double<float>(bytes, host.rows, out); break;
        case DataType::Float64: append_as_double<double>(bytes, host.rows, out); break;
        case DataType::Int32:   append_as_double<std::int32_t>(bytes, host.rows, out); break;
        case DataType::Int64:   append_as_double<std::int64_t>(bytes, host.rows, out); break;
        case DataType::UInt32:  append_as_double<std::uint32_t>(bytes, host.rows, out); break;
        case DataType::UInt64:  append_as_double<std::uint64_t>(bytes, host.rows, out); break;
        case DataType::UInt8:   append_as_double<std::uint8_t>(bytes, host.rows, out); break;
    }
    return out;
}

// --- the device-backed local sort -------------------------------------------

ComputeStatus sort_host(ComputeDevice& device, const HostFrame& host,
                        std::string_view key_column, SortOrder order,
                        HostFrame& out) {
    const std::optional<std::size_t> key_index = host.column_index(key_column);
    if (!key_index) {
        return ComputeStatus::invalid_argument(
            "sort: no column named '" + std::string{key_column} + "'");
    }
    if (host.rows <= 1) {
        // A 0/1-row frame is already ordered; an identity gather is the copy.
        std::vector<std::size_t> identity(host.rows);
        std::iota(identity.begin(), identity.end(), std::size_t{0});
        out = gather_rows(host, identity);
        return ComputeStatus::success();
    }

    const DataType key_dtype = host.dtypes[*key_index];
    const std::size_t element_size = compute::byte_size(key_dtype);

    // Upload the key column and an index buffer [0, rows); sort_by_key reorders the
    // indices into the sort permutation (the device does the compare work).
    DeviceBuffer keys = device.allocate(key_dtype, host.rows);
    DeviceBuffer indices = device.allocate(DataType::Int64, host.rows);
    if (keys.empty() || indices.empty()) {
        return ComputeStatus::out_of_memory("sort: device allocation failed");
    }
    if (const ComputeStatus status = device.copy_from_host(
            host.data[*key_index].data(), keys, host.rows * element_size);
        !status.ok()) {
        return status;
    }
    std::vector<std::int64_t> identity(host.rows);
    std::iota(identity.begin(), identity.end(), std::int64_t{0});
    if (const ComputeStatus status = device.copy_from_host(
            identity.data(), indices, host.rows * sizeof(std::int64_t));
        !status.ok()) {
        return status;
    }
    if (const ComputeStatus status = device.sort_by_key(keys, indices, order);
        !status.ok()) {
        return status;
    }
    std::vector<std::int64_t> sorted_indices(host.rows);
    if (const ComputeStatus status = device.copy_to_host(
            indices, sorted_indices.data(), host.rows * sizeof(std::int64_t));
        !status.ok()) {
        return status;
    }

    std::vector<std::size_t> permutation(host.rows);
    for (std::size_t row = 0; row < host.rows; ++row) {
        permutation[row] = static_cast<std::size_t>(sorted_indices[row]);
    }
    out = gather_rows(host, permutation);
    return ComputeStatus::success();
}

}  // namespace internal

// --- public ops --------------------------------------------------------------

DataFrameResult sort(const DataFrame& frame, std::string_view key_column,
                     SortOrder order) {
    HostFrame host;
    if (const ComputeStatus status = internal::download_frame(frame, host);
        !status.ok()) {
        return {DataFrame{}, status};
    }
    if (frame.device() == nullptr) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "sort: an empty frame has no device")};
    }
    HostFrame sorted;
    if (const ComputeStatus status =
            internal::sort_host(*frame.device(), host, key_column, order, sorted);
        !status.ok()) {
        return {DataFrame{}, status};
    }
    const std::size_t chunk_rows =
        frame.chunk_rows() == 0 ? kDefaultChunkRows : frame.chunk_rows();
    return internal::upload_frame(*frame.device(), sorted, chunk_rows);
}

DataFrameResult concat(const std::vector<const DataFrame*>& frames) {
    if (frames.empty()) {
        return {DataFrame{},
                ComputeStatus::invalid_argument("concat: no frames to concatenate")};
    }
    ComputeDevice* device = nullptr;
    std::size_t chunk_rows = kDefaultChunkRows;
    std::vector<HostFrame> staged;
    staged.reserve(frames.size());
    for (const DataFrame* frame : frames) {
        if (frame == nullptr) {
            return {DataFrame{},
                    ComputeStatus::invalid_argument("concat: null frame")};
        }
        if (device == nullptr && frame->device() != nullptr) {
            device = frame->device();
            chunk_rows = frame->chunk_rows() == 0 ? kDefaultChunkRows
                                                  : frame->chunk_rows();
        }
        HostFrame host;
        if (const ComputeStatus status = internal::download_frame(*frame, host);
            !status.ok()) {
            return {DataFrame{}, status};
        }
        staged.push_back(std::move(host));
    }
    if (device == nullptr) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "concat: every frame is empty (no device)")};
    }

    std::vector<const HostFrame*> pointers;
    pointers.reserve(staged.size());
    for (const HostFrame& host : staged) pointers.push_back(&host);
    const std::optional<HostFrame> merged = internal::concat_host(pointers);
    if (!merged) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "concat: frames disagree on schema")};
    }
    return internal::upload_frame(*device, *merged, chunk_rows);
}

DataFrameResult slice(const DataFrame& frame, std::size_t row_begin,
                      std::size_t row_end) {
    HostFrame host;
    if (const ComputeStatus status = internal::download_frame(frame, host);
        !status.ok()) {
        return {DataFrame{}, status};
    }
    if (frame.device() == nullptr) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "slice: an empty frame has no device")};
    }
    const HostFrame cut = internal::slice_rows(host, row_begin, row_end);
    const std::size_t chunk_rows =
        frame.chunk_rows() == 0 ? kDefaultChunkRows : frame.chunk_rows();
    return internal::upload_frame(*frame.device(), cut, chunk_rows);
}

}  // namespace zetta::ddf
