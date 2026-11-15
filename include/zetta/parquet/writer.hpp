// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/parquet/metadata.hpp"

// Native Parquet **writer** (#833) — the inverse of `read_parquet_rows`. No
// Apache Arrow, no parquet-cpp, no pyarrow (ADR-32 / ADR-17: no foreign
// runtime).
//
// zetta could read Parquet and not write it, which is why partition bodies were
// still JSON-Lines. This module closes that: it emits a complete, spec-shaped
// Parquet file — Thrift-compact page headers and footer, PLAIN-encoded column
// values, RLE definition levels for nullable columns, SNAPPY-compressed page
// bodies, `PAR1` magic at both ends.
//
// The scope is deliberately the exact shape the reader already handles, because
// the reader is the oracle: **flat** columns (no nesting or repetition), **v1**
// data pages, **PLAIN** values, and the physical types `read_parquet_rows`
// materializes — boolean, int32, int64, float, double, byte_array. Dictionary
// encoding, data page v2, and nested schemas are deliberately later slices; a
// column this writer cannot express fails with a reason rather than being
// silently coerced.
//
// The round-trip is the contract:
//
//     write_parquet_rows(rows) → read_parquet_rows(bytes) == rows
//
// value for value and type for type (an integer stays an integer, a double
// stays a double, a null stays a null), for every row shape the schema
// inference accepts. See docs/data/parquet-writer.md.
//
// Nothing here throws: zetta-core is compiled `-fexperimental-library`, so every
// entry point returns a result carrying a path-qualified reason, exactly like
// the reader's.
namespace zetta::parquet {

// One column of the file's flat schema: the name it is written under, the
// physical type its values are encoded as, and whether it may hold nulls.
//
// `optional` is what decides whether the column carries a definition-level
// stream. A REQUIRED column carries none — which also means a null in a
// REQUIRED column is a write error, not a silently dropped value.
struct WriterColumn {
    std::string name;
    PhysicalType type = PhysicalType::Unknown;
    bool optional = true;
};

// How the file is laid out. The defaults produce the page/row-group shape a
// partition body wants; a test can shrink them to force many pages and many row
// groups over a handful of rows.
struct WriterOptions {
    // Page codec. SNAPPY is the default because these blobs are the on-disk
    // storage; UNCOMPRESSED is accepted for debugging. Any other codec fails —
    // zetta has no encoder for it, and writing one it cannot read back would be
    // worse than refusing.
    CompressionCodec codec = CompressionCodec::Snappy;

    // Rows per row group and rows per data page. Both must be positive.
    std::int64_t max_rows_per_row_group = 65536;
    std::int64_t max_rows_per_page = 8192;

    // The footer's `created_by` string, reported by every Parquet tool.
    std::string created_by = "zetta";
};

// The result of inferring a schema from rows: on success `error` is empty and
// `columns` holds the inferred schema in column order; on failure `error` says
// which column could not be typed and why.
struct ParquetSchemaResult {
    std::vector<WriterColumn> columns;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Infer a flat Parquet schema from JSON rows.
//
//   * Column order is the sorted key order the rows present (which is also the
//     order `read_parquet_rows` hands its rows back in, so a round trip is
//     order-stable).
//   * A column is OPTIONAL if any row omits it or holds JSON null for it.
//   * Types: all-boolean → `boolean`; all-integer → `int64`; any real number
//     among the numbers → `double` (an integer widens); all-string →
//     `byte_array`. A column with no non-null value anywhere is `byte_array`
//     (nothing about it contradicts that, and every row of it is null anyway).
//   * A column mixing families (a string and a number, a boolean and a number)
//     or holding an array/object fails with a path-qualified reason — there is
//     no physical type that would round-trip it.
ParquetSchemaResult infer_parquet_schema(const std::vector<nlohmann::json>& rows);

// The result of writing: on success `error` is empty and `data` holds the whole
// file's bytes; on failure `data` is empty and `error` says why.
struct ParquetWriteResult {
    std::string data;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Write `rows` as a Parquet file, inferring the schema with
// `infer_parquet_schema`. Non-throwing; an unrepresentable row shape or an
// invalid option returns a failing result.
ParquetWriteResult write_parquet_rows(const std::vector<nlohmann::json>& rows,
                                      const WriterOptions& options = {});

// Write `rows` against an explicit `columns` schema — for a caller that already
// knows its types (a column of integers that must stay `int32`, a column that
// must stay nullable even though this batch happens to be dense). A value whose
// JSON type does not match its column's physical type, or a null in a REQUIRED
// column, fails with a path-qualified reason. Keys in a row that are not in
// `columns` are ignored; columns absent from a row are written as null.
ParquetWriteResult write_parquet_rows(const std::vector<nlohmann::json>& rows,
                                      const std::vector<WriterColumn>& columns,
                                      const WriterOptions& options = {});

}  // namespace zetta::parquet
