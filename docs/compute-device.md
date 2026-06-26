# Compute device interface (`zetta::compute`)

The **`ComputeDevice`** interface is the single seam every heavy compute operator
in zetta targets — the operator-level accelerator interface of blueprint
**ADR-27 / ARC-9**, the native distributed accelerated-compute pass for UC-1
churn ([#264](https://github.com/oramasearch/zetta/issues/264)). It is the same
shape as zetta's other seams ([`StepExecutor`](execution.md),
[`Check`](gate.md), [`StorageEngine`](storage.md)): a pure-virtual base class
whose concrete implementations are the **backends**. Consumers call the
interface and stay unchanged when new hardware appears — **new hardware = a new
backend, nothing else.**

This page documents the **interface, the device buffer / memory model, and the
host-memory stub** that ship in this slice ([#359](https://github.com/oramasearch/zetta/issues/359)).
No real backend lives here.

```
include/zetta/compute/data_type.hpp       DataType tag + byte_size / to_string / data_type_of<T>
include/zetta/compute/compute_status.hpp  ComputeStatus + ComputeStatusCode (the no-throw result)
include/zetta/compute/device_buffer.hpp   DeviceBuffer — typed, owning, move-only device memory
include/zetta/compute/compute_device.hpp  ComputeDevice — the seam: info + memory + operators
include/zetta/compute/host_stub_device.hpp HostStubDevice — host-memory transfer stub
src/core/compute/device_buffer.cpp        buffer lifetime + ComputeDevice::make_buffer
src/core/compute/host_stub_device.cpp     the stub implementation
```

## Where this sits in the "accel" family

This is the **first** of five issues; the rest build on it:

| Issue | What it adds |
| --- | --- |
| **#359 (this)** | the `ComputeDevice` interface + `DeviceBuffer` memory model + `HostStubDevice` |
| [#360](https://github.com/oramasearch/zetta/issues/360) | **`CpuDevice`** — the always-built reference backend, operators on host memory via the [`zetta::runtime`](runtime.md) thread pool |
| [#361](https://github.com/oramasearch/zetta/issues/361) | **`CudaDevice`** — the optional CUDA backend |
| [#362](https://github.com/oramasearch/zetta/issues/362) | a **device registry** — enumerate a node's devices, report each one's capability, select a backend per op |
| [#363](https://github.com/oramasearch/zetta/issues/363) | **CMake** — optional CUDA detection + a `zetta-compute` target housing the backends, linked without leaking CUDA headers |

`HostStubDevice` is **not** the CPU backend. It is a memory/transfer stub used to
ship and test the interface on its own; the real, operator-implementing CPU
device is `CpuDevice` (#360).

## Altitude: the interface, not the kernels

This slice declares the seam and proves the **memory model** round-trips; it does
**not** implement any operator. Every operator on `HostStubDevice` returns
`ComputeStatus::unsupported`. Real kernels plug in behind the seam **without
touching consumers** — exactly the shape of the gate's `Check` seam and the
storage engine's `StorageEngine`. No SDL/CUDA type appears in any header: the
interface is pure `zetta-core` and compiles on a CPU-only machine (dev Macs, CI).

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
[`ir::from_json`](execution-graph.md) returns its optional + reason at the IR
trust boundary.

> **No JSON boundary here.** Unlike the [Run](runs.md) and
> [audit](audit-trail.md) records, this slice introduces **no JSON format** — it
> is a pure in-process C++ interface that neither stores nor parses a document,
> so there is no schema to validate. When the registry (#362) one day serializes
> a device's `DeviceInfo` for the coordinator, that is where a JSON Schema will
> be added, at that boundary.

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

## Tests

`tests/test_compute_device.cpp` covers the `DataType` helpers, the typed
host⇄device round-trip for floats and ints (the **acceptance test**), byte-level
transfer validation (oversized copy, dtype/size mismatch on download), buffer
RAII + move semantics, `DeviceInfo`, and that every operator reports
`Unsupported` with a carried reason on the stub.

## What's next (later slices)

- **`CpuDevice`** (#360) — the reference backend implementing every operator on
  host memory via the [`zetta::runtime`](runtime.md) thread pool; its results are
  the numerical reference CUDA is checked against.
- **`CudaDevice`** (#361) — the GPU backend, behind optional CUDA detection.
- **Device registry** (#362) — enumerate a node's devices, report each one's
  capability ([`DeviceInfo`](#the-interface-computedevice)), and select a backend
  per operation; feeds the autoscaler/coordinator's capability model.
- **CMake `zetta-compute` target** (#363) — house the backends, link them into
  `zetta-core` without leaking CUDA headers (mirrors how SDL/ImGui are kept out).

See also [runtime.md](runtime.md), [execution.md](execution.md), and
[architecture.md](architecture.md).
