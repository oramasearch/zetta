#!/usr/bin/env python3
"""Generate the committed Parquet metadata test fixture + golden (DEV-TIME ONLY).

zetta's native Parquet footer/metadata parser (ADR-32, issue #505) is verified
against a golden produced here by Apache Arrow's pyarrow — an *independent*
reference for the schema + row-group / column-chunk layout the native parser
extracts from the file's Thrift `FileMetaData`.

This script is NOT part of the build or CI (ADR-17 — no foreign runtime in the
engine / build / CI). It is run by hand, once, to (re)generate four committed
artifacts under tests/fixtures/parquet/:

  * churn_sample.parquet            — a small, representative Parquet file
  * churn_sample.metadata.json      — pyarrow's dump of its metadata, in the
                                      exact shape zetta::parquet::to_json emits
  * churn_decode.parquet            — a Parquet file the native column-chunk
                                      decoders (#506) read end to end: it uses
                                      only codecs zetta can decompress today
                                      (uncompressed + snappy — churn_sample mixes
                                      in zstd/gzip, deliberately, to exercise the
                                      metadata parser, but those are not decodable
                                      yet), spans both PLAIN and dictionary
                                      encodings, every needed physical type, and
                                      a nullable column (definition levels).
  * churn_decode.values.json        — the decoded values per column, in the shape
                                      zetta::parquet::to_json(DecodedColumn) emits;
                                      a null row is JSON null.

Usage (needs pyarrow in a throwaway venv):

  python3.12 -m venv /tmp/pqvenv && /tmp/pqvenv/bin/pip install pyarrow
  /tmp/pqvenv/bin/python scripts/gen-parquet-fixture.py
"""
import json
import os

import pyarrow as pa
import pyarrow.parquet as pq

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "tests", "fixtures", "parquet"))
PARQUET_PATH = os.path.join(OUT_DIR, "churn_sample.parquet")
GOLDEN_PATH = os.path.join(OUT_DIR, "churn_sample.metadata.json")
DECODE_PARQUET_PATH = os.path.join(OUT_DIR, "churn_decode.parquet")
DECODE_VALUES_PATH = os.path.join(OUT_DIR, "churn_decode.values.json")

# A churn-shaped table: a mix of physical types the UC-1 datasets use.
TABLE = pa.table(
    {
        "account_id": pa.array([1, 2, 3, 4, 5, 6], type=pa.int64()),
        "risk_score": pa.array([0.10, 0.92, 0.55, 0.71, 0.18, 0.83], type=pa.float64()),
        "failed_payments": pa.array([0, 4, 1, 2, 0, 3], type=pa.int64()),
        "is_churned": pa.array([False, True, False, True, False, True], type=pa.bool_()),
        "segment": pa.array(["smb", "ent", "smb", "ent", "mid", "smb"], type=pa.string()),
    }
)

# Vary codecs + dictionary use across columns so the golden exercises codec and
# encoding identification (and the optional dictionary-page offset) per chunk.
COMPRESSION = {
    "account_id": "snappy",
    "risk_score": "zstd",
    "failed_payments": "none",   # uncompressed
    "is_churned": "gzip",
    "segment": "snappy",
}
USE_DICTIONARY = ["account_id", "risk_score", "is_churned", "segment"]  # not failed_payments

# Two row groups (3 rows each) so the layout has more than one row group.
pq.write_table(
    TABLE,
    PARQUET_PATH,
    compression=COMPRESSION,
    use_dictionary=USE_DICTIONARY,
    row_group_size=3,
    version="2.6",
)


def major_version(format_version: str) -> int:
    # The Thrift FileMetaData.version field is the format major (1 or 2).
    return int(str(format_version).split(".")[0])


def physical(name: str) -> str:
    return name.lower()


def codec(name: str) -> str:
    return name.lower()


def encoding(name: str) -> str:
    return name.lower()


meta = pq.read_metadata(PARQUET_PATH)

columns = []
for i in range(len(meta.schema)):
    col = meta.schema.column(i)
    columns.append({"path": col.path, "type": physical(col.physical_type)})

row_groups = []
for g in range(meta.num_row_groups):
    rg = meta.row_group(g)
    chunks = []
    for c in range(rg.num_columns):
        cc = rg.column(c)
        chunk = {
            "path": cc.path_in_schema,
            "type": physical(cc.physical_type),
            "codec": codec(cc.compression),
            "encodings": [encoding(e) for e in cc.encodings],
            "num_values": cc.num_values,
            "total_uncompressed_size": cc.total_uncompressed_size,
            "total_compressed_size": cc.total_compressed_size,
            "data_page_offset": cc.data_page_offset,
            "file_offset": cc.file_offset,
        }
        if cc.has_dictionary_page:
            chunk["dictionary_page_offset"] = cc.dictionary_page_offset
        chunks.append(chunk)
    row_groups.append(
        {
            "num_rows": rg.num_rows,
            "total_byte_size": rg.total_byte_size,
            "columns": chunks,
        }
    )

