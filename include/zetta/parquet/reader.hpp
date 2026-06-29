#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Native Parquet row reader (ADR-32 / #508) — assemble the footer parser (#505),
// the column-chunk decoders (#506), and the page codecs (#507) into a reader that
// materializes a whole file's rows as JSON objects. No Apache Arrow, no
// parquet-cpp.
//
// This is the **untyped-rows** face of the reader: it decodes every leaf column
// and transposes the typed, columnar values back into one `nlohmann::json` object
// per logical row (`{ column_path: value-or-null, ... }`), the inverse of the
// `JsonlStore`'s `{_zid, doc}` shape — so a `.parquet` file drops into any code
// path that already consumes JSON rows (the churn dataset loader, `import_records`,
// …) the way a `.jsonl` file does. A null row becomes JSON null; a `byte_array`
// becomes a JSON string; numeric and boolean columns become their natural JSON
// scalar.
//
// The *typed-columns* face — projecting a **requested subset** of columns
// straight into DDF columns (DeviceBuffers), so the 3072 embedding columns of a
// dataset are never decoded — is `zetta::ddf::load_parquet_data_frame`
// (include/zetta/ddf/parquet_reader.hpp). Both share this metadata + decoder
// layer.
//
// The file bytes arrive from disk or a peer, so they are a **trust boundary**:
// zetta-core is compiled `-fexperimental-library` (no exceptions across the
// seam), so this entry point is **non-throwing** and every read is bounds-checked
// (it inherits the bounds checks of the footer parser + decoders). A malformed /
// truncated / unsupported file returns a failing result with a reason instead of
// reading out of bounds. See docs/parquet-reader.md.
namespace zetta::parquet {

// The outcome of reading a Parquet file's rows: on success `error` is empty and
// `rows` holds one JSON object per logical row (in row order); on failure `error`
// says why and `rows` is empty.
struct ParquetRowsResult {
    std::vector<nlohmann::json> rows;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Read `file_bytes` (the whole Parquet file) into one JSON object per row. Each
// object maps every leaf column's dotted path to its value for that row (JSON
// null for a null row, a JSON string for a `byte_array`, the natural JSON scalar
// otherwise). Non-throwing: a malformed file, an unsupported page/codec/encoding,
// or an out-of-range index returns a failing result with a reason.
ParquetRowsResult read_parquet_rows(std::string_view file_bytes);

}  // namespace zetta::parquet
