# Parquet reader parity (TC-9) · `ddf.parquet_reader_parity`

**Category:** DDF dataframe ops · **Source:** `src/core/testkit/cases_parquet.cpp` · **Engine doc:** [parquet-reader.md](../../parquet-reader.md)

The in-app form of **TC-9** ([#499](https://github.com/oramasearch/zetta/issues/499)):
load a projected subset of a pyarrow-produced Parquet file into a DDF
[`DataFrame`](../../ddf.md) with the native (no-Arrow) [reader](../../parquet-reader.md)
and match a committed pyarrow golden on schema, row count, dtypes, and values —
dropping the `embedding_*` columns via column projection. The fixture + golden are
embedded in the binary, so the case needs no on-disk file.

## What it does

1. **Decodes the embedded fixture + golden.** The pyarrow-produced
   `saas_sample.parquet` bytes (base64) and the golden DataFrame JSON ride in the
   binary (`parquet_fixture_data.hpp`); the golden re-enters through its validated
   schema boundary (`validate_parquet_data_frame_json`), so a malformed embed fails
   fast.
2. **Projects exactly the golden's columns.** It builds a `DataFrameSchema` of the
   golden's columns (the engineered numeric columns + `outcome`) at their declared
   dtypes — the `embedding_*` columns are deliberately *not* requested — and runs
   [`load_parquet_data_frame`](../../parquet-reader.md) at the selected `chunk_rows`.
3. **Checks four axes.** Row count, schema (column names + order, with an explicit
   assertion that no `embedding_` column leaked), produced dtype per column, and
   values column by column (ints exactly, float64 to full precision).
4. **Proves projection, not inference, dropped the embeddings.** It re-runs the
   reader's metadata-only [`infer_parquet_schema`](../../parquet-reader.md) and
   asserts the inferred dtypes agree with pyarrow for every projected column **and**
   that inference *sees* the embeddings (as `float32`) it drops — so the check isn't
   circular on the golden-imposed load.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ DDF dataframe ops → Parquet reader parity (TC-9)**. Set the options in the modal
and click **Run**; the result pane shows pass/fail, the metrics (rows / columns /
embeddings dropped / chunk rows), and the log.

**Headless:** the case is exercised by
`tests/test_testkit.cpp::ParquetReaderParityCasePasses` — at the default chunk size
**and** at a tiny `chunk_rows=5` (each column splits across chunks yet still matches
the golden):

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ParquetReaderParityCasePasses' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `chunk_rows` | 10000 | 1–1000000 | rows per DataFrame chunk; shrink to split each column across chunks while values stay identical |

## What it asserts (pass criteria)

- The loaded frame's row count equals the golden's `num_rows`.
- The produced columns are exactly the golden's, in order, with no `embedding_`
  column present (column projection dropped all 3072).
- Each column's produced dtype matches the golden, and its values match field for
  field (ints exactly, float64 to full precision).
- Metadata-only inference agrees with pyarrow on every projected column's dtype and
  *sees* the embeddings (as `float32`) it omits — so projection, not inference, is
  what dropped them.

## Related

- Engine: [parquet-reader.md](../../parquet-reader.md) ·
  [parquet-metadata.md](../../parquet-metadata.md) ·
  [parquet-column-decoders.md](../../parquet-column-decoders.md) · [ddf.md](../../ddf.md)
- Sibling cases: [`ddf.scan_filter`](ddf.scan_filter.md) ·
  [`ddf.group_by`](ddf.group_by.md)