golden = {
    "version": major_version(meta.format_version),
    "created_by": meta.created_by,
    "num_rows": meta.num_rows,
    "num_columns": meta.num_columns,
    "columns": columns,
    "row_groups": row_groups,
}

with open(GOLDEN_PATH, "w") as f:
    json.dump(golden, f, indent=2)
    f.write("\n")

print("wrote", PARQUET_PATH, os.path.getsize(PARQUET_PATH), "bytes")
print("wrote", GOLDEN_PATH)
print(json.dumps(golden, indent=2))


# --- decode fixture (#506) --------------------------------------------------
#
# A file the native column-chunk decoders read end to end. It is separate from
# churn_sample.parquet because that file mixes in zstd + gzip (to exercise the
# metadata parser's codec identification), and those codecs have no native
# decompressor yet. This file uses only uncompressed + snappy, but spans every
# encoding/type the decoders must handle:
#
#   * PLAIN int64 / double / float32 / boolean        (fixed-width + bit-packed)
#   * dictionary int32 / string                       (RLE/bit-packed indices)
#   * a REQUIRED column (no definition levels) and an OPTIONAL one (def levels)
#
# The float32 values are exact in float32 (and the doubles round-trip), so the
# decoded numbers equal the golden's exactly.
DECODE_SCHEMA = pa.schema(
    [
        pa.field("account_id", pa.int64(), nullable=False),
        pa.field("risk_score", pa.float64(), nullable=False),
        pa.field("score_f32", pa.float32(), nullable=False),
        pa.field("failed_payments", pa.int32(), nullable=False),
        pa.field("is_churned", pa.bool_(), nullable=False),
        pa.field("segment", pa.string(), nullable=False),
        pa.field("tenure_days", pa.int64(), nullable=True),  # OPTIONAL → def levels
    ]
)
DECODE_VALUES = {
    "account_id": [101, 102, 103, 104, 105, 106],
    "risk_score": [0.1, 0.92, 0.55, 0.71, 0.18, 0.83],
    "score_f32": [0.5, 0.25, 0.75, 1.0, 0.125, 0.625],
    "failed_payments": [0, 4, 1, 2, 0, 3],
    "is_churned": [False, True, False, True, False, True],
    "segment": ["smb", "ent", "smb", "ent", "mid", "smb"],
    "tenure_days": [10, None, 30, 40, None, 60],
}
DECODE_NUM_ROWS = 6
DECODE_TABLE = pa.table(
    {
        name: pa.array(DECODE_VALUES[name], type=DECODE_SCHEMA.field(name).type)
        for name in DECODE_SCHEMA.names
    },
    schema=DECODE_SCHEMA,
)
DECODE_COMPRESSION = {
    "account_id": "none",       # PLAIN int64, uncompressed
    "risk_score": "snappy",     # PLAIN double, snappy
    "score_f32": "none",        # PLAIN float, uncompressed
    "failed_payments": "snappy",  # dictionary int32, snappy
    "is_churned": "none",       # PLAIN bool, uncompressed
    "segment": "snappy",        # dictionary byte_array, snappy
    "tenure_days": "none",      # PLAIN int64 OPTIONAL, uncompressed (def levels)
}
DECODE_USE_DICTIONARY = ["failed_payments", "segment"]

# Two row groups (4 + 2) so the decoder concatenates chunks across row groups.
pq.write_table(
    DECODE_TABLE,
    DECODE_PARQUET_PATH,
    compression=DECODE_COMPRESSION,
    use_dictionary=DECODE_USE_DICTIONARY,
    row_group_size=4,
    version="2.6",
)

decode_meta = pq.read_metadata(DECODE_PARQUET_PATH)


def physical_of(field_type) -> str:
    # The parquet physical type name zetta::parquet::to_string emits.
    if field_type == pa.int32():
        return "int32"
    if field_type == pa.int64():
        return "int64"
    if field_type == pa.float32():
        return "float"
    if field_type == pa.float64():
        return "double"
    if field_type == pa.bool_():
        return "boolean"
    if field_type == pa.string():
        return "byte_array"
    raise ValueError("unmapped type " + str(field_type))


decode_golden = {
    "num_rows": DECODE_NUM_ROWS,
    "columns": [
        {
            "path": name,
            "type": physical_of(DECODE_SCHEMA.field(name).type),
            "values": DECODE_VALUES[name],
        }
        for name in DECODE_SCHEMA.names
    ],
}

with open(DECODE_VALUES_PATH, "w") as f:
    json.dump(decode_golden, f, indent=2)
    f.write("\n")

print("wrote", DECODE_PARQUET_PATH, os.path.getsize(DECODE_PARQUET_PATH), "bytes")
print("wrote", DECODE_VALUES_PATH)
print(json.dumps(decode_golden, indent=2))
