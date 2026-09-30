#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/device_buffer.hpp"
#include "zetta/storage/engine.hpp"

// The DDF (distributed dataframe) core — a typed, columnar `DataFrame` over the
// existing storage engine / tables (ARC-12 / GLO-11, entity ENT-33; part of the
// native distributed accelerated-compute pass for UC-1 churn, #264 / #380).
//
// A table today is untyped JSON rows (`JsonlStore`'s `{_zid, doc}` lines). The
// DataFrame is the *substrate every DDF op operates on*: it reads a table's rows
// into typed numeric **columns**, **chunked** by row range, with each chunk backed
// by a `zetta::compute::DeviceBuffer` — so the same CPU/CUDA operators that back
// the rest of the compute pass (map/reduce/histogram/gemm/sort/…) run directly
// over a frame's columns on whichever device it was loaded onto.
//
// The element-type set is `zetta::compute::DataType` — numeric only (float / int /
// uint). A DataFrame is therefore a *numeric* view of a table: the loader projects
// the chosen numeric fields of each row into columns, validating each value at the
// boundary (missing field / non-number / out-of-range -> a failing ComputeStatus,
// never a throw across the seam, matching zetta-core's no-exceptions convention).
//
// Scope of this slice: the type, the table->frame loader, schema inference, and the
// JSON boundary for the schema config. No Node CRUD, no SQL surface, no replication
// — like the Provisioner seam, those are later slices once DDF *ops* land.
namespace zetta::ddf {

// One column's mapping from a row field to a typed device column.
struct ColumnSchema {
    std::string name;                                       // column name in the frame
    std::string field;                                      // JSON key in each row's
                                                            //   doc (empty => use name)
    compute::DataType dtype = compute::DataType::Float64;   // the column's element type
    bool timestamp = false;                                 // accept ISO-8601 or epoch

    // The row field actually read: `field` when set, otherwise `name`.
    const std::string& source_field() const noexcept {
        return field.empty() ? name : field;
    }
};

// The set of columns to project a table into, with the JSON boundary the schema
// config crosses. Mirrors schemas/data_frame_schema.schema.json.
struct DataFrameSchema {
    std::vector<ColumnSchema> columns;

    bool empty() const noexcept { return columns.empty(); }
    std::size_t column_count() const noexcept { return columns.size(); }

    // The JSON form:
    // { "columns": [ { "name", "field"?, "dtype", "timestamp"? }, ... ] }.
    nlohmann::json to_json() const;

    // Parse + validate a schema config at the boundary; the non-throwing inverse of
    // to_json (std::optional, like ir::from_json / NodeCapability::from_json).
    // std::nullopt unless `value` is an object with a non-empty `columns` array of
    // objects each having a non-empty string `name`, a known `dtype`, and (optional)
    // non-empty string `field`, with unique column names.
    static std::optional<DataFrameSchema> from_json(const nlohmann::json& value);
};

// The DataFrameSchema JSON Schema (draft-07) — the code-owned source of truth;
// schemas/data_frame_schema.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& data_frame_schema_schema();

// A typed column: a sequence of row-aligned chunks, each a DeviceBuffer of the
// column's dtype on the frame's device. Move-only (DeviceBuffer is move-only).
//
// Chunks are contiguous over the row range: chunk i holds rows
// [i*chunk_rows, min((i+1)*chunk_rows, row_count)). Every column in a frame shares
// the same chunk layout, so a chunk index selects the same rows across columns —
// the property DDF ops rely on to operate chunk-by-chunk.
class Column {
public:
    Column() = default;
    Column(std::string name, compute::DataType dtype,
           std::vector<compute::DeviceBuffer> chunks, std::size_t row_count)
        : name_(std::move(name)),
          dtype_(dtype),
          chunks_(std::move(chunks)),
          row_count_(row_count) {}

    Column(Column&&) noexcept = default;
    Column& operator=(Column&&) noexcept = default;
    Column(const Column&) = delete;
    Column& operator=(const Column&) = delete;

    const std::string& name() const noexcept { return name_; }
    compute::DataType dtype() const noexcept { return dtype_; }
    std::size_t row_count() const noexcept { return row_count_; }

    std::size_t chunk_count() const noexcept { return chunks_.size(); }
    const std::vector<compute::DeviceBuffer>& chunks() const noexcept {
        return chunks_;
    }
    // The chunk at `index` (precondition: index < chunk_count()).
    const compute::DeviceBuffer& chunk(std::size_t index) const {
        return chunks_.at(index);
    }

