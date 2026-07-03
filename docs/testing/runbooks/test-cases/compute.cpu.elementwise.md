# CPU device: map + elementwise · `compute.cpu.elementwise`

**Category:** Compute & device · **Source:** `src/core/testkit/cases_compute.cpp` · **Engine doc:** [cpu-device.md](../../../compute/cpu-device.md)

Exercises two leaf operators of the [`ComputeDevice`](../../../compute/compute-device.md) seam
on the always-built [`CpuDevice`](../../../compute/cpu-device.md) reference backend: upload two
float vectors, run an elementwise add then a unary square, and check the device
result against the same computation done on the host.

## What it does

1. **Builds a CpuDevice and uploads.** Constructs a `CpuDevice`, fills two
   `count`-length float vectors (`a[i] = i · 0.5`, `b[i] = count − i`), and uploads
   each into a `DeviceBuffer` via the typed `upload<float>` helper.
2. **Elementwise add.** Runs `elementwise(Add, a, b)` into a sum buffer; a non-ok
   `ComputeStatus` fails the case.
3. **Unary square.** Runs `map(Square, sum)` into a result buffer; a non-ok status
   fails the case.
4. **Downloads and compares.** Downloads the squared result and compares each
   element to the host value `(a[i] + b[i])²`, tracking the maximum absolute error
   across all elements.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Compute & device → CPU device: map + elementwise**. Set the options in the
modal and click **Run**; the result pane shows pass/fail, the metrics (element
count, max `|error|`), and the log.

**Headless:** the case is exercised (at `count=256`) by
`tests/test_testkit.cpp::ComputeCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ComputeCasesPass' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `count` | 1024 | 1–2000000 | how many floats to process |

A large `count` forces the multi-chunk path through
[`runtime::parallel_for`](../../../architecture/runtime.md), so the threaded operators are exercised
rather than just the inline fast path.

## What it asserts (pass criteria)

- The `elementwise(Add)` and `map(Square)` calls both return an ok `ComputeStatus`.
- The device result matches the host `(a + b)²` within a maximum absolute error of
  `1e-2` over all `count` elements (the small tolerance absorbs float32 rounding).
- The result reports the element count and the observed max `|error|` as metrics.

## Related

- Engine: [cpu-device.md](../../../compute/cpu-device.md) ·
  [compute-device.md](../../../compute/compute-device.md)
- Sibling cases: [`compute.cpu.reduce_sort`](compute.cpu.reduce_sort.md) ·
  [`compute.cuda.conformance`](compute.cuda.conformance.md)
