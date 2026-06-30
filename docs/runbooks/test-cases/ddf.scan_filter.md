# Scan + filter · `ddf.scan_filter`

**Category:** DDF dataframe ops · **Source:** `src/core/testkit/cases_ddf.cpp` · **Engine doc:** [ddf-ops.md](../../ddf-ops.md)

Scan a table into a typed columnar [`DataFrame`](../../ddf.md), then filter it with the
device gather op and check the surviving row count against an independent host
predicate. The leaf DDF ops, exercised end to end on a [`CpuDevice`](../../cpu-device.md).

## What it does

1. **Builds a numeric table.** A synthetic in-memory `JsonStore` of `rows` rows,
   each `{entity: i (Int64), value: i % 10 (Float64)}` — a known, deterministic
   distribution so the host can re-derive the expected answer.
2. **Scans it into a frame.** [`ddf::scan`](../../ddf-ops.md) reads the store into a
   `DataFrame` over the schema `{entity Int64, value Float64}` and the case asserts
   the scanned frame has exactly `rows` rows.
3. **Filters on the device.** A `FilterPredicate` `value >= threshold` runs through
   [`ddf::filter`](../../ddf-ops.md), which moves the surviving rows with the device
   **gather** op (per chunk, then re-packs into regular output chunks).
4. **Checks against the host.** It counts, in plain host code, how many rows satisfy
   `(i % 10) >= threshold` and asserts the filtered frame's row count matches — so the
   device path is verified against an independent reference.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ DDF dataframe ops → Scan + filter**. Set the options in the modal and click
**Run**; the result pane shows pass/fail, the metrics (scanned / kept), and the log.

**Headless:** the case is exercised (at `rows=500`, default `threshold`) by
`tests/test_testkit.cpp::DdfCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DdfCasesPass' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `rows` | 5000 | 1–5000000 | rows scanned into the DataFrame |
| `threshold` | 5.0 | 0.0–10.0 | keep rows whose `value` (`i % 10`) is `>=` this |

## What it asserts (pass criteria)

- The scanned frame has exactly `rows` rows (the scan round-trips the table).
- `ddf::filter` succeeds and the filtered frame's row count equals the host count of
  rows with `(i % 10) >= threshold` — the device gather op matched the host predicate
  field for field.

## Related

- Engine: [ddf-ops.md](../../ddf-ops.md) · [ddf.md](../../ddf.md) ·
  [compute-device.md](../../compute-device.md)
- Sibling cases: [`ddf.group_by`](ddf.group_by.md) ·
  [`ddf.parquet_reader_parity`](ddf.parquet_reader_parity.md)
