# Provisioner seam + node capability model

The **`Provisioner`** seam (`include/zetta/provisioner.hpp`) and the **node
capability model** (`include/zetta/node_capability.hpp`) are the first slice of
the **native distributed accelerated compute** pass for UC-1 churn (`#264`). They
realize **ARC-13** and entity **ENT-32**. This slice is the **abstraction only**
— the interface plus the value types — with a fake test double. The concrete
backends are separate, dependent slices:

- **Local** (given-hardware) provisioner — reports/uses the existing cluster
  nodes, best-effort (`#374`). **Built** — see
  [The Local backend](#the-local-backend-localprovisioner) below.
- **Cloud burst** — `AzureProvisioner`, the **reference** cloud backend (`#376`).
  **Built** — see [azure-provisioner.md](azure-provisioner.md). `GcpProvisioner`
  (`#378`) **mirrors** it via the [CLI pattern](cloud-provisioner-pattern.md) —
  see [gcp-provisioner.md](gcp-provisioner.md); `AwsProvisioner` (`#377`) follows.
- **Bulk burst identity + teardown** — reuse `Principal`/`AccessKey` (`#375`);
  **built** — see [burst-identity.md](burst-identity.md).
- **Large-burst cost-cap approval gate** — APR-3 (`#379`), built: see
  [burst-approval-gate.md](burst-approval-gate.md). A `Provisioner` decorator that
  blocks an over-cap burst pending approval and audits the decision.

The seam is what the **autoscaler / job coordinator** consumes to grow or shrink
the compute fleet for a job. **No cloud SDK lives behind this header** — that is
deliberately out of scope here.

## Node capability model (ENT-32)

`zetta::NodeCapability` describes a single node's compute resources along three
dimensions:

| Field | Type | Meaning |
|---|---|---|
| `cores` | `std::uint32_t` | logical CPU cores |
| `memory_bytes` | `std::uint64_t` | total memory |
| `gpu` | `Gpu` | the accelerator (`GpuKind::None` when CPU-only) |

A `Gpu` is `{ kind, throughput_tflops, model }` — a `GpuKind`
(`none` / `cuda` / `rocm` / `metal`; an **append-only** enum whose lowercase name
is the stored identifier), a coarse **throughput** figure in teraFLOPS the
autoscaler ranks placements by, and an optional free-form model name
(`"A100"`, `"H100"`, `"MI300X"`). A CPU-only node has `GpuKind::None` and zero
throughput.

> **Not** the access-control capability. `zetta::NodeCapability` is *compute*;
> `zetta::Capability` (see [access-control.md](access-control.md)) is a *right*.
> The names are kept apart on purpose. This is also distinct from — and feeds —
> the per-node device registry (`#362`), which enumerates a node's
> `ComputeDevice`s and reports each one's capability in this same shape.

### `satisfies` — the meet-or-exceed comparison

The same shape serves two roles: the **actual** capability a node reports and a
**requested** per-node floor a provision spec asks for. `satisfies()` links them:

```cpp
bool NodeCapability::satisfies(const NodeCapability& request) const;
```

`node.satisfies(request)` is true iff the node meets-or-exceeds the request on
**every** dimension:

- `cores >= request.cores`, and
- `memory_bytes >= request.memory_bytes`, and
- if the request needs a GPU (`request.gpu.present()`), the node has a GPU of the
  **same** `GpuKind` with at least the requested `throughput_tflops`.

A request with **no** GPU floor (`GpuKind::None`) ignores the GPU dimension, so a
GPU node still satisfies a CPU-only request. A different GPU family never
satisfies a request (a CUDA request is not met by a Metal node).

## The seam

```cpp
class Provisioner {
public:
    virtual ~Provisioner() = default;
    virtual ProvisionResult provision(const ProvisionRequest& request) = 0;
    virtual bool teardown(const std::string& node_id) = 0;
    virtual std::vector<ProvisionedNode> provisioned() const = 0;
};
```

- **`provision`** — bring up `request.count` nodes, each meeting `request.spec`.
  Backends are **best-effort**: they may return fewer than asked. A `count` of 0
  is a no-op that returns an empty, satisfied result.
- **`teardown`** — release one node by id. **Idempotent**: tearing down an
  unknown / already-gone id is a no-op that returns `false`, never an error.
- **`provisioned`** — the nodes currently up (provisioned and not yet torn down).

### Value types

| Type | Shape | Role |
|---|---|---|
| `ProvisionRequest` | `{ count, spec }` | how many nodes + the per-node `NodeCapability` floor |
| `ProvisionedNode` | `{ node_id, address, capability }` | a node that came up: its id, reachable `host:port`, and what it provides |
| `ProvisionResult` | `{ nodes, satisfied, detail }` | what `provision` returned: the nodes brought up this call, whether the full count was met, and a human-readable note (e.g. why it came up short) for the autoscaler / cost-cap gate |

## The Local backend (`LocalProvisioner`)

`zetta::LocalProvisioner` (`include/zetta/local_provisioner.hpp`) is the first
concrete `Provisioner` — the **given-hardware** path (ARC-13, `#374`). It does
**not** stand up VMs: it reports and uses the nodes **already in the cluster**,
best-effort. This is the case where we don't control the infrastructure — the
fleet is fixed, so it can select from it but cannot grow or shrink it.

### Where the fleet comes from — the injected source

It reads the live fleet through an injected **fleet source**, a callable that
returns the current cluster nodes as `ProvisionedNode`s:

```cpp
using FleetSource = std::function<std::vector<ProvisionedNode>()>;
explicit LocalProvisioner(FleetSource fleet);
```

It does **not** own a `Cluster` directly. That keeps it in `zetta-core` with no
network coupling and makes it unit-testable without a live cluster, and it is
re-read on **every** call, so the provisioner always reflects live, self-healing
membership (a node that leaves the cluster simply stops appearing — no teardown
needed). Production wiring binds the source to `Cluster::nodes()` paired with
each node's reported `NodeCapability` once capability reporting lands (a later
slice; `NodeStatus` carries no capability today). Until then a caller supplies
the inventory. An empty / unset source behaves as an empty cluster.

