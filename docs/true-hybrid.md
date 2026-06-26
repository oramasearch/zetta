# True hybrid — one cluster, every vendor and architecture

**True hybrid** is the property that a *single* Orama Zetta cluster spans
heterogeneous hardware **at once** — different CPUs, GPUs, and accelerators,
different operating systems and architectures, on-prem and multiple clouds — and
gets the most out of whatever it is given. It is not "you can deploy to more than
one place" (pick one target per deployment); it is **one equal-peer cluster whose
nodes are deliberately *not* uniform**, with work flowing to whatever each node
runs best.

This page describes the architecture that makes that work and is **explicit about
what is built today versus declared/planned** — the rest of `docs/` is a
description of shipped code, and this page holds to the same bar. Each capability
links to its own page.

Heterogeneous fleets are the normal case, not the exception — a few GPU boxes next
to a rack of CPU servers, an on-prem cluster that bursts into a cloud, an
Apple-silicon laptop joined to x86 Linux peers. A system that demands a uniform
fleet either leaves that hardware idle or refuses to use it.

---

## Why it matters: cost, flexibility, resilience

True hybrid is what gives an operator the **ultimate control over cost,
flexibility, and resilience** — because the cluster is never bound to one vendor's
hardware, prices, or availability.

- **Cost.** Run each workload on the cheapest hardware that fits, and spend the
  capacity you already own or have idle before renting more. The
  [work-stealing scheduler](#squeeze-the-most-out-of-what-you-have-the-work-stealing-scheduler)
  keeps every machine busy at its own rate, and the
  [`Provisioner`](#grow-the-fleet-the-provisioner-seam) seam bursts into a cloud
  **only when needed** (under a cost cap). No premium for standardizing on a single
  vendor's stack. This is how true hybrid **squeezes the most out of what you
  already have.**
- **Flexibility.** Mix any vendor, architecture, and cloud in one cluster, and
  adopt new silicon as **a backend, not a re-architecture** — the
  [`ComputeDevice`](#the-seam-that-makes-it-work-computedevice) seam means a new
  GPU or ASIC is additive. Place each workload where it belongs instead of forcing
  the whole system onto one platform's terms.
- **Resilience.** The cluster is **not susceptible to a single vendor's outages or
  degradations.** Because nodes are
  [equal peers that self-heal](#equal-peers-no-leader-the-cluster--crdt) and the
  scheduler **leases and re-queues** stolen work (a dead or slow helper never hangs
  a job), an outage or slowdown in one cloud, region, or GPU vendor does not take
  the cluster down — healthy nodes pick up the pending work and the CRDT converges
  on whatever is still up. Spread a job across two clouds and a rack of your own
  metal, and any one of them can fail without stopping it.

---

## The seam that makes it work: `ComputeDevice`

Every heavy compute operator targets one pure-virtual seam,
`zetta::compute::ComputeDevice` ([compute-device.md](compute-device.md), ADR-27 /
ARC-9). Concrete implementations are the **backends**; consumers call the
interface and stay unchanged when new hardware appears — **new hardware is a new
backend, nothing else.** An operator a backend can't do returns
`ComputeStatus::unsupported` (the status *is* the per-op capability — there is no
parallel bitmask to drift), so the registry can route that work to a device that
can.

This is exactly why "true hybrid" is a property of the **design**, not a feature
bolted on: adding a vendor's silicon is adding a backend behind a seam the rest of
the system already speaks.

### Compute backends

| Backend | Hardware / vendor | Status |
| --- | --- | --- |
| **`CpuDevice`** | CPU — x86-64 and ARM (every operator, multi-threaded via `runtime::parallel_for`) | **built** — the always-on reference backend ([cpu-device.md](cpu-device.md), #360) |
| **`CudaDevice`** | NVIDIA GPU (CUDA) | **built**, opt-in behind `-DZETTA_ENABLE_CUDA=ON` ([compute-device.md](compute-device.md), #361/#363) |
| ROCm backend | AMD GPU | **planned** — `GpuKind::rocm` is in the capability model; the backend is a future seam impl |
| Metal / MLX backend | Apple GPU (Apple silicon) | **planned** — `GpuKind::metal` is in the capability model; the backend is future |
| Vulkan backend | Cross-vendor GPU compute | **planned** backend behind the same seam |
| SYCL backend | Cross-vendor (oneAPI-class: CPU/GPU/accelerator) | **planned** backend |
| GCP TPU backend | Google Cloud TPU (ASIC) | **planned** backend |
| AWS Neuron backend | AWS Neuron ASICs — Trainium (training) + Inferentia (inference) | **planned** backend |

The CPU reference is what the GPU/accelerator backends are checked **against**
(correctness over throughput). The default build is CPU-only and needs no GPU
toolkit; accelerator backends are opt-in so a dev Mac / CI compiles clean.

---

## Selecting per node: `DeviceRegistry` + `NodeCapability`

A node does not assume its hardware. The per-node **`DeviceRegistry`**
([device-registry.md](device-registry.md), #362) enumerates the node's devices,
**reports** each one's `DeviceInfo` capability, and **selects** a backend per unit
of work — accelerator-first, falling back to the CPU, with `require_kind` /
`min_memory_bytes` filters. So the *same* graph step runs on a GPU where one
exists and on the CPU where it doesn't, with no change to the caller.

The fleet-level capability model is **`zetta::NodeCapability`**
([provisioner.md](provisioner.md), ENT-32): `cores`, `memory_bytes`, and a `Gpu`
(`GpuKind` — `none` / `cuda` / `rocm` / `metal` — plus `throughput_tflops` and an
optional `model`). `satisfies()` is the meet-or-exceed comparison the scheduler and
provisioner use to match work to hardware. New accelerator classes extend this
model; they do not replace it.

---

## Squeeze the most out of what you have: the work-stealing scheduler

The heart of true hybrid is that the cluster **does not need a fixed per-worker
capability model to balance heterogeneous hardware**. The **work-stealing
scheduler** ([work-stealing-scheduler.md](work-stealing-scheduler.md), ARC-10,
#366) splits a job into many small tasks; idle or faster workers **steal** pending
tasks from busy ones. A faster worker — or a GPU node — naturally processes more,
a slower one fewer. The split is *fluid and heterogeneity-tolerant* by
construction: no node is assigned a fixed quota that a slower box would bottleneck
or a faster box would starve on.

- The **local** half (`WorkStealingScheduler`) steals between worker threads on a
  node (per-worker deques; owner pops the back for locality, a thief takes the
  front).
- The **cross-node** half (`DistributedScheduler`) extends the same idea over the
  [signal plane](signal-plane.md): idle/faster *nodes* steal pending tasks from
  the owner of a job, run them on their own pool, and return results — single-hop,
  leased + re-queued so a dead helper never hangs the job, idempotent on late
  results.

This is the mechanism behind "squeeze the most out of what you have": every
machine contributes at its own rate, automatically.

---

## Combine partials across the cluster: collectives

Heterogeneous nodes still have to agree on a combined result. **`Collectives`**
([collectives.md](collectives.md), ARC-10, #365) provides allreduce / broadcast /
barrier / gather over the signal plane to fold per-node compute partials (gradient
histograms, partial aggregates) into one. It is **deterministic regardless of node
order** — the root is the lexicographically-smallest node id (leaderless), and it
reduces once in node-id order and pushes the single result to everyone, so every
node ends bit-identical even though floating-point addition isn't associative. A
hybrid fleet does not change the answer.

---

## Equal peers, no leader: the cluster + CRDT

Nodes form a cluster of **equal peers** over TCP with **no configured leader**
([cluster.md](cluster.md)). A laptop, a server, and a cloud instance are the *same
kind of node*; adding capacity is joining a node. Links self-heal (gossip,
idle-timeout, re-dial), so a hybrid fleet that loses and regains members keeps
running. All distributed state — schemas, rows, membership, metadata — is a single
Automerge **CRDT** ([replication.md](replication.md)); every node is a writer and
merges converge conflict-free, which is what lets the peers stay equal across a
heterogeneous, partitionable network.

---

## Grow the fleet: the `Provisioner` seam

True hybrid also means the fleet can **grow across targets**. The **`Provisioner`**
seam ([provisioner.md](provisioner.md), ARC-13) brings up nodes meeting a
`NodeCapability` spec, tears them down, and lists what is up — with cost-cap
approval ([burst-approval-gate.md](burst-approval-gate.md)) and bulk burst
identities ([burst-identity.md](burst-identity.md)) layered on.

| Target | Status |
| --- | --- |
| **Bare metal / given hardware** | **built** — `LocalProvisioner` uses the nodes already in the cluster, best-effort (#374) |
| AWS / GCP / Azure cloud-burst | **planned / in progress** — concrete `Provisioner` backends (#377 / #378 / #376) behind the same seam |
| Other clouds (e.g. Oracle Cloud) | **planned** — a new `Provisioner` backend, nothing else changes |

Placement of a validated graph onto a target rides the **`DeploymentTarget`** dial
`local → kubernetes → vpc → cloud` ([deployments.md](deployments.md)); **R1 ships
`Local`**, the rest are declared and land as their provisioners do.

---

## Support matrix (today)

| Dimension | Built today | Declared / planned |
| --- | --- | --- |
| **Compute** | CPU (`CpuDevice`); NVIDIA CUDA (`CudaDevice`, opt-in) | AMD ROCm, Apple Metal/MLX, Vulkan, SYCL, GCP TPU, AWS Neuron (Trainium/Inferentia) — each a backend behind `ComputeDevice` |
| **CPU architecture** | x86-64, ARM | — |
| **Operating system** | macOS (macOS-first), Linux | Windows |
| **Cluster** | equal-peer, self-healing, CRDT-replicated | leadership-by-lease for exclusive structures |
| **Scheduling** | local + cross-node work-stealing; collectives | k-ary/ring collective topologies |
| **Fleet / cloud** | bare-metal / given-hardware (`LocalProvisioner`) | AWS / GCP / Azure cloud-burst; k8s / vpc / cloud deployment targets |

The honest summary: the **seams that make true hybrid possible are built** — the
compute-device abstraction, the per-node registry + capability model, the
heterogeneity-tolerant work-stealing scheduler, deterministic collectives, the
equal-peer CRDT cluster, and the provisioner seam with a working given-hardware
backend. The **breadth of concrete backends and cloud-burst providers is landing
behind those seams**, each as "a new backend, nothing else."

## See also

- [compute-device.md](compute-device.md) — the `ComputeDevice` seam + backends.
- [cpu-device.md](cpu-device.md) — the CPU reference backend.
- [device-registry.md](device-registry.md) — per-node enumerate / report / select.
- [work-stealing-scheduler.md](work-stealing-scheduler.md) — heterogeneity-tolerant scheduling.
- [collectives.md](collectives.md) — combining partials deterministically.
- [cluster.md](cluster.md) / [replication.md](replication.md) — equal peers + CRDT.
- [provisioner.md](provisioner.md) — the fleet/capability model + the `Provisioner` seam.
- [deployments.md](deployments.md) — the placement dial (`local → kubernetes → vpc → cloud`).
