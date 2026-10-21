# Bare-metal provisioner (local host)

The **`BareMetalProvisioner`** (`include/zetta/cloud/bare_metal_provisioner.hpp`) is the
bare-metal (local-host) cloud-burst backend behind the
[`Provisioner`](provisioner.md) seam — a **mirror** of the reference
[`AzureProvisioner`](azure-provisioner.md) (`#376`) and the
[`GcpProvisioner`](gcp-provisioner.md) (`#378`) /
[`OciProvisioner`](oci-provisioner.md) (`#432`) /
[`K8sProvisioner`](k8s-provisioner.md) (`#434`) backends, following the
[cloud-provisioner CLI pattern](cloud-provisioner-pattern.md). It realizes
**ARC-13 / ADR-20** in the native distributed accelerated-compute pass for UC-1
churn (`#264`, `#436`).

The lifecycle is identical on every backend: **burst** N ephemeral nodes → **mint**
bulk identities so they can join the cluster → run cluster-parallel → **tear down**
+ **revoke** those identities. `BareMetalProvisioner` is structurally identical to
the cloud/Kubernetes backends — only the config type and the CLI binding differ.

> **Bare metal is the local host — not a remote cloud or cluster.** Each worker runs
> as a **local container** via the host's container runtime (`docker` / `podman`).
> Why containers? The `Provisioner` seam runs commands through a **synchronous**
> [`CommandRunner`](../../include/zetta/cloud/command_runner.hpp) (fork/execvp, runs to
> completion, no shell) — and `docker run -d` *starts a detached worker and returns
> immediately*, which is exactly what makes a long-lived worker launchable through
> that seam, cross-OS (macOS / Linux / Windows). The bare-metal guide frames this as
> "containers on bare metal still run on the local machine" (§7). A future variant
> could launch raw processes via systemd / launchd, but those are OS-specific; the
> container runtime is the universal, synchronous-launch mechanism.

### Distinct from `LocalProvisioner`

