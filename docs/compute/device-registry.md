# Device registry (`zetta::compute::DeviceRegistry`)

The **device registry** is the piece that turns the
[`ComputeDevice`](compute-device.md) *interface* into a usable set of *backends*
on a node — the per-node enumeration + selection layer of blueprint **ARC-9**, the
native distributed accelerated-compute pass for UC-1 churn
([#264](https://github.com/oramasearch/zetta/issues/264),
[#362](https://github.com/oramasearch/zetta/issues/362)). It does exactly three
things, the issue's scope:

1. **Enumerate** the compute devices available on a node.
2. **Report** each device's capability (kind / compute units / memory), serialized
   as the coordinator-facing capability report — the JSON boundary the
   ComputeDevice slice anticipated for this issue.
3. **Select** a backend per unit of work — the accelerator (GPU) when one is
   present, falling back to the CPU, with hard filters for a required kind and a
   device-memory floor.

```
include/zetta/compute/device_registry.hpp  DeviceRegistry + DeviceRequest + is_accelerator + the DeviceInfo JSON boundary
src/core/compute/device_registry.cpp       enumeration + selection + boundary validation
schemas/device_registry.schema.json         the capability-report JSON Schema (mirror)
tests/test_device_registry.cpp              enumeration, the selection unit test, capability report, JSON boundary
```

## Where this sits in the "accel" family

This is the **fourth** of the five accel-compute issues, building on the
[`ComputeDevice`](compute-device.md) seam (#359):

| Issue | What it adds |
| --- | --- |
| [#359](https://github.com/oramasearch/zetta/issues/359) | the `ComputeDevice` interface + `DeviceBuffer` memory model + `HostStubDevice` |
| [#360](https://github.com/oramasearch/zetta/issues/360) | **`CpuDevice`** — the always-built reference backend (operators on host memory) |
| [#361](https://github.com/oramasearch/zetta/issues/361) | **`CudaDevice`** — the optional CUDA backend |
| **#362 (this)** | a **device registry** — enumerate a node's devices, report each one's capability, select a backend per op |
| [#363](https://github.com/oramasearch/zetta/issues/363) | **CMake** — optional CUDA detection + a `zetta-compute` target housing the backends |

## Altitude: the container/selector, not the backend factory

The registry is a **container + selector** that backends are **registered into** —
it does **not** manufacture `CpuDevice` / `CudaDevice` itself. There are two
reasons, both deliberate:

- The real backends are separate slices (#360 / #361) that **do not exist yet**.
- `CudaDevice` pulls in CUDA, which must **never** leak into `zetta-core` (the same
  rule that keeps SDL/ImGui out — see [architecture.md](../architecture/architecture.md)). The
  registry header is pure `zetta-core` and compiles CPU-only.

So the **"CPU always, CUDA if built + present"** enumeration policy is the job of
the `zetta-compute` assembly point (#363): it builds a `CpuDevice` and, when CUDA
is compiled in and a GPU is detected, a `CudaDevice`, and `add()`s each into the
registry. The registry is the seam those backends plug into — exactly the same
discipline as the [`Provisioner`](../cloud/provisioner.md) (which shipped the seam, not the
cloud backends). Until #363, the registry is exercised with the
[`HostStubDevice`](compute-device.md) (which reports `DeviceKind::Cpu`) and a test
double standing in for a CUDA device.

This slice adds **no `Node` wiring and no SQL table** — it is a standalone library
seam, the same altitude `ComputeDevice` and `Provisioner` shipped at. The
coordinator that owns a registry per node, and any `system.zetta_devices` view,
are later slices.

## Enumeration

```cpp
#include "zetta/compute/device_registry.hpp"

zetta::compute::DeviceRegistry registry;
registry.add(make_cpu_device());               // CpuDevice (#360), once it exists
if (cuda_present()) registry.add(make_cuda_device());  // CudaDevice (#361), behind #363
```

- `add(device)` registers a device and returns its index (registration order); a
  null device is ignored, so lookups never dereference null.
- `devices()` / `size()` / `empty()` / `at(index)` enumerate them.
- `find(kind)` returns the first device of a kind (or `nullptr`); `has(kind)` is the
  boolean — **`has(DeviceKind::Cpu)` is true on every machine, `has(DeviceKind::Cuda)`
  only where a CUDA backend was registered** (the acceptance criterion).

Lookups hand back a `std::shared_ptr<ComputeDevice>` (empty on miss), the house
style — never a raw pointer.

## Capability reporting

Each device answers `info()` with a [`DeviceInfo`](compute-device.md#the-interface-computedevice)
— its `kind`, `compute_units` (CPU cores / GPU SMs), `memory_bytes`, and
`unified_memory`. The registry exposes them two ways:

- `device_infos()` — the live `std::vector<DeviceInfo>`, in registration order.
- `to_json()` — the **coordinator-facing capability report**, `{ "devices":
  [ <DeviceInfo>, … ] }`, which **feeds the autoscaler / coordinator's capability
  model**. This is the per-node hardware fact base the
  [`NodeCapability`](../cloud/provisioner.md) model is built from.

## Selection

`select(DeviceRequest)` ranks the registered devices and returns the best fit (or
`nullptr` when none qualifies):

```cpp
struct DeviceRequest {
    bool prefer_accelerator = true;            // route heavy ops to a GPU when present
    std::optional<DeviceKind> require_kind{};  // hard kind requirement (a CUDA-only op)
    std::size_t min_memory_bytes = 0;          // device-memory floor (0 = none)
};
```

The ranking:

1. **Filter** out devices that fail `require_kind` or `min_memory_bytes`. A device
   that reports `0` memory (host / unknown, like the CPU's host RAM) does **not**
   satisfy a positive floor — selection only picks a device that can *prove* the
   capacity.
2. **Rank** the survivors: the preferred class first (an **accelerator** when
   `prefer_accelerator`, else the **CPU**), breaking ties by more compute units,
   then more memory, then earliest registration (stable).

```cpp
registry.select();                                            // best heavy-op device: GPU if present, else CPU
registry.select({.prefer_accelerator = false});              // prefer the CPU (e.g. a tiny op)
registry.select({.require_kind = zetta::compute::DeviceKind::Cuda});  // a CUDA-only kernel
registry.select({.min_memory_bytes = 8ull << 30});           // needs ≥ 8 GiB of device memory
```

`is_accelerator(kind)` is the helper the ranking uses — every kind except
`DeviceKind::Cpu` is an accelerator (CUDA today; ROCm / Metal later).

A **per-op fallback** when the chosen device returns
[`ComputeStatus::unsupported`](compute-device.md#error-model-computestatus) (the
status *is* the per-op capability) is the coordinator's job — route to the
next-best device. The registry supplies the *ranking*; it does not probe operators
(that would need buffers).

## JSON boundary

This slice introduces the JSON format the
[ComputeDevice page](compute-device.md#error-model-computestatus) foresaw: *"When
the registry (#362) one day serializes a device's `DeviceInfo` for the
coordinator, that is where a JSON Schema will be added, at that boundary."*

- `device_info_to_json(info)` / `device_info_from_json(value)` — a `DeviceInfo`'s
  JSON value (`{ kind, name, compute_units, memory_bytes, unified_memory }`).
- `DeviceRegistry::to_json()` / `parse_device_report(value)` — the whole report,
  `{ "devices": [ … ] }`.

`device_info_from_json` / `parse_device_report` **validate the shape at the
boundary** (non-throwing, returning `std::optional`) so a malformed report fails
fast instead of silently mis-stating a node's hardware — a not-an-object document,
an unknown `kind`, a wrong-typed or negative field, all reject. The validation is
hand-written, mirroring [`schemas/device_registry.schema.json`](../../schemas/device_registry.schema.json)
— the same pattern as its sibling [`NodeCapability`](../cloud/provisioner.md) (keep the code
and the schema in sync). The `DeviceInfo` JSON form lives in the **registry**
module, not `compute_device.hpp`, so the pure `ComputeDevice` seam header stays
JSON-free.

## Tests

`tests/test_device_registry.cpp` covers enumeration (registration order, `find` /
`has` / `at`, null-ignored), the **acceptance criterion** (CPU reported on any
machine, CUDA only where present), the **selection unit test** (accelerator-first
default, CPU preference, `require_kind` / `min_memory_bytes` filters, CPU-only
fallback, the empty result, tie-break by capacity then registration order), the
capability report round-trip, and the JSON boundary (round-trip + malformed
rejection). It uses a `FakeDevice` test double that reports a configurable
`DeviceInfo` and supports no operators — all the registry ever calls is `info()`.

## What's next (later slices)

- **`CpuDevice`** (#360) / **`CudaDevice`** (#361) — the real backends the registry
  enumerates.
- **CMake `zetta-compute`** (#363) — the assembly point that detects CUDA and
  `add()`s the concrete backends into a node's registry ("CPU always, CUDA if
  built + present").
- **The job coordinator / autoscaler** — owns a registry per node, consumes the
  capability report, and drives the per-op selection + fallback across the cluster.

See also [compute-device.md](compute-device.md), [provisioner.md](../cloud/provisioner.md),
and [architecture.md](../architecture/architecture.md).
