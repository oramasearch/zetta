# CPU reference backend (`zetta::compute::CpuDevice`)

`CpuDevice` is the **always-built reference backend** of the
[`ComputeDevice`](compute-device.md) seam — the operator-level accelerator
interface of **ADR-27 / ARC-9**, the native distributed accelerated-compute pass
for UC-1 churn ([#360](https://github.com/oramasearch/zetta/issues/360), part of
[#264](https://github.com/oramasearch/zetta/issues/264)). It implements **every**
declared operator on host memory, parallelizing the data-parallel ones across the
[`zetta::runtime`](../architecture/runtime.md) thread pool. Its results are the **numerical
reference** the optional CUDA backend
([#361](https://github.com/oramasearch/zetta/issues/361)) is checked against, so
**correctness — not peak throughput — is the contract.**

```
include/zetta/compute/cpu_device.hpp   the CpuDevice class
src/core/compute/cpu_device.cpp        the operator implementations
tests/compute/test_cpu_device.cpp              one numerical test per operator (the reference)
```

## CpuDevice vs. HostStubDevice

Both live in host RAM and share the same memory model (allocate is `new[]`,
transfer is `memcpy`). The difference is the operators:

| | memory model | operators |
| --- | --- | --- |
| [`HostStubDevice`](compute-device.md) (#359) | host RAM | all `Unsupported` |
| **`CpuDevice` (this, #360)** | host RAM | **all implemented**, multi-threaded |

`HostStubDevice` existed only to ship and test the interface + memory model on
their own. `CpuDevice` is the real backend you compute with on any machine — it
needs no GPU, no SDL, no CUDA, and builds CPU-only (dev Macs, CI).

## The memory model

Identical to the stub's: a [`DeviceBuffer`](compute-device.md#the-memory-model-devicebuffer)
owns a `std::byte[]` on the host heap, minted through the device and freed through
it (`release_buffer` is `delete[]`). `allocate` returns an empty buffer on
allocation failure rather than throwing; `copy_from_host` / `copy_to_host` are
bounds-checked `memcpy`s; `synchronize()` is a no-op because `parallel_for` joins
before returning, so all issued work is already complete.

## The operators

Every operator validates its arguments (matching dtypes, element counts, shapes;
non-empty buffers) and returns a [`ComputeStatus`](compute-device.md#error-model-computestatus)
— `Ok`, or `InvalidArgument` with a reason. Nothing throws across the seam.

| Operator | Behavior | Threading |
| --- | --- | --- |
| `map(UnaryOp)` | `out[i] = op(in[i])` — negate, abs, square (all dtypes); sqrt, exp, log (float only) | element-parallel |
| `elementwise(BinaryOp)` | `out[i] = op(a[i], b[i])` — add, sub, mul, div, max, min | element-parallel |
| `reduce(ReduceOp)` | `out[0] = op` over all elements — sum, product, min, max | chunk-local partials, merged |
| `scan(ReduceOp, inclusivity)` | prefix scan, inclusive or exclusive | parallel three-phase |
| `histogram` | count into `bin_count` equal-width bins over `[range_min, range_max)` | chunk-local bins, merged |
| `gemm(GemmParams)` | `C = α·op(A)·op(B) + β·C`, row-major, transpose flags (float only) | parallel over rows |
| `axpy(α)` | `y[i] = α·x[i] + y[i]` (float only) | element-parallel |
| `sort` | sort keys in place, ascending / descending | sequential (see below) |
| `sort_by_key` | sort keys + apply the same permutation to values | sequential (see below) |
| `gather` | `out[i] = in[indices[i]]` | element-parallel |
| `scatter` | `out[indices[i]] = in[i]` | sequential (see below) |

### Numeric / dtype notes

- **Float-only operators.** `sqrt` / `exp` / `log`, `gemm`, and `axpy` require a
  floating-point dtype (`Float32` / `Float64`) and return `InvalidArgument` for
  integer buffers — the math isn't well defined over the integers, so the
  reference rejects rather than guesses.
- **Integer divide-by-zero** returns `0` (rather than the hardware's undefined
  behavior) so a stray zero divisor can't crash the node; float division follows
  IEEE (`inf` / `nan`).
- **`gemm` with `β == 0`** does not read `C` (BLAS convention), so an
  uninitialized or `NaN`-laden `C` does not poison the result.
- **`histogram`** counts values into half-open bins; values outside
  `[range_min, range_max)` are dropped. `output_counts` must be an integer dtype
  sized to `bin_count`.

### Why three operators run sequentially

`sort` and `sort_by_key` use a single comparison sort (`std::sort` /
`std::stable_sort`) — a correct parallel merge sort is a future optimization, and
the reference's job is the right answer. `scatter` runs sequentially **on
purpose**: duplicate indices would be a write race in parallel, so running in
input order makes "last writer wins" deterministic. `sort_by_key` sorts an index
permutation by key (stable, so equal keys keep input order — the documented
"rank entities by risk score, carry their ids" use case) and then reorders the
keys (typed) and the values (by raw element size) through it.

## Multi-threading: `runtime::parallel_for`

The data-parallel operators don't spawn threads themselves — they call
[`zetta::runtime::parallel_for`](../architecture/runtime.md#data-parallelism-parallel_for), the
seam's blocking fan-out primitive over one shared, process-wide thread pool. It
tiles `[0, count)` into contiguous chunks (one per worker, above a grain
threshold), runs each chunk on a pool worker, and joins before returning. So
`CpuDevice` names no backend type directly — the Capy thread pool stays behind the
runtime seam, exactly as the rest of zetta uses it.

Reductions follow the standard pattern: each chunk computes a local partial and
merges it into a shared accumulator under a mutex (merged once per chunk, not per
element). The prefix `scan` is a three-phase parallel scan (per-chunk reduction →
sequential prefix of the chunk totals → per-chunk scan seeded with the chunk's
offset); it relies on `parallel_for` tiling a given `count` identically across
calls, which the primitive guarantees.

## Tests

`tests/compute/test_cpu_device.cpp` is the reference suite: one correctness test per
operator (exact expected results), argument-validation tests (dtype / shape /
out-of-range rejection), and large-input cases (≥ 500k–1M elements) that force
the multi-chunk path so the threaded reduce / scan / map are exercised, not just
the inline fast path. `tests/test_runtime.cpp` covers `parallel_for` itself
(disjoint tiling, contiguous chunks, parallel-vs-sequential sum, the inline
small-range path, and that a throwing body aborts).

## What's next

- **`CudaDevice`** ([#361](https://github.com/oramasearch/zetta/issues/361)) — the
  GPU backend, checked against these CPU results.
- **Device registry** ([#362](https://github.com/oramasearch/zetta/issues/362)) —
  enumerate a node's devices and select a backend per operation.
- **CMake `zetta-compute` target**
  ([#363](https://github.com/oramasearch/zetta/issues/363)) — house the backends,
  optional CUDA detection without leaking CUDA headers.

See also [compute-device.md](compute-device.md) (the interface), [runtime.md](../architecture/runtime.md)
(`parallel_for`), and [architecture.md](../architecture/architecture.md).
