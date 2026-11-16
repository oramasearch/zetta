// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Native Parquet footer / metadata parser — no Apache Arrow, no parquet-cpp
// (ADR-32 — hand-rolled, read-only Parquet reader; ADR-17 — no foreign runtime).
//
// A Parquet file ends with its footer:
//
//   <PAR1>                       4-byte leading magic
//   ... data pages, dictionary pages, page headers ...
//   <Thrift FileMetaData>        the layout description (Thrift compact protocol)
//   <footer length>              4-byte little-endian u32 = byte length of the
//                                FileMetaData blob just before it
//   <PAR1>                       4-byte trailing magic
//
// This module reads just that footer: it locates and decodes the Thrift
// `FileMetaData` (the parquet.thrift schema, serialized with the Thrift
// *compact* protocol) into a typed, JSON-serializable layout — the file's
// schema (column names + physical types), its row groups, and per-column-chunk
// offsets / sizes / encodings / codecs. It does **not** decode any data:
// reading the page bodies into typed values (PLAIN / dictionary / RLE) is the
// column-chunk decoders (#506, column_decoder.hpp), decompressing them is the
// Snappy codec (#507), and projecting them into DDF columns is #508 — separate
// slices that consume this layout (this header also exposes the per-leaf max
// definition / repetition levels those decoders need, on LeafColumn).
//
// The footer bytes arrive from disk or a peer, so they are a **trust boundary**:
// zetta-core is compiled `-fexperimental-library` (no exceptions across the
// seam), so every entry point is **non-throwing** and every read is
// bounds-checked. A malformed / truncated / encrypted-footer file fails with a
// reason rather than reading out of bounds. The JSON form `to_json` emits has a
// schema (`parquet_metadata_schema`) validated where it re-enters the program
// (`validate_parquet_metadata_json`), the binary-format analog of the
// schema-at-the-boundary rule (Thrift is binary, so there is no JSON schema for
// the file itself). See docs/data/parquet-metadata.md.
namespace zetta::parquet {

// The Parquet physical column type (parquet.thrift `Type`). `Unknown` is a
// forward-compatible catch-all for a type code this reader does not model.
enum class PhysicalType {
    Boolean,
    Int32,
    Int64,
    Int96,
    Float,
    Double,
    ByteArray,
    FixedLenByteArray,
    Unknown,
};

// How often a column's value repeats (parquet.thrift `FieldRepetitionType`).
// `Undefined` means the field was absent (the root/group elements).
enum class RepetitionType {
    Required,
    Optional,
    Repeated,
    Undefined,
};

// The page encoding of a column chunk (parquet.thrift `Encoding`). `Unknown`
// is the forward-compatible catch-all.
enum class Encoding {
    Plain,
    PlainDictionary,
    Rle,
    BitPacked,
    DeltaBinaryPacked,
    DeltaLengthByteArray,
    DeltaByteArray,
    RleDictionary,
    ByteStreamSplit,
    Unknown,
};

// The page compression codec of a column chunk (parquet.thrift
// `CompressionCodec`). `Unknown` is the forward-compatible catch-all.
enum class CompressionCodec {
    Uncompressed,
    Snappy,
    Gzip,
    Lzo,
    Brotli,
    Lz4,
    Zstd,
    Lz4Raw,
    Unknown,
};

// Lowercase canonical names (matching pyarrow's lowercased enum names, the
// golden reference) — the strings `to_json` emits.
std::string_view to_string(PhysicalType type);
std::string_view to_string(RepetitionType repetition);
std::string_view to_string(Encoding encoding);
std::string_view to_string(CompressionCodec codec);

// One node of the file's flattened schema tree (parquet.thrift `SchemaElement`,
// stored depth-first preorder). The first element is the root group (no type,
// `num_children` = the number of top-level columns); leaf elements carry a
// physical type. Nesting is preserved faithfully even though the leaf
// projection (`leaf_columns`) is what most callers want.
struct SchemaElement {
    std::string name;
    bool has_type = false;                              // false for group nodes
    PhysicalType type = PhysicalType::Unknown;
    RepetitionType repetition = RepetitionType::Undefined;
    std::int32_t type_length = 0;                       // FIXED_LEN_BYTE_ARRAY width
    std::int32_t num_children = 0;                      // > 0 for group nodes
};

// A leaf (data-bearing) column: its dotted path through the schema and its
// physical type — the "schema (column names/types)" the acceptance asks for.
// The max definition / repetition levels are the per-column constants the
// page decoders (#506) need: a flat OPTIONAL column has `max_definition_level`
// 1 (its def-level stream marks nulls), a REQUIRED column has 0 (no def levels
// are stored at all); `max_repetition_level` > 0 marks a nested/repeated column.
struct LeafColumn {
    std::string path;       // dotted, e.g. "account_id" or "address.city"
    PhysicalType type = PhysicalType::Unknown;
    int max_definition_level = 0;
    int max_repetition_level = 0;
};

// One column chunk in one row group (parquet.thrift `ColumnChunk` +
// `ColumnMetaData`) — the per-column-chunk layout the decoders consume.
struct ColumnChunkMeta {
    std::vector<std::string> path_in_schema;   // column path components
    PhysicalType type = PhysicalType::Unknown;
    CompressionCodec codec = CompressionCodec::Unknown;
    std::vector<Encoding> encodings;
    std::int64_t num_values = 0;
    std::int64_t total_uncompressed_size = 0;
    std::int64_t total_compressed_size = 0;
    std::int64_t data_page_offset = 0;
    std::int64_t file_offset = 0;
    bool has_dictionary_page = false;
    std::int64_t dictionary_page_offset = 0;   // valid only if has_dictionary_page

