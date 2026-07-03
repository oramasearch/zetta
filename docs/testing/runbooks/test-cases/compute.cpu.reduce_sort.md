# CPU device: reduce + sort_by_key · `compute.cpu.reduce_sort`

**Category:** Compute & device · **Source:** `src/core/testkit/cases_compute.cpp` · **Engine doc:** [cpu-device.md](../../../compute/cpu-device.md)

Exercises the reduce and sort operators of the [`ComputeDevice`](../../../compute/compute-device.md)
seam on the always-built [`CpuDevice`](../../../compute/cpu-device.md) reference backend: sum a
vector (checked exactly against the host) then sort `(key, value)` pairs ascending
and confirm each value is carried along with its key.

## What it does

1. **Builds and reduces.** Constructs a `CpuDevice`, fills a `count`-length double
   vector (`values[i] = (i mod 17) − 4`), uploads it, runs `reduce(Sum)` into a
   single-element buffer, and compares the device total to the host sum.
2. **Sorts by key.** Uploads descending keys (`keys[i] = count − i`) paired with
   each element's original index as the value (`payload[i] = i`), then runs
   `sort_by_key(keys, values, Ascending)`; a non-ok `ComputeStatus` at either step
   fails the case.
3. **Verifies ordering and carry.** Confirms the keys come back non-descending, and
   that the value carried with each key `k` equals `count − k`, proving the values
   followed their keys through the permutation.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Compute & device → CPU device: reduce + sort_by_key**. Set the options in
the modal and click **Run**; the result pane shows pass/fail, the metrics (element
count, reduce sum), and the log.

**Headless:** the case is exercised (at `count=256`) by
`tests/test_testkit.cpp::ComputeCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ComputeCasesPass' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `count` | 1024 | 2–2000000 | how many elements to reduce / sort |

`sort_by_key` is the operator the [batch scorer](../../../ml/gbdt-score.md) uses to rank
at-risk entities by score while carrying their ids, which this case verifies in the
small.

## What it asserts (pass criteria)

- The `reduce(Sum)` and `sort_by_key` calls both return an ok `ComputeStatus`.
- The device reduce sum equals the host sum within `1e-6` (an exact check, the
  values are small integers).
- After the ascending sort the keys are non-descending, and each value is still
  paired with its key (`value == count − key`), so no value was dropped or
  mis-permuted.
- The result reports the element count and the reduce sum as metrics.

## Related

- Engine: [cpu-device.md](../../../compute/cpu-device.md) ·
  [compute-device.md](../../../compute/compute-device.md)
- Sibling cases: [`compute.cpu.elementwise`](compute.cpu.elementwise.md) ·
  [`compute.cuda.conformance`](compute.cuda.conformance.md)
