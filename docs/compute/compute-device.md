# Compute device interface (`zetta::compute`)

The **`ComputeDevice`** interface is the single seam every heavy compute operator
in zetta targets — the operator-level accelerator interface of blueprint
**ADR-27 / ARC-9**, the native distributed accelerated-compute pass for UC-1
churn ([#264](https://github.com/oramasearch/zetta/issues/264)). It is the same
shape as zetta's other seams ([`StepExecutor`](../pipeline/execution.md),
[`Check`](../pipeline/gate.md), [`StorageEngine`](../data/storage.md)): a pure-virtual base class
whose concrete implementations are the **backends**. Consumers call the
interface and stay unchanged when new hardware appears — **new hardware = a new
backend, nothing else.**

This page documents the **interface, the device buffer / memory model, the
host-memory stub** ([#359](https://github.com/oramasearch/zetta/issues/359)), and
the **`zetta-compute` build target + the optional CUDA backend**
([#363](https://github.com/oramasearch/zetta/issues/363)). The default build is
CPU-only; the GPU backend is opt-in.

```
include/zetta/compute/data_type.hpp       DataType tag + byte_size / to_string / data_type_of<T>
include/zetta/compute/compute_status.hpp  ComputeStatus + ComputeStatusCode (the no-throw result)
include/zetta/compute/device_buffer.hpp   DeviceBuffer — typed, owning, move-only device memory
include/zetta/compute/compute_device.hpp  ComputeDevice — the seam: info + memory + operators
include/zetta/compute/host_stub_device.hpp HostStubDevice — host-memory transfer stub
include/zetta/compute/cuda_device.hpp     CudaDevice — the GPU backend (CUDA-free header)
src/core/compute/device_buffer.cpp        buffer lifetime + ComputeDevice::make_buffer
src/core/compute/host_stub_device.cpp     the stub implementation
src/core/compute/cuda_device.cu           the CUDA backend (kernels; built only with CUDA)
```

These sources build into a dedicated **`zetta-compute`** static library that
`zetta-core` links — see [Build: `zetta-compute` + optional CUDA](#build-zetta-compute--optional-cuda)
below.

## Where this sits in the "accel" family

This is the **first** of five issues; the rest build on it:

| Issue | What it adds | Status |
| --- | --- | --- |
| **#359** | the `ComputeDevice` interface + `DeviceBuffer` memory model + `HostStubDevice` | built |
| [#360](https://github.com/oramasearch/zetta/issues/360) | **`CpuDevice`** — the always-built reference backend, operators on host memory via the [`zetta::runtime`](../architecture/runtime.md) thread pool ([cpu-device.md](cpu-device.md)) | built |
| [#361](https://github.com/oramasearch/zetta/issues/361) | **`CudaDevice`** — the optional CUDA backend (memory model + operator kernels) | built (alongside #363) |
| [#362](https://github.com/oramasearch/zetta/issues/362) | a **device registry** — enumerate a node's devices, report each one's capability, select a backend per op (see [device-registry.md](device-registry.md)) | built |
| **#363** | **CMake** — the `zetta-compute` static target + optional CUDA detection, linked into `zetta-core` without leaking CUDA headers | built |

> The CMake plumbing for #363 needs *something* CUDA to compile to prove the
> guard works end to end, and #363 depends only on the interface (not on #361), so
> `CudaDevice` and its kernels first shipped **together** with the build target.
> #361 then brought those kernels into **conformance with the `CpuDevice`
> reference** — matching its per-operator preconditions and edge cases
> (integer-dtype rejection, safe integer divide, bounds-checked `gather`/`scatter`,
> deterministic duplicate-index `scatter`) — and added the CPU↔CUDA conformance
> suite (`tests/test_cuda_device.cpp`). It remains the home for any further GPU
> operators (DDF / GBDT / SHAP) layered on this same seam.

`HostStubDevice` is **not** the CPU backend. It is a memory/transfer stub used to
ship and test the interface on its own; the real, operator-implementing CPU
device is `CpuDevice` (#360).

## Altitude: the interface, not the kernels

The interface slice declares the seam and proves the **memory model**
round-trips; the **`HostStubDevice`** implements no operator — every operator on
it returns `ComputeStatus::unsupported`. Real kernels plug in behind the seam
**without touching consumers** — exactly the shape of the gate's `Check` seam and
the storage engine's `StorageEngine` — which is precisely how the
[`CudaDevice`](#the-cuda-backend-cudadevice) GPU kernels land. No SDL/CUDA type
appears in any header (the CUDA backend's header is CUDA-free), and the **default
build is CPU-only** — pure `zetta-core`, compiling on a machine with no GPU
toolkit (dev Macs, CI).

## The memory model: `DeviceBuffer`

A `DeviceBuffer` is a **typed, owning, move-only** handle to a region of device
memory — a `std::unique_ptr` for whatever the owning device allocated (host RAM
for a CPU device, GPU VRAM for a CUDA device). Memory is **opaque**: the buffer
carries an element type ([`DataType`](#data-types-datatype)), an element count,
and a backend-defined handle it never interprets — only the device that minted it
does.

- **Owning + RAII** — the destructor returns the storage to its device; a move
  transfers ownership and empties the source; copies are deleted (a handle is
  never double-freed). The opaque handle is the one raw pointer the design allows
  (an unavoidable C-API-style resource handle), fully encapsulated and
  RAII-managed.
- **Minted only by a device** — the constructor is private; a `ComputeDevice`
  builds one through its protected `make_buffer`, and a buffer frees itself
  through its device's `release_buffer`. Minting and freeing both stay inside the
  device, so freeing is polymorphic per backend.
- `empty()` is true for a default-constructed or moved-from buffer, and is what
  `allocate()` returns on failure (no exception).

## The interface: `ComputeDevice`

```cpp
class ComputeDevice {
public:
    virtual DeviceInfo info() const = 0;                         // identity + coarse capability

    // --- memory model ---
    virtual DeviceBuffer  allocate(DataType, std::size_t count) = 0;
    virtual ComputeStatus copy_from_host(const void* src, DeviceBuffer& dst, std::size_t bytes) = 0;
    virtual ComputeStatus copy_to_host(const DeviceBuffer& src, void* dst, std::size_t bytes) const = 0;
    virtual ComputeStatus synchronize() = 0;                     // barrier before reading results

    // --- operators (the declared coarse-grained set) ---
    virtual ComputeStatus map(UnaryOp, …) = 0;          // out[i] = op(in[i])
    virtual ComputeStatus elementwise(BinaryOp, …) = 0; // out[i] = op(a[i], b[i])
    virtual ComputeStatus reduce(ReduceOp, …) = 0;      // out[0] = op over in
    virtual ComputeStatus scan(ReduceOp, ScanInclusivity, …) = 0;  // prefix scan
    virtual ComputeStatus histogram(…, bin_count, range_min, range_max, …) = 0;
    virtual ComputeStatus gemm(const GemmParams&, …) = 0;          // C = αAB + βC
    virtual ComputeStatus axpy(double alpha, …) = 0;               // y = αx + y
    virtual ComputeStatus sort(DeviceBuffer& keys, SortOrder) = 0;
    virtual ComputeStatus sort_by_key(DeviceBuffer& keys, DeviceBuffer& values, SortOrder) = 0;
    virtual ComputeStatus gather(…, indices, …) = 0;    // out[i] = in[indices[i]]
    virtual ComputeStatus scatter(…, indices, …) = 0;   // out[indices[i]] = in[i]

    // typed convenience over the byte API
    template <class Element> DeviceBuffer  upload(std::span<const Element>);
    template <class Element> ComputeStatus download(const DeviceBuffer&, std::span<Element>) const;
};
```

### The operator set

Operators are declared at the **largest grain that still lets the cluster
coordinator distribute the work** — the primitives the churn pipeline (and later
DDF, GBDT, and SHAP ops, added by their own issues against this interface)
compose:

| Bucket | Operators | Used by |
| --- | --- | --- |
| map / elementwise | `map` (unary), `elementwise` (binary) | feature transforms |
| reduce / scan | `reduce`, `scan` | aggregates, prefix sums |
| histogram | `histogram` | GBDT split finding |
| gemm / axpy | `gemm`, `axpy` | linear algebra, gradient steps |
| sort | `sort`, `sort_by_key` | ranking at-risk entities by score |
| gather / scatter | `gather`, `scatter` | feature/index reshuffles |

Each operator returns a [`ComputeStatus`](#error-model-computestatus) rather than
throwing. A backend that does not implement an op returns
`ComputeStatus::unsupported`, so the registry/coordinator (#362) can route the
work to a device that does. There is **no separate per-op capability bitmask** —
the return status *is* the source of truth for "can this device do this op",
which keeps the two from drifting (the same single-source-of-truth discipline the
rest of zetta follows).

### Data types (`DataType`)

Because `ComputeDevice` is runtime-polymorphic, its methods cannot be templated,
so a buffer carries its element type as a runtime `DataType` tag rather than a
template parameter — a buffer is *typed* without forcing template instantiations
across the virtual interface, and backends dispatch their kernels on the tag. The
closed set is `Float32`, `Float64`, `Int32`, `Int64`, `UInt32`, `UInt64`,
`UInt8` (floats for features/gradients, signed ints for indices/bin ids, unsigned
for counts/masks). `data_type_of<Element>` bridges C++ types to the tag for the
typed helpers (`data_type_of<float> == DataType::Float32`).

### Error model (`ComputeStatus`)

Operators are **exception-free**: `zetta-core` compiles with
`-fexperimental-library` and must never unwind across a seam, and a backend
(especially CUDA) reports failure with a code, not a C++ exception. So every call
returns a `ComputeStatus` — `Ok`, or one of `Unsupported` / `InvalidArgument` /
`OutOfMemory` / `BackendError` plus a human-readable reason — exactly like
[`ir::from_json`](../pipeline/execution-graph.md) returns its optional + reason at the IR
trust boundary.

> **No JSON boundary here.** Unlike the [Run](../pipeline/runs.md) and
> [audit](../governance/audit-trail.md) records, this slice introduces **no JSON format** — it
> is a pure in-process C++ interface that neither stores nor parses a document,
> so there is no schema to validate. The registry slice
> ([#362](device-registry.md)) is where a device's `DeviceInfo` is serialized for
> the coordinator, and that is where the JSON Schema was added, at that boundary
> (`schemas/device_registry.schema.json`).

## Using it: the host⇄device round-trip

```cpp
#include "zetta/compute/host_stub_device.hpp"

zetta::compute::HostStubDevice device;

std::vector<float> source = {1.0f, -2.5f, 3.25f, 42.0f};
zetta::compute::DeviceBuffer buffer = device.upload<float>(source);   // host -> device

std::vector<float> back(source.size());
zetta::compute::ComputeStatus status = device.download<float>(buffer, back);  // device -> host
// status.ok() && back == source
```

`upload<T>` / `download<T>` attach the right `DataType` tag and validate
dtype/size before transferring; the raw byte-oriented `copy_from_host` /
`copy_to_host` are the virtual primitives they call (and what a backend
implements). A backend whose transfers/operators run asynchronously (a CUDA
stream) implements `synchronize()` as the barrier before reading results back;
the synchronous `HostStubDevice` makes it a no-op.

## Build: `zetta-compute` + optional CUDA

The compute backends build into a dedicated **`zetta-compute`** static library
that `zetta-core` links `PUBLIC` (so anything linking `zetta-core` — the GUI, the
tests — gets the backend symbols and the public `include/zetta/compute` headers).
This is the same isolation pattern SDL/ImGui get: a heavy/optional dependency
lives in its own target and **never leaks its headers into `zetta-core`**.

A **CPU-only configure is the default** and needs no CUDA toolkit — it builds
`zetta-compute` with just `device_buffer.cpp` + `host_stub_device.cpp` and is what
dev Macs and CI run:

```bash
cmake -S . -B build -G Ninja        # CUDA compute backend: disabled (CPU-only build)
cmake --build build
ctest --test-dir build              # all tests green, no GPU required
```

The **CUDA backend is opt-in** behind `-DZETTA_ENABLE_CUDA=ON`:

```bash
cmake -S . -B build -G Ninja -DZETTA_ENABLE_CUDA=ON   # requires the CUDA toolkit
```

When enabled, CMake `find_package(CUDAToolkit REQUIRED)` (so a missing toolkit
fails the configure loudly rather than silently dropping the backend),
`enable_language(CUDA)`, adds `cuda_device.cu` to `zetta-compute`, links
`CUDA::cudart` **`PRIVATE`** (a static-library private dependency links into the
final binary as link-only, so the CUDA *include directories* never reach
`zetta-core`), and sets the public `ZETTA_WITH_CUDA` compile definition.
Consumers — the device registry (#362), the tests — guard their use of
`CudaDevice` on `#if defined(ZETTA_WITH_CUDA)`, so a CPU-only build never names
the symbol. The `.cu` is compiled at `CUDA_STANDARD 20` (nvcc predates C++26; the
public compute headers only need C++20's `std::span`) using the same host
compiler as the rest of zetta, with `-fexperimental-library` propagated to the
host side so the libc++ string ABI matches across the link boundary (the same
concern the json-schema-validator handles).

## The CUDA backend: `CudaDevice`

`CudaDevice` (`include/zetta/compute/cuda_device.hpp`) is the GPU implementation
of the seam. Its **header is CUDA-free** — it forward-declares nothing CUDA and
includes no `<cuda_runtime.h>`, exactly the way `src/gui/app.hpp` keeps SDL out of
its header. All CUDA lives in `src/core/compute/cuda_device.cu`, the one
translation unit nvcc compiles:

- **Memory model** — `allocate`/`copy_from_host`/`copy_to_host`/`synchronize` over
  the CUDA runtime (`cudaMalloc`/`cudaMemcpy`/`cudaFree`/`cudaDeviceSynchronize`);
  buffers free themselves through `release_buffer` → `cudaFree`, so the RAII
  `DeviceBuffer` works unchanged over VRAM.
- **Operators** — real device work: `map`/`elementwise`/`axpy` via
  `thrust::transform` with hand-written functors (no device lambdas, so no
  `--extended-lambda`), `reduce`/`scan`/`sort`/`sort_by_key` via
  [Thrust](https://nvidia.github.io/cccl/thrust/) (ships with the toolkit), and
  `histogram`/`gemm`/`gather`/`scatter` as custom `__global__` kernels. Each call
  dispatches on the buffer's runtime `DataType` to the matching element type.
- **Conforms to the CPU reference** — `CpuDevice` (#360) is the contract, so the
  CUDA operators reproduce its preconditions *and* edge cases, not just the happy
  path: empty-buffer / dtype / exact-size guards; `sqrt`/`exp`/`log`, `gemm`, and
  `axpy` reject integer dtypes; integer `Divide` by zero returns `0` (rather than
  faulting the kernel); `gather`/`scatter` **bounds-check** every index and reject
  an out-of-range one with `InvalidArgument` (custom kernels — `thrust::gather`/
  `thrust::scatter` do no bounds check); and a **duplicate-index `scatter`** is
  resolved deterministically *last-writer-wins by input order* (a two-pass
  `atomicMax` kernel, matching the CPU reference's sequential scatter, where
  `thrust::scatter` would race).
- **No-throw at the seam** — a Thrust/CUDA failure is caught inside the `.cu` and
  returned as a `ComputeStatus::backend_error`, never an exception crossing into
  `zetta-core`. A `cudaErrorMemoryAllocation` maps to `out_of_memory`.

> **Verification note.** The CPU-only path (the acceptance gate — green build +
> all tests) is verified on macOS/CI. The `.cu` requires a CUDA toolkit + NVIDIA
> GPU to compile and run, which dev Macs don't have; validate the CUDA build on a
> Linux CUDA host before relying on the GPU kernels. The CPU↔CUDA conformance
> suite (`tests/test_cuda_device.cpp`, below) is the harness that proves the
> "matches the CPU reference" criterion when run there.

## Tests

`tests/test_compute_device.cpp` covers the `DataType` helpers, the typed
host⇄device round-trip for floats and ints (the **acceptance test**), byte-level
transfer validation (oversized copy, dtype/size mismatch on download), buffer
RAII + move semantics, `DeviceInfo`, and that every operator reports
`Unsupported` with a carried reason on the stub. A `#if defined(ZETTA_WITH_CUDA)`
block adds a `CudaDevice` round-trip + `map`/`reduce` test that compiles only in a
CUDA build and skips at runtime when no GPU is present (so a toolkit-but-no-GPU
host still passes).

`tests/test_cpu_device.cpp` asserts the exact per-operator results that are the
**numerical reference** — `CudaDevice` is checked against these.
`tests/test_cuda_device.cpp` is the **CPU↔CUDA conformance suite** (the acceptance
test of #361): for every operator it runs identical inputs through both
`CpuDevice` and `CudaDevice` and asserts they agree (bit-identical for integer
ops, within tolerance for floating-point), plus the edge-case parity that
distinguishes a correct backend from one that only computes the happy path —
integer-dtype rejection, integer-divide-by-zero safety, out-of-range
`gather`/`scatter` rejection, and deterministic duplicate-index `scatter`. Its
whole body is guarded on `ZETTA_WITH_CUDA` (an empty translation unit in a
CPU-only build) and every case skips at runtime when no GPU is present.

## What's next (later slices)

- **`CpuDevice`** (#360) ✅ — the reference backend implementing every operator on
  host memory via the [`zetta::runtime`](../architecture/runtime.md) thread pool; its results are
  the numerical reference CUDA is checked against. **Built** — see
  [cpu-device.md](cpu-device.md).
- **Device registry** (#362) — enumerate a node's devices, report each one's
  capability ([`DeviceInfo`](#the-interface-computedevice)), and select a backend
  per operation (guarding `CudaDevice` selection on `ZETTA_WITH_CUDA`); feeds the
  autoscaler/coordinator's capability model.
  **Built — see [device-registry.md](device-registry.md).**

The `zetta-compute` target + optional CUDA detection (#363) and the `CudaDevice`
kernels (#361) are **built** — see
[Build: `zetta-compute` + optional CUDA](#build-zetta-compute--optional-cuda).

See also [runtime.md](../architecture/runtime.md), [execution.md](../pipeline/execution.md), and
[architecture.md](../architecture/architecture.md).