    // The path joined with '.', as pyarrow reports it.
    std::string path() const;
};

// One row group: a horizontal slice of the table, holding one column chunk per
// column (parquet.thrift `RowGroup`).
struct RowGroupMeta {
    std::int64_t num_rows = 0;
    std::int64_t total_byte_size = 0;
    std::vector<ColumnChunkMeta> columns;
};

// The decoded footer (parquet.thrift `FileMetaData`).
struct FileMetaData {
    std::int32_t version = 0;                   // format major (1 or 2)
    std::string created_by;
    std::int64_t num_rows = 0;
    std::vector<SchemaElement> schema;          // flattened, schema[0] = root
    std::vector<RowGroupMeta> row_groups;

    // The data-bearing columns in column order, derived from the schema tree.
    std::vector<LeafColumn> leaf_columns() const;
};

// The result of parsing a footer: on success `error` is empty and `metadata`
// holds the layout; on failure `metadata` is default and `error` says why.
struct ParquetMetadataResult {
    FileMetaData metadata;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Parse a Parquet file's footer from the **whole file's bytes** (the reader
// needs the trailing magic + length, which sit at the very end). Non-throwing:
// a too-small / wrong-magic / encrypted-footer / truncated / malformed file
// returns a failing result with a reason instead of reading out of bounds.
ParquetMetadataResult parse_parquet_metadata(std::string_view file_bytes);

// The canonical JSON projection of a decoded footer (schema leaf columns +
// row-group / column-chunk layout). Matches the committed pyarrow golden shape
// and `parquet_metadata_schema`. Never throws.
nlohmann::json to_json(const FileMetaData& metadata);

// The draft-07 JSON Schema for `to_json`'s output — the schema-as-code source
// of truth (schemas/parquet_metadata.schema.json is generated from it).
const nlohmann::json& parquet_metadata_schema();

// Validate a `to_json`-shaped document against `parquet_metadata_schema`.
// Returns "" when valid, else the first path-qualified failure reason. The
// boundary check for the committed golden (and any persisted metadata JSON).
std::string validate_parquet_metadata_json(const nlohmann::json& document);

}  // namespace zetta::parquet
