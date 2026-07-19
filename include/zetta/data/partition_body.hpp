#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The **body format of a partition** (#833) — one writer, one reader, for every
// dataset.
//
// A partition body used to be JSON Lines: one `{"_zid": <id>, "doc": <row>}`
// object per row. It is now **Parquet** — the row's fields as columns, with the
// row id in a `_zid` column beside them — so that one body format serves the
// whole engine, a body can be read a page at a time
// ([`ParquetStore`](../../../docs/data/parquet-store.md)), and a reader can
// project the columns it wants instead of parsing every row (a featurize that
// drops 3072 `embedding_*` columns should not pay to decode them).
//
// Two things keep that honest:
//
//   * **Old bodies still read.** `.jsonl` bodies exist on running nodes.
//     `read_partition_body` sniffs the format from the bytes (`PAR1` magic at
//     both ends) rather than trusting a name, so an old body is read as JSON
//     Lines and a new one as Parquet — never mis-parsed as the other.
//   * **A row Parquet cannot express falls back to JSON Lines.** The writer is
//     flat and scalar-typed: a row holding an array or an object, or a column
//     mixing strings and numbers, has no Parquet schema that would round-trip
//     it. Such a partition is written as JSON Lines and says so in
//     `PartitionBodyWriteResult::format` (which the catalog records), rather
//     than being coerced into a shape that loses data.
//
// Nothing here throws. See docs/data/partitioned-write-path.md.
namespace zetta::data {

// How a body's bytes are laid out.
enum class PartitionBodyFormat {
    Parquet,  // the default: columns + a `_zid` column, `PAR1` at both ends
    Jsonl,    // one {"_zid", "doc"} object per line — legacy bodies + the fallback
};

// "parquet" / "jsonl" — what the catalog's `stats.format` records.
std::string_view to_string(PartitionBodyFormat format);

// ".parquet" / ".jsonl" — the large object's name suffix, so a body's format is
// visible without reading it.
std::string_view body_extension(PartitionBodyFormat format);

// One row of a partition: its stable id (its CRDT key) and its data.
struct PartitionRow {
    std::string id;
    nlohmann::json doc;
};

// The result of serializing a body: on success `error` is empty, `bytes` holds
// the body and `format` says which format it ended up in (Parquet, or JSON Lines
// when the rows could not be expressed as flat columns).
struct PartitionBodyWriteResult {
    std::string bytes;
    PartitionBodyFormat format = PartitionBodyFormat::Parquet;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Serialize a partition's rows. Prefers Parquet; falls back to JSON Lines for
// rows Parquet cannot represent (nested values, a column mixing types, or a row
// that already carries its own `_zid` field, which would collide with the id
// column). Only a failure of *both* returns an error.
PartitionBodyWriteResult serialize_partition_body(const std::vector<PartitionRow>& rows);

// Which format `body` is in, decided from the bytes: `PAR1` at both ends is
// Parquet, anything else is read as JSON Lines.
PartitionBodyFormat sniff_partition_body(std::string_view body);

// The result of reading a body: on success `error` is empty, `rows` holds the
// rows in body order and `format` says how they were read; on failure `rows` is
// empty and `error` carries a reason naming the partition.
struct PartitionBodyReadResult {
    std::vector<PartitionRow> rows;
    PartitionBodyFormat format = PartitionBodyFormat::Parquet;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Read a partition body into rows, sniffing its format. `partition_id` names the
// partition in error messages and seeds synthetic ids for rows that carry none.
//
// **Fails fast**: a malformed line or an undecodable Parquet body returns an
// error rather than dropping rows, because a silently short partition diverges
// from the single-node oracle.
//
// One representational difference is worth stating: a Parquet body has a column
// for every field any row carries, so a row that **omitted** a field reads back
// with that field explicitly null. JSON Lines distinguished the two; a columnar
// format cannot, and every consumer here treats an absent field and a null field
// the same way.
PartitionBodyReadResult read_partition_body(std::string_view body,
                                            std::string_view partition_id);

}  // namespace zetta::data