[`LocalProvisioner`](provisioner.md#the-local-backend-localprovisioner) (`#374`)
*reports* the nodes already in the cluster and **never creates any** (its teardown is
a no-op). `BareMetalProvisioner` actually **brings up** new local worker nodes (and
tears them down). The two are complementary: given-hardware reporting vs. local
bursting.

## How it fits

```
                       Provisioner (vendor-neutral seam)
                                   ▲
                                   │ implements
                       BareMetalProvisioner ───────────► IdentityHooks (optional)
                       burst lifecycle + identity         mint on provision /
                                   │ delegates host work   revoke on teardown
                                   ▼                       (→ BurstIdentityManager)
                       BareMetalControlPlane (seam)
                                   ▲
                                   │ implements
                       BareMetalCliControlPlane ────────► CommandRunner
                       shells out to docker / podman       exec argv, capture JSON
```

`BareMetalProvisioner` owns the **burst orchestration** (provision → mint → teardown
→ revoke) but **none of the host mechanics**. It delegates those to an injected
`BareMetalControlPlane`, so it stays in `zetta-core` with **no container SDK** and is
fully unit-testable against a fake. The reference `BareMetalControlPlane` is
`BareMetalCliControlPlane`, which shells out to the runtime CLI — a libcontainer /
API binding could replace it without touching the provisioner.

## The burst lifecycle

| Operation | Behaviour |
|---|---|
| `provision(request)` | Calls `control_plane.bring_up(config, request)` to launch the **shortfall** of worker containers up to `request.count` (best-effort — image-pull / resource failures may yield fewer). When any worker comes up and a mint hook is wired, **mints bulk identities** for the returned workers so they can authenticate and join. `count` 0 is a satisfied no-op that touches neither the control plane nor the hooks. |
| `teardown(node_id)` | Calls `control_plane.destroy(config, node_id)` — force-removes the container by id/name (idempotent — unknown id → `false`). When a successful teardown drains the pool to **empty**, **bulk-revokes** the job's identities (the revoke hook). |
| `provisioned()` | Delegates to `control_plane.list(config)`, so it reflects **live state** — a worker that exits disappears without a teardown call, exactly as `LocalProvisioner` reflects self-healing membership. |

The control plane is the **source of truth** for what is up: `provisioned()` and the
"did this teardown drain the pool?" check both read it, so the provisioner keeps no
parallel copy of fleet state (the [everything-is-derived rule](../cluster/replication.md)).

### Bulk identity — minted on provision, revoked on full teardown

The issue's "join with bulk identity" is wired through two optional
**`IdentityHooks`** callables, identical to the cloud/Kubernetes references:

```cpp
struct IdentityHooks {
    std::function<void(const std::vector<ProvisionedNode>&)> mint;   // on provision
    std::function<void()> revoke;                                    // on full teardown
};
```

Production binds them to a `BurstIdentityManager` for the job — even local workers get
a short-lived, job-scoped zetta credential to join the cluster; `tests/
test_bare_metal_provisioner.cpp` exercises the wiring against a real
`BurstIdentityManager`.

## Configuration (`BareMetalConfig`)

`BareMetalConfig` is how to burst workers on the local host. Only the worker image is
required; everything else has a sensible default.

| Field | Type | Required | Meaning |
|---|---|:--:|---|
| `image` | `string` | ✓ | the container image each worker runs |
| `runtime` | `string` | | the host container runtime CLI — `docker` or `podman` (default `docker`) |
| `pool_label` | `string` | | the label applied to every burst worker; the stable pool key for list/teardown (default `zetta-burst`) |
| `name_prefix` | `string` | | the container name prefix; a UUID is appended (default `zetta-burst`) |
| `network` | `string` | | the container network (e.g. `host` so a worker can reach the cluster); empty → the runtime default |
| `gpus` | `bool` | | give workers the host GPUs (`--gpus all`) (default `false`) |
| `seed_peer` | `string` | | cluster seed `host:port` injected as the `ZETTA_SEED_PEER` env var so each worker auto-joins |

> There is no `region` / `account` — the "account" is the local machine. The
> `pool_label` plays the role the scale-set / pool *name* plays on the cloud
> backends: it is how the binding finds the burst workers to list and tear down.

### JSON boundary + schema

`BareMetalConfig` has a **validated JSON boundary** (`to_json` / non-throwing
`from_json`), the same pattern as the cloud configs: `from_json` returns
`std::nullopt` on anything malformed — not an object, a missing/empty required
`image`, or a wrong-typed field — so a misconfigured burst fails fast instead of
bringing up the wrong (or no) fleet. The shape is documented by
[`schemas/bare_metal_config.schema.json`](../../schemas/bare_metal_config.schema.json),
which the `from_json` checks **mirror** — keep the two in sync. Optional string fields
are emitted by `to_json` only when set; `gpus` is always emitted.

## The reference runtime binding (`BareMetalCliControlPlane`)

`BareMetalCliControlPlane` (`include/zetta/cloud/bare_metal_cli_control_plane.hpp`) is the
reference `BareMetalControlPlane`. It borrows the **local-container shape**: a stable
**pool label** marks the burst workers, launching `count` labelled containers is the
burst, force-removing one is teardown. It builds runtime argv, runs them through an
injected **`CommandRunner`**, and parses the JSON the CLI returns.

| `BareMetalControlPlane` call | runtime command (shape) |
|---|---|
| `bring_up` | list the pool (`<runtime> ps --filter label=<pool> --format '{{json .}}'`), then `<runtime> run -d --label <pool> --name <prefix>-<uuid> [--network … --gpus all --env ZETTA_SEED_PEER=… --env-file <private file>] <image>` for the **shortfall** only, then list again (the env-file holds `ZETTA_CLUSTER_TOKEN=…` and is added only on a secure cluster — #670; the token is never an argv value — #770) |
| `destroy` | `<runtime> rm -f <id>` |
| `list` | `<runtime> ps --filter label=<pool> --format '{{json .}}'` |

`bring_up` is **launch-the-shortfall**: it counts what is already up under the pool
label and launches only `count − current` new workers, so a re-burst to the same
count launches nothing (idempotent on the label). `docker run -d` returns
immediately, so each launch is a synchronous command that completes. The per-worker
shape — the pool label, a unique name, the network, `--gpus all`, the
`ZETTA_SEED_PEER` env, and (on a secure cluster) the `ZETTA_CLUSTER_TOKEN` env
(#670) — lands on the `run` argv, except the token, which comes from a mode-0600
`--env-file` removed after the burst so it never shows in the host's process list
(#770; the runtime still records it in `docker inspect` — see
[cluster security](../cluster/security.md#provisioned--burst-nodes-670)). `destroy` force-removes a worker
(`rm -f`, stop + remove). The command shapes are grounded in
[research/baremetal-compute.md](../research/baremetal-compute.md) (the cited guide — §7
containers on bare metal, §8 `--gpus all`).

### The command runner — testable without a runtime

```cpp
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
```

`CommandRunner` is **shared, vendor-neutral** infrastructure
(`include/zetta/cloud/command_runner.hpp`) — the same exec seam the cloud / Kubernetes
bindings use (see [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md)). The
runner takes **argv** (program + arguments, no shell — so no shell-injection risk)
and returns `{ exit_code, stdout_text, stderr_text }`. The default
(`default_command_runner()`) `fork`/`execvp`s the runtime and captures its merged
output; a test injects a fake that asserts on the argv and returns canned output.
That makes the **command construction + JSON parsing** — the parts most likely to
drift — fully unit-tested. The actual runtime exec is the only path CI cannot run; a
production deployment installs Docker Engine / Docker Desktop / Podman.

The `list` output is parsed at the **`ProvisionedNode` boundary**
(`ProvisionedNode::from_json`). `docker ps --format '{{json .}}'` emits
**newline-delimited JSON** (one object per line), so the parser reads it line-by-line
(and also accepts a JSON array / `{ "data" | "items": [ … ] }` envelope for
robustness across runtimes), skipping malformed entries — a production binding
projects `ps` output into the `{ node_id, address, capability }` shape (container
id/name + IP), and the reference binding consumes that shape directly.

## What this slice does **not** do

- **No live-runtime CI.** The acceptance criterion ("provision N workers that join
  the cluster and are torn down on the local host") is realized as the full *shape* —
  the seam, config + schema, lifecycle, identity wiring, and the runtime binding —
  verified against fakes. Running it against a real Docker/Podman install is a manual
  / deployment step.
- **No persistence / replication.** `BareMetalConfig` and the in-flight burst are
  in-memory; the config's JSON form exists for forward use.
- **Not yet driven by a live autoscaler.** The
  [autoscaler control loop](../compute/job-autoscaler.md) (#371) and
  [job coordinator](../compute/job-coordinator.md) are built and act through any
  `Provisioner&` (optionally wrapped by the
  [cost-cap approval gate](burst-approval-gate.md)); wiring them to drive *this*
  backend in a live burst is the remaining slice. `BareMetalProvisioner` drops in
  wherever one holds a `Provisioner&`.
- **No raw-process variant.** Workers run as local containers; a systemd/launchd raw-
  process backend is a possible future sibling behind the same control-plane seam.

## Tests

`tests/cloud/test_bare_metal_provisioner.cpp` mirrors the sibling tests — the config
boundary, the lifecycle, and the reference binding:

- **`BareMetalConfig` JSON boundary** — round-trips a minimal and a full config; the
  defaults (`runtime`, `pool_label`, `name_prefix`, `gpus`); and rejects malformed
  documents (non-object, missing/empty `image`, wrong-typed `image` / `runtime` /
  `gpus`).
- **lifecycle (fake control plane)** — provision brings up workers **and mints
  identities** for exactly what came up; best-effort when short; zero-count is a
  no-op; **no mint** when nothing comes up; teardown **bulk-revokes when the pool
  drains** (once, on the last worker) and an unknown id is an idempotent no-op;
  `provisioned()` reflects a worker exiting; and it works with no identity hooks
  wired. A dedicated test composes it with a **real `BurstIdentityManager`** (burst →
  mint per worker → teardown → bulk revoke).
- **`BareMetalCliControlPlane` (stateful fake runtime)** — `bring_up` launches the
  **shortfall** (each `run -d` carries the label / unique name / image / seed env),
  then lists; a re-burst to the same count **launches nothing** (idempotent on the
  label); `--gpus all` and `--network` flags are passed when set; a `run` failure is
  surfaced best-effort in the `detail`; the configured `runtime` (`podman`) is used
  for every call; `destroy` builds `rm -f <id>` and maps a non-zero exit to `false`;
  `list` parses **NDJSON** and skips malformed lines, returning empty on a command
  failure.

## See also

- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the recipe this
  backend follows (the four pieces + shared infra + the lifecycle→CLI mapping).
- [azure-provisioner.md](azure-provisioner.md) — the **reference** backend.
- [gcp-provisioner.md](gcp-provisioner.md) / [oci-provisioner.md](oci-provisioner.md)
  / [k8s-provisioner.md](k8s-provisioner.md) — the sibling backends.
- [provisioner.md](provisioner.md) — the seam + the given-hardware `LocalProvisioner`
  (which *reports* rather than *bursts* local nodes).
- [burst-identity.md](burst-identity.md) — the `BurstIdentityManager` the mint/revoke
  hooks bind to.
- [research/baremetal-compute.md](../research/baremetal-compute.md) — the cited
  bare-metal / container command basis.
