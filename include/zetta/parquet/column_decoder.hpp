#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/parquet/metadata.hpp"

// Native Parquet column-chunk decoders (ADR-32 / #506) — read a column's data
// pages into typed values, on top of the footer parser (#505) and the page
// codecs (#507). No Apache Arrow, no parquet-cpp.
//
// The footer parser hands back, per column chunk, where its pages live (offset)
// and how they are stored (physical type, codec, encodings). This module reads
// those pages:
//
//   * PLAIN — fixed-width little-endian for int32/int64/float/double, bit-packed
//     for boolean, and a 4-byte-length-prefixed body for byte_array.
//   * dictionary — a PLAIN-encoded dictionary page plus data pages of
//     RLE/bit-packed integer indices into it (PLAIN_DICTIONARY / RLE_DICTIONARY).
//   * definition levels — the RLE/bit-packed-hybrid null markers an OPTIONAL
//     column carries, decoded into a per-row validity bitmap; a REQUIRED column
//     carries none (all rows present). Repetition levels (nested / repeated
//     columns) are out of scope: such a column fails with a reason.
//
// Scope is the **flat** churn-shaped columns the target datasets use, page
// version 1 (pyarrow's default), and the codecs zetta can decompress today
// (uncompressed + snappy). A page this slice does not model — a v2 data page, an
// unsupported value encoding (delta / byte-stream-split), a codec without a
// decoder yet (zstd / gzip), or an out-of-range dictionary index — fails the
// decode with a clear reason rather than producing wrong values, the binary
// analog of schema-at-the-boundary. Projecting these typed columns into
// [DDF](../../docs/ddf.md) columns is the next slice (#508).
//
// The page bytes arrive from disk or a peer, so they are a **trust boundary**:
// zetta-core is compiled `-fexperimental-library` (no exceptions across the
// seam), so every entry point is **non-throwing** and every read is
// bounds-checked. The JSON projection `to_json` emits has a schema
// (`parquet_columns_schema`) validated where it re-enters the program
// (`validate_parquet_columns_json`). See docs/parquet-column-decoders.md.
namespace zetta::parquet {

// The dense, present-only decoded values of a column. Exactly one vector is
// populated, selected by the column's `PhysicalType` (the rest stay empty).
// Nulls are NOT stored here — they are marked by `DecodedColumn::validity`, so
// these vectors hold only the present values, in row order (the Arrow-native
// values + validity-bitmap split the DDF projection consumes).
struct ColumnValues {
    std::vector<std::int32_t> int32_values;       // Int32
    std::vector<std::int64_t> int64_values;       // Int64
    std::vector<float> float_values;              // Float
    std::vector<double> double_values;            // Double
    std::vector<std::uint8_t> bool_values;        // Boolean (0 / 1)
    std::vector<std::string> byte_array_values;   // ByteArray / FixedLenByteArray
};

// One decoded leaf column (a whole column across its row groups, or a single
// chunk). `validity` is empty for a REQUIRED column (every row present); for an
// OPTIONAL column it has one entry per logical row (1 = present, 0 = null), and
// `values` holds only the present values in row order.
struct DecodedColumn {
    std::string path;                       // dotted schema path
    PhysicalType type = PhysicalType::Unknown;
    std::int64_t num_rows = 0;              // logical rows, including nulls
    std::vector<std::uint8_t> validity;     // size num_rows, or empty => all present
    ColumnValues values;                    // dense present values, by type

    // True if `row` (a logical row index) holds a present value.
    bool row_is_valid(std::int64_t row) const {
        return validity.empty() || (row >= 0 && static_cast<std::size_t>(row) < validity.size() &&
                                    validity[static_cast<std::size_t>(row)] != 0);
    }

    // The number of present (non-null) values — the length of the populated
    // `values` vector.
    std::int64_t valid_count() const;
};

// The result of a decode: on success `error` is empty and `column` holds the
// values; on failure `error` says why and `column` is default.
struct DecodedColumnResult {
    DecodedColumn column;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Decode one column chunk — one row group's slice of a leaf column. `metadata`
// is the parsed footer (#505); the chunk is `row_groups[row_group_index]
// .columns[column_index]`, and `column_index` also selects the matching leaf
// (for its physical type + max definition level). Non-throwing.
DecodedColumnResult decode_column_chunk(std::string_view file_bytes,
                                        const FileMetaData& metadata,
                                        std::size_t row_group_index,
                                        std::size_t column_index);

// Decode a whole leaf column across all row groups, concatenated in row-group
// order — the full column's typed values. Non-throwing.
DecodedColumnResult decode_column(std::string_view file_bytes,
                                  const FileMetaData& metadata,
                                  std::size_t column_index);

// The canonical JSON projection of a decoded column:
// `{ "path", "type", "values": [ <value-or-null> ... ] }`, the `values` array
// one entry per logical row (a null row is JSON null; a byte_array is its bytes
// as a JSON string). Matches the committed golden shape and `parquet_columns_schema`.
nlohmann::json to_json(const DecodedColumn& column);

// The draft-07 JSON Schema for a decoded-columns document
// (`{ "num_rows", "columns": [ <to_json column> ... ] }`) — the schema-as-code
// source of truth (schemas/parquet_columns.schema.json is generated from it).
const nlohmann::json& parquet_columns_schema();

// Validate a decoded-columns document against `parquet_columns_schema`. Returns
// "" when valid, else the first path-qualified failure reason — the boundary
// check for the committed golden (and any persisted decoded-column JSON).
std::string validate_parquet_columns_json(const nlohmann::json& document);

}  // namespace zetta::parquet
