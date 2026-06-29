#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/ddf/data_frame.hpp"

// Native Parquet → DDF reader with column projection (ADR-32 / #508) — the
// capstone of the native Parquet stack: it assembles the footer parser (#505),
// the column-chunk decoders (#506), and the page codecs (#507) into a reader that
// materializes a **requested subset** of a file's columns straight into a typed,
// chunked `ddf::DataFrame` (DeviceBuffer-backed columns). No Apache Arrow, no
// parquet-cpp.
//
// **Column projection** is the point: the `DataFrameSchema` names *which* columns
// to read (and their target dtype), and only those leaf columns' chunks are
// decoded — the other columns' page bytes are never touched. That is what lets a
// caller load the engineered numeric columns + `outcome` of the
// saas-sales-conversations dataset while **dropping `embedding_0..3071`** (3072
// columns) without paying to decode them. It is the typed-columns sibling of
// `zetta::parquet::read_parquet_rows` (the untyped whole-file rows reader), and
// the on-bytes analogue of `ddf::load_data_frame` (which loads from a table).
//
// A DataFrame is **numeric + dense**, so this reader is too: each requested column
// must be a numeric (or boolean) leaf with **no nulls** in the rows read, and its
// values are converted to the requested `DataType` validated at the boundary
// (integer-into-float widens; a real-into-integer / negative-into-unsigned /
// out-of-range / byte_array-into-numeric / null value fails with a clear reason).
//
// Non-throwing (zetta-core is `-fexperimental-library`): a malformed file, a
// missing / non-numeric / null / out-of-range column, or a device error returns a
// failing `ComputeStatus` (and an empty frame) rather than throwing across the
// seam. See docs/parquet-reader.md.
namespace zetta::ddf {

// Load a projected subset of `file_bytes` (a whole Parquet file) into a typed,
// chunked DataFrame on `device`. Each `schema` column's `source_field()` is the
// **parquet leaf path** to read and `name` is the frame column; only the selected
// leaf columns are decoded (column projection). The chunk layout matches
// `load_data_frame` (rows-per-chunk = `chunk_rows`). Non-throwing.
DataFrameResult load_parquet_data_frame(std::string_view file_bytes,
                                        const DataFrameSchema& schema,
                                        compute::ComputeDevice& device,
                                        std::size_t chunk_rows = kDefaultChunkRows);

// The outcome of inferring a schema from a Parquet file's metadata: the schema
// (populated only when `error` is empty) plus a reason on failure.
struct ParquetSchemaResult {
    DataFrameSchema schema;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Infer a DataFrameSchema from `file_bytes`'s **metadata only** (no column
// decode): every numeric leaf column becomes a frame column typed to its natural
// DataType (int32 → Int32, int64 → Int64, float → Float32, double → Float64,
// boolean → UInt8), in column order. Non-numeric (`byte_array`) leaves are
// skipped. The "just load every number" convenience; the projection overload
// above is what drops the embeddings. Non-throwing.
ParquetSchemaResult infer_parquet_schema(std::string_view file_bytes);

// The draft-07 JSON Schema for a materialized-DataFrame golden document —
// `{ "num_rows", "columns": [ { "name", "dtype", "values": [<number>...] } ] }`,
// the dense numeric projection this reader produces. The schema-as-code source of
// truth (schemas/parquet_data_frame.schema.json is generated from it), so the
// TC-9 parity golden re-enters the program through a validated boundary.
const nlohmann::json& parquet_data_frame_schema();

// Validate a materialized-DataFrame document against `parquet_data_frame_schema`.
// Returns "" when valid, else the first path-qualified failure reason.
std::string validate_parquet_data_frame_json(const nlohmann::json& document);

}  // namespace zetta::ddf
