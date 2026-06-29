#!/usr/bin/env python3
"""Generate the committed Parquet metadata test fixture + golden (DEV-TIME ONLY).

zetta's native Parquet footer/metadata parser (ADR-32, issue #505) is verified
against a golden produced here by Apache Arrow's pyarrow — an *independent*
reference for the schema + row-group / column-chunk layout the native parser
extracts from the file's Thrift `FileMetaData`.

This script is NOT part of the build or CI (ADR-17 — no foreign runtime in the
engine / build / CI). It is run by hand, once, to (re)generate two committed
artifacts under tests/fixtures/parquet/:

  * churn_sample.parquet            — a small, representative Parquet file
  * churn_sample.metadata.json      — pyarrow's dump of its metadata, in the
                                      exact shape zetta::parquet::to_json emits

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