`LocalProvisioner` holds no mutable state of its own beyond the source, so it is
as thread-safe as the source it is given.

### Behaviour of the three operations

| Operation | Local behaviour |
|---|---|
| `provision(request)` | Selects up to `request.count` existing cluster nodes that `satisfies(request.spec)`, in fleet order — **best-effort**. `satisfied` is true iff it found the full count; when short it returns what it has with a `detail` note (it can't add hardware it doesn't control). `count` 0 → empty, satisfied. A spec with **no floor** (a default `NodeCapability`) is satisfied by every node, so the call returns the current cluster nodes up to the count — the acceptance criterion. |
| `teardown(node_id)` | A **no-op** that always returns `false`. Given hardware is not ours to release; nothing is ever torn down. (The idempotent seam contract — unknown id → `false` — taken to its limit for given hardware.) |
| `provisioned()` | The current cluster fleet — everything it "has up", which is the **whole cluster**, since it never creates or destroys nodes. |

This slice is the backend + a unit test only — it is not yet wired into the
(unbuilt) autoscaler / job coordinator, and it adds **no** new persisted or
replicated state and **no** new JSON format (it reuses the validated
`ProvisionedNode` boundary below).

## The cloud-burst backends (`AzureProvisioner` reference, `GcpProvisioner`, …)

Where `LocalProvisioner` *reports* given hardware, the **cloud-burst** backends
**own** the nodes they bring up: `provision` bursts them + mints bulk identities
so they join, `teardown` releases one + bulk-revokes when the pool drains, and
`provisioned` lists the live burst fleet. Every vendor follows **one shape** — a
`<Vendor>Provisioner` (the burst orchestration) delegating to a
`<Vendor>ControlPlane` seam, whose reference binding shells out to the vendor CLI
through the shared [`CommandRunner`](../include/zetta/command_runner.hpp) (no cloud
SDK in `zetta-core`). [`AzureProvisioner`](azure-provisioner.md) (`#376`) is the
**reference**; [`zetta::GcpProvisioner`](gcp-provisioner.md)
(`include/zetta/gcp_provisioner.hpp`, ARC-13 / ADR-20, `#378`) **mirrors** it with
a `GcpControlPlane` seam + a `GcpCliControlPlane` `gcloud` binding (the Managed
Instance Group + Spot shape) and a validated `GcpConfig`. The recipe every vendor
copies is [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md); full GCP
detail in [gcp-provisioner.md](gcp-provisioner.md).

## JSON boundary + schema

`NodeCapability` and `ProvisionedNode` each have a **validated JSON boundary**
(`to_json` / non-throwing `from_json`), following the same pattern as
[`DeploymentTarget`](deployments.md): `from_json` returns `std::nullopt` on
anything malformed — not an object, a missing/wrong-typed field, a negative
number, or an unknown GPU kind — so a node never silently over- or under-states
what it offers. The JSON shapes are documented by
[`schemas/node_capability.schema.json`](../schemas/node_capability.schema.json)
(the `NodeCapability` object, a `gpu` `$def`, and a `provisioned_node` `$def`),
which the `from_json` boundary checks **mirror** — keep the two in sync. The
parser accepts an integer whether nlohmann stored it signed or unsigned, so a
hand-written `{"cores": 4}` and a round-tripped value both parse.

The JSON form exists for forward use — reporting a node's capability into
replicated state, carrying a spec in a provision request, persisting a
`ProvisionedNode`. This slice does not yet persist or replicate any of it; it is
in-memory only.

## Tests

`tests/test_provisioner.cpp` covers the acceptance criterion and the value types:

- **`FakeProvisioner` round-trip** — a capacity-bounded in-memory test double
  provisions N nodes meeting a spec, lists them, and tears them down; each
  provisioned node `satisfies` the spec, and teardown is idempotent (unknown id →
  `false`, list unchanged).
- **best-effort** — a request beyond capacity returns fewer nodes with
  `satisfied == false` and a non-empty `detail`; a zero-count request is a
  satisfied no-op.
- **`satisfies`** — every dimension (cores, memory, GPU kind + throughput),
  including a CPU-only node failing a GPU request and a cross-family mismatch.
- **JSON boundary** — `NodeCapability` and `ProvisionedNode` round-trip through
  `to_json`/`from_json` (CPU-only and GPU nodes), and `from_json` rejects
  malformed documents (non-object, missing/negative fields, unknown GPU kind).

`tests/test_local_provisioner.cpp` covers the `LocalProvisioner` backend against
a fixed sample fleet (two CUDA boxes + one CPU box) read through an injected
source: the acceptance criterion (`provision` returns the current cluster nodes;
`teardown` is a no-op; `provisioned` lists the whole cluster), best-effort
selection over the spec floor (only matching nodes, fewer than asked when the
cluster is too small), the requested-count cap, the zero-count no-op, live
membership changes (a node leaving disappears without a teardown call), and an
empty source behaving as an empty cluster.

See also [deployments.md](deployments.md) (the `DeploymentTarget` placement dial,
the sibling abstraction for *where* a graph runs) and ARC-13 / ENT-32 (`#373`).
