#!/usr/bin/env python3
"""Generate the committed Parquet test fixtures + goldens (DEV-TIME ONLY).

zetta's native Parquet reader (ADR-32, issues #505–#508) is verified against
goldens produced here by Apache Arrow's pyarrow — an *independent* reference for
the schema + layout the native parser extracts, the decoded values, and the
materialized DataFrame.

This script is NOT part of the build or CI (ADR-17 — no foreign runtime in the
engine / build / CI). It is run by hand, once, to (re)generate the committed
artifacts under tests/fixtures/parquet/ (and the churn Parquet siblings under
tests/fixtures/churn/):

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
  * saas_sample.parquet             — a scaled-down saas-sales-conversations file
                                      (#508): engineered numeric columns + an
                                      `outcome` label + a handful of embedding_*
                                      float columns (the real dataset has
                                      embedding_0..3071) — to exercise the reader's
                                      column projection (drop the embeddings).
  * saas_sample.dataframe.json      — the golden DataFrame for the projected
                                      subset (engineered numeric + outcome only),
                                      in the shape zetta::ddf::load_parquet_data_frame
                                      materializes (TC-9 / #499 parity).
  * ../churn/accounts.parquet       — the churn fixture's accounts/events as
  * ../churn/events.parquet           Parquet (written from the canonical JSONL),
                                      so load_churn_dataset(..., Parquet) round-trips
                                      to the same dataset as JSONL/CSV (#508).

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
CHURN_DIR = os.path.normpath(os.path.join(HERE, "..", "tests", "fixtures", "churn"))
PARQUET_PATH = os.path.join(OUT_DIR, "churn_sample.parquet")
GOLDEN_PATH = os.path.join(OUT_DIR, "churn_sample.metadata.json")
DECODE_PARQUET_PATH = os.path.join(OUT_DIR, "churn_decode.parquet")
DECODE_VALUES_PATH = os.path.join(OUT_DIR, "churn_decode.values.json")
SAAS_PARQUET_PATH = os.path.join(OUT_DIR, "saas_sample.parquet")
SAAS_GOLDEN_PATH = os.path.join(OUT_DIR, "saas_sample.dataframe.json")
CHURN_ACCOUNTS_PARQUET = os.path.join(CHURN_DIR, "accounts.parquet")
CHURN_EVENTS_PARQUET = os.path.join(CHURN_DIR, "events.parquet")

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


# --- saas-sales-conversations sample (#508, TC-9) ---------------------------
#
# A scaled-down stand-in for the saas-sales-conversations dataset: a few
# engineered numeric columns + an `outcome` label, plus a handful of embedding_*
# float columns. The real dataset carries embedding_0..3071 (3072 columns); the
# whole point of the reader's column projection is to load the engineered columns
# + outcome WITHOUT decoding the embeddings, so the fixture only needs enough
# embedding columns (8) to prove they are dropped.
SAAS_NUM_ROWS = 12
SAAS_EMBEDDING_DIMS = 8
# The engineered numeric columns + the label, in the order the golden projects.
SAAS_PROJECTION = [
    ("messages", pa.int64(), "int64",
     [12, 4, 31, 7, 19, 2, 26, 9, 14, 5, 22, 1]),
    ("avg_response_seconds", pa.float64(), "float64",
     [45.5, 320.0, 12.25, 88.75, 5.5, 600.0, 33.0, 410.5, 60.0, 150.25, 8.0, 720.0]),
    ("sentiment", pa.float64(), "float64",
     [0.82, -0.45, 0.91, 0.13, 0.77, -0.88, 0.5, -0.2, 0.66, 0.08, 0.95, -0.97]),
    ("questions_asked", pa.int64(), "int64",
     [3, 0, 6, 1, 4, 0, 5, 1, 2, 0, 7, 0]),
    ("outcome", pa.int64(), "int64",
     [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0]),
]

saas_columns = {name: pa.array(values, type=pa_type)
                for name, pa_type, _dtype, values in SAAS_PROJECTION}
# The embedding columns we DROP — deterministic floats, exact in float32.
for dim in range(SAAS_EMBEDDING_DIMS):
    saas_columns[f"embedding_{dim}"] = pa.array(
        [float((dim + (row * 0.0625)) % 1.0) for row in range(SAAS_NUM_ROWS)],
        type=pa.float32(),
    )

SAAS_TABLE = pa.table(saas_columns)
# Two row groups (6 + 6) so the reader concatenates chunks across row groups.
pq.write_table(
    SAAS_TABLE,
    SAAS_PARQUET_PATH,
    compression="snappy",
    use_dictionary=False,
    row_group_size=6,
    version="2.6",
)

saas_golden = {
    "num_rows": SAAS_NUM_ROWS,
    "columns": [
        {"name": name, "dtype": dtype, "values": values}
        for name, _pa_type, dtype, values in SAAS_PROJECTION
    ],
}
with open(SAAS_GOLDEN_PATH, "w") as f:
    json.dump(saas_golden, f, indent=2)
    f.write("\n")

print("wrote", SAAS_PARQUET_PATH, os.path.getsize(SAAS_PARQUET_PATH), "bytes")
print("wrote", SAAS_GOLDEN_PATH)


# --- churn fixture as Parquet (#508) ----------------------------------------
#
# Write the canonical churn JSONL (the source of truth) to Parquet with explicit
# types, so load_churn_dataset(..., Parquet) reads it through the native reader
# and round-trips to the SAME dataset as JSONL/CSV. Uncompressed/snappy + page v1
# only (the codecs/page version the native decoders read).
def read_jsonl(path):
    rows = []
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def write_records_parquet(rows, schema, out_path):
    columns = {
        field.name: pa.array([row[field.name] for row in rows], type=field.type)
        for field in schema
    }
    table = pa.table(columns, schema=schema)
    pq.write_table(
        table,
        out_path,
        compression="snappy",
        use_dictionary=True,
        version="2.6",
    )
    print("wrote", out_path, os.path.getsize(out_path), "bytes")


ACCOUNTS_SCHEMA = pa.schema([
    pa.field("account_id", pa.string(), nullable=False),
    pa.field("name", pa.string(), nullable=False),
    pa.field("brand", pa.string(), nullable=False),
    pa.field("entity", pa.string(), nullable=False),
    pa.field("plan_tier", pa.string(), nullable=False),
    pa.field("industry", pa.string(), nullable=False),
    pa.field("region", pa.string(), nullable=False),
    pa.field("mrr", pa.float64(), nullable=False),
    pa.field("seats", pa.int64(), nullable=False),
    pa.field("tenure_days", pa.int64(), nullable=False),
    pa.field("renewal_date", pa.string(), nullable=False),
    pa.field("churned", pa.bool_(), nullable=False),
    pa.field("churn_date", pa.string(), nullable=False),
])
EVENTS_SCHEMA = pa.schema([
    pa.field("event_id", pa.string(), nullable=False),
    pa.field("account_id", pa.string(), nullable=False),
    pa.field("event_ts", pa.string(), nullable=False),
    pa.field("event_type", pa.string(), nullable=False),
    pa.field("channel", pa.string(), nullable=False),
    pa.field("mention", pa.string(), nullable=False),
    pa.field("value", pa.float64(), nullable=False),
])

write_records_parquet(
    read_jsonl(os.path.join(CHURN_DIR, "accounts.jsonl")),
    ACCOUNTS_SCHEMA, CHURN_ACCOUNTS_PARQUET)
write_records_parquet(
    read_jsonl(os.path.join(CHURN_DIR, "events.jsonl")),
    EVENTS_SCHEMA, CHURN_EVENTS_PARQUET)
