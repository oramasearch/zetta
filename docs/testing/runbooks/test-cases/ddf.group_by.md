# Group-by aggregate · `ddf.group_by`

**Category:** DDF dataframe ops · **Source:** `src/core/testkit/cases_ddf.cpp` · **Engine doc:** [ddf-groupby.md](../../../ddf/ddf-groupby.md)

Group a [`DataFrame`](../../../ddf/ddf.md) by a key column and compute `count` + `sum`, then
check each group against a host map built alongside the data. The single-node
groupby-aggregate op verified group by group.

## What it does

1. **Builds a numeric table + a host oracle.** A synthetic `JsonStore` of `rows`
   rows, each `{key: i % groups (Int64), amount: (i % 7) + 1 (Float64)}`; as it
   writes each row it accumulates the expected per-group `count` and `sum` into
   `std::map`s — the independent reference.
2. **Scans it into a frame.** [`ddf::scan`](../../../ddf/ddf-ops.md) reads the store into a
   `DataFrame` over the schema `{key Int64, amount Float64}`.
3. **Runs the groupby.** A `GroupBySpec` keyed on `key` with `Count` + `Sum(amount)`
   runs through [`ddf::group_by`](../../../ddf/ddf-groupby.md), which reads the columns back
   through the [`ComputeDevice`](../../../compute/compute-device.md) download seam and groups in
   ascending key order (deterministic).
4. **Checks group by group.** It asserts the group count matches the host map, that
   the `count` and `sum_amount` result columns are present, and that every group's
   `count` matches exactly and `sum` matches within `1e-6` of the host total.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ DDF dataframe ops → Group-by aggregate**. Set the options in the modal and click
**Run**; the result pane shows pass/fail, the metrics (rows / groups), and the log.

**Headless:** the case is exercised (at `rows=500`, default `groups`) by
`tests/testkit/test_testkit.cpp::DdfCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DdfCasesPass' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `rows` | 5000 | 1–5000000 | rows grouped into the frame |
| `groups` | 4 | 1–1024 | distinct group keys (`i % groups`) |

## What it asserts (pass criteria)

- `ddf::group_by` succeeds and produces exactly as many groups as the host map.
- Both result columns (`count`, `sum_amount`) are present.
- Every group's `count` equals the host count exactly and its `sum` matches the host
  total within `1e-6` — the device-backed aggregate agreed with the host oracle.

## Related

- Engine: [ddf-groupby.md](../../../ddf/ddf-groupby.md) · [ddf.md](../../../ddf/ddf.md) ·
  [compute-device.md](../../../compute/compute-device.md)
- Sibling cases: [`ddf.scan_filter`](ddf.scan_filter.md) ·
  [`ddf.parquet_reader_parity`](ddf.parquet_reader_parity.md)
- Distributed counterpart: [`cluster.group_by_consistency`](cluster.group_by_consistency.md) ·
  [`cluster.compute_near_data`](cluster.compute_near_data.md)
