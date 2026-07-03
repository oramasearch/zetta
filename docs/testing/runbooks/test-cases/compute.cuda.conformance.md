# CUDA backend: build state · `compute.cuda.conformance`

**Category:** Compute & device · **Source:** `src/core/testkit/cases_compute.cpp` · **Engine doc:** [compute-device.md](../../../compute/compute-device.md)

Reports whether this build carries the optional [`CudaDevice`](../../../compute/compute-device.md)
GPU backend. It is a build-state probe, not the parity check itself: the real
CPU↔CUDA operator conformance suite runs as a gtest. **Passes** when built with CUDA
and is **skipped** on a CPU-only build (the default).

## What it does

1. **Reads the build flag.** The case branches on the public `ZETTA_WITH_CUDA`
   compile definition, which is set only when the project is configured with
   `-DZETTA_ENABLE_CUDA=ON` (the opt-in CUDA build).
2. **CUDA build → pass.** When the backend is compiled in, the case **passes**,
   reporting `cuda = available` and noting that the CPU↔CUDA operator parity suite
   runs as `tests/compute/test_cuda_device.cpp`.
3. **CPU-only build → skip.** On the default CPU-only build no CUDA backend is
   present, so the case is **skipped** with `cuda = not built` and a hint to
   configure with `-DZETTA_ENABLE_CUDA=ON` on a CUDA host. It computes nothing
   itself: the actual operator parity assertions live in the gtest build.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Compute & device → CUDA backend: build state**. Click **Run**; the result
pane shows pass (on a CUDA build) or skipped (CPU-only), the `cuda` metric, and the
log.

**Headless:** the case is exercised by
`tests/testkit/test_testkit.cpp::ComputeCasesPass`, which accepts either Pass (CUDA build)
or Skipped (CPU-only build):

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ComputeCasesPass' --output-on-failure
```

### Options

This case takes no options.

## What it asserts (pass criteria)

- On a CUDA build (`-DZETTA_ENABLE_CUDA=ON`): the case **passes**, reporting
  `cuda = available`.
- On a CPU-only build (the default): the case is **skipped** (`cuda = not built`),
  never failed; the `ComputeCasesPass` gtest treats both Pass and Skipped as
  acceptable.
- The actual operator parity, every operator agreeing between `CpuDevice` and
  `CudaDevice` plus the edge-case parity (integer-dtype rejection, safe integer
  divide, bounds-checked `gather`/`scatter`, deterministic duplicate-index
  `scatter`), is asserted by `tests/compute/test_cuda_device.cpp`, which compiles only in a
  CUDA build and runs only on a host with a GPU.

## Related

- Engine: [compute-device.md](../../../compute/compute-device.md) ·
  [cpu-device.md](../../../compute/cpu-device.md)
- Sibling cases: [`compute.cpu.elementwise`](compute.cpu.elementwise.md) ·
  [`compute.cpu.reduce_sort`](compute.cpu.reduce_sort.md)