    // Download the whole column into `out` (resized to row_count()), concatenating
    // its chunks in row order. Mirrors ComputeDevice::download: returns a
    // ComputeStatus, InvalidArgument if Element's dtype does not match the column's.
    template <class Element>
    compute::ComputeStatus to_host(const compute::ComputeDevice& device,
                                   std::vector<Element>& out) const {
        if (compute::data_type_of<Element> != dtype_) {
            return compute::ComputeStatus::invalid_argument(
                "Column::to_host: host element type does not match column '" + name_ +
                "' dtype " + std::string{compute::to_string(dtype_)});
        }
        out.resize(row_count_);
        std::size_t offset = 0;
        for (const compute::DeviceBuffer& chunk : chunks_) {
            const std::span<Element> destination(out.data() + offset,
                                                 chunk.element_count());
            const compute::ComputeStatus status =
                device.download<Element>(chunk, destination);
            if (!status.ok()) {
                out.clear();
                return status;
            }
            offset += chunk.element_count();
        }
        return compute::ComputeStatus::success();
    }

private:
    std::string name_;
    compute::DataType dtype_ = compute::DataType::Float64;
    std::vector<compute::DeviceBuffer> chunks_;
    std::size_t row_count_ = 0;
};

// A typed, columnar frame over a table's rows, backed by ComputeDevice buffers.
// Move-only (it owns its columns' device memory). An empty frame owns no columns
// and references no device.
class DataFrame {
public:
    DataFrame() = default;
    DataFrame(compute::ComputeDevice* device, std::vector<Column> columns,
              std::size_t row_count, std::size_t chunk_rows)
        : device_(device),
          columns_(std::move(columns)),
          row_count_(row_count),
          chunk_rows_(chunk_rows) {}

    DataFrame(DataFrame&&) noexcept = default;
    DataFrame& operator=(DataFrame&&) noexcept = default;
    DataFrame(const DataFrame&) = delete;
    DataFrame& operator=(const DataFrame&) = delete;

    bool empty() const noexcept { return columns_.empty(); }
    std::size_t row_count() const noexcept { return row_count_; }
    std::size_t column_count() const noexcept { return columns_.size(); }

    // The rows-per-chunk this frame was loaded with (all columns share it).
    std::size_t chunk_rows() const noexcept { return chunk_rows_; }
    // The number of row-chunks: ceil(row_count / chunk_rows), 0 when empty.
    std::size_t chunk_count() const noexcept {
        if (chunk_rows_ == 0 || row_count_ == 0) return 0;
        return (row_count_ + chunk_rows_ - 1) / chunk_rows_;
    }

    // The device the columns live on (null for an empty frame).
    compute::ComputeDevice* device() const noexcept { return device_; }

    const std::vector<Column>& columns() const noexcept { return columns_; }
    std::vector<std::string> column_names() const;

    // Find a column by name, or nullptr (a non-owning view into the frame's
    // columns — the column is owned inline by the frame, not separately shared).
    const Column* column(std::string_view name) const;

    // The schema this frame realizes (column names + dtypes; `field` left empty).
    DataFrameSchema schema() const;

    // Convenience: download a named column to a typed host vector via the frame's
    // device. InvalidArgument if the column is absent or the frame is empty.
    template <class Element>
    compute::ComputeStatus to_host(std::string_view name,
                                   std::vector<Element>& out) const {
        const Column* found = column(name);
        if (found == nullptr) {
            return compute::ComputeStatus::invalid_argument(
                "DataFrame::to_host: no column named '" + std::string{name} + "'");
        }
        if (device_ == nullptr) {
            return compute::ComputeStatus::invalid_argument(
                "DataFrame::to_host: an empty frame has no device");
        }
        return found->to_host<Element>(*device_, out);
    }

private:
    compute::ComputeDevice* device_ = nullptr;
    std::vector<Column> columns_;
    std::size_t row_count_ = 0;
    std::size_t chunk_rows_ = 0;
};

// The default rows per chunk a frame is loaded with — matches
// JsonlStore::kRecordsPerChunk so a frame's chunking lines up with the table's
// on-disk chunking.
inline constexpr std::size_t kDefaultChunkRows = 10'000;

// The outcome of loading a table into a DataFrame: the frame (populated only when
// `status.ok()`) plus the status that explains any failure.
struct DataFrameResult {
    DataFrame frame;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// Load `store`'s rows into a typed, chunked DataFrame on `device`, one column per
// `schema` entry. Non-throwing: returns a failing ComputeStatus (and an empty
// frame) on a device error or when a row's field is missing, not a number (or an
// ISO-8601 value for a timestamp float column), not an integer for an integer
// column, or out of range for the column's dtype.
DataFrameResult load_data_frame(const zetta::storage::StorageEngine& store,
                                const DataFrameSchema& schema,
                                compute::ComputeDevice& device,
                                std::size_t chunk_rows = kDefaultChunkRows);

// Infer a DataFrameSchema from `store`'s rows: a field becomes a column iff it is
// present and numeric in *every* row (so the column is dense and the inferred
// schema round-trips), typed Int64 when every value is an integer, else Float64.
// Columns are in the first row's key order. An empty store, or one whose rows share
// no numeric field, yields an empty schema.
DataFrameSchema infer_schema(const zetta::storage::StorageEngine& store);

}  // namespace zetta::ddf
