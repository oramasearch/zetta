# OCI provisioner (cloud burst)

The **`OciProvisioner`** (`include/zetta/oci_provisioner.hpp`) is the Oracle Cloud
Infrastructure cloud-burst backend behind the [`Provisioner`](provisioner.md) seam
— a **mirror** of the reference [`AzureProvisioner`](azure-provisioner.md) (`#376`)
and [`GcpProvisioner`](gcp-provisioner.md) (`#378`), following the
[cloud-provisioner CLI pattern](cloud-provisioner-pattern.md). It realizes
**ARC-13 / ADR-20** in the native distributed accelerated-compute pass for UC-1
churn (`#264`, `#432`).

The lifecycle is identical on every cloud: **burst** N ephemeral nodes → **mint**
bulk identities so they can join the cluster → run cluster-parallel → **tear down**
+ **revoke** those identities. `OciProvisioner` is structurally identical to
`AzureProvisioner` / `GcpProvisioner` — only the config type and the CLI binding
differ.

## How it fits

```
                       Provisioner (vendor-neutral seam)
                                   ▲
                                   │ implements
                          OciProvisioner ─────────────────► IdentityHooks (optional)
                          burst lifecycle + identity         mint on provision /
                                   │ delegates OCI work       revoke on teardown
                                   ▼                          (→ BurstIdentityManager)
                          OciControlPlane (seam)
                                   ▲
                                   │ implements
                          OciCliControlPlane ─────────────► CommandRunner
                          shells out to the `oci` CLI         exec argv, capture JSON
```

`OciProvisioner` owns the **burst orchestration** (provision → mint → teardown →
revoke) but **none of the OCI mechanics**. It delegates those to an injected
`OciControlPlane`, so it stays in `zetta-core` with **no cloud SDK** and is fully
unit-testable against a fake. The reference `OciControlPlane` is
`OciCliControlPlane`, which shells out to the `oci` CLI (no OCI C++ SDK) — a REST
or SDK binding could replace it without touching the provisioner.

> **"No cloud SDK lives behind the seam."** Same rule the `Provisioner` header
> states and the same isolation SDL/ImGui and CUDA get. The `oci` binding is plain
> subprocess + JSON parsing — no OCI headers, no new build dependency. The shared
> `CommandRunner` (`include/zetta/command_runner.hpp`) is the same exec seam the
> Azure `az` and GCP `gcloud` bindings use.

## The burst lifecycle

| Operation | Behaviour |
|---|---|
| `provision(request)` | Calls `control_plane.bring_up(config, request)` to scale the burst pool (an **Instance Pool**) to `request.count` (best-effort — preemptible capacity / quota may yield fewer). When any node comes up and a mint hook is wired, **mints bulk identities** for the returned nodes so they can authenticate and join. `count` 0 is a satisfied no-op that touches neither the control plane nor the hooks. |
| `teardown(node_id)` | Calls `control_plane.destroy(config, node_id)` (idempotent — unknown id → `false`). When a successful teardown drains the pool to **empty**, **bulk-revokes** the job's identities (the revoke hook). |
| `provisioned()` | Delegates to `control_plane.list(config)`, so it reflects **live capacity** — a preemptible reclaim disappears without a teardown call, exactly as `LocalProvisioner` reflects self-healing membership. |

The control plane is the **source of truth** for what is up: `provisioned()` and
the "did this teardown drain the pool?" check both read it, so the provisioner
keeps no parallel copy of fleet state (the
[everything-is-derived rule](replication.md)).

### Bulk identity — minted on provision, revoked on full teardown

The issue's "join with bulk identity" is wired through two optional
**`IdentityHooks`** callables, not a hard dependency on
[`BurstIdentityManager`](burst-identity.md) — identical to the Azure / GCP
references:

```cpp
struct IdentityHooks {
    std::function<void(const std::vector<ProvisionedNode>&)> mint;   // on provision
    std::function<void()> revoke;                                    // on full teardown
};
```

Production binds them to a `BurstIdentityManager` for the job:

```cpp
BurstIdentityManager identities{node};
OciProvisioner::IdentityHooks hooks;
hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
    identities.mint(job_id, nodes, Scope{/* the job's project */}, ttl_caps);
};
hooks.revoke = [&] { identities.revoke(job_id); };

OciCliControlPlane oci_cli;                       // the reference `oci` binding
OciProvisioner provisioner{oci_cli, config, hooks};
```

`BurstIdentityManager::mint` is **idempotent + additive** and `revoke(job_id)` is
**bulk**, so this maps cleanly onto the per-node seam: mint for whatever a burst
brought up, and revoke the whole job once the last node is torn down. Because the
burst credentials are **short-lived** (TTL'd capabilities, see
[access-control.md](access-control.md)), a node lost to a preemptible reclaim —
which never calls `teardown` — has its credential expire on its own; bulk revoke is
the clean-shutdown path, the TTL is the backstop. `tests/test_oci_provisioner.cpp`
exercises this against a real `BurstIdentityManager`.

## Configuration (`OciConfig`)

`OciConfig` is where on OCI to burst, how to size and price the nodes, and how each
joins the cluster. Only the compartment, availability domain, and subnet are
required; everything else has a sensible default.

| Field | Type | Required | Meaning |
|---|---|:--:|---|
| `compartment_id` | `string` | ✓ | the OCI compartment OCID the pool is created in |
| `availability_domain` | `string` | ✓ | the availability domain (e.g. `Uocm:PHX-AD-1`) |
| `subnet_id` | `string` | ✓ | the subnet OCID the primary VNIC attaches to |
| `region` | `string` | | the OCI region (e.g. `us-ashburn-1`); empty → the CLI profile's |
| `pool_name` | `string` | | Instance Pool display name; stable → idempotent resize (default `zetta-burst`) |
| `shape` | `string` | | the compute shape (default `VM.Standard.E5.Flex`) |
| `ocpus` | `uint32` | | OCPUs per node for a flexible shape (the shapeConfig); 0 omits it (default `1`) |
| `memory_in_gbs` | `uint32` | | memory per node for a flexible shape; 0 omits it (default `16`) |
| `image_id` | `string` | | the image OCID nodes boot from; empty → the config default |
| `spot` | `bool` | | burst preemptible (cheaper, evictable) capacity (default `false`) |
| `seed_peer` | `string` | | cluster seed `host:port` injected into each node's cloud-init so it auto-joins |

> **OCI vs. Azure/GCP config.** OCI is OCID-addressed and uses a separate
> **Instance Configuration** (the per-node template) that the **Instance Pool**
> instantiates, so the per-node shape (shape / shapeConfig / image / preemptible /
> cloud-init seed) lives in that configuration, not on the pool create. `ocpus` /
> `memory_in_gbs` size a *flexible* shape's `shapeConfig`. Otherwise the field set
> mirrors `AzureConfig` / `GcpConfig`.

### JSON boundary + schema

`OciConfig` has a **validated JSON boundary** (`to_json` / non-throwing `from_json`),
the same pattern as [`AzureConfig`](azure-provisioner.md) /
[`GcpConfig`](gcp-provisioner.md): `from_json` returns `std::nullopt` on anything
malformed — not an object, a missing/empty required field, a wrong-typed field, or
a negative number — so a misconfigured burst fails fast instead of bringing up the
wrong (or no) fleet. The shape is documented by
[`schemas/oci_config.schema.json`](../schemas/oci_config.schema.json), which the
`from_json` checks **mirror** — keep the two in sync. Optional string fields are
emitted by `to_json` only when set; the numeric `ocpus` / `memory_in_gbs` and the
`spot` flag are always emitted, so a round-trip is stable.

## The reference `oci` binding (`OciCliControlPlane`)

`OciCliControlPlane` (`include/zetta/oci_cli_control_plane.hpp`) is the reference
`OciControlPlane`. It borrows the **Instance Pool + preemptible shape** (not the
SDK): a stable, named Instance Pool (created from an Instance Configuration) *is*
the burst pool, resizing it is the burst, detaching+terminating an instance is
teardown. It builds `oci compute-management` argv, runs them through an injected
**`CommandRunner`**, and parses the JSON the CLI returns.

| `OciControlPlane` call | `oci` command (shape) |
|---|---|
| `bring_up` | resolve pool OCID by name (`oci compute-management instance-pool list … --query 'data[0].id' --raw-output`); **create path** → `instance-configuration create --instance-details <json>` then `instance-pool create --instance-configuration-id … --placement-configurations <json> --size N`; **resize path** → `instance-pool update --instance-pool-id … --size N`; then `instance-pool list-instances` |
| `destroy` | `oci compute-management instance-pool-instance detach --instance-pool-id … --instance-id <id> --is-auto-terminate true --is-decrement-size true` |
| `list` | `oci compute-management instance-pool list-instances --instance-pool-id …` |

> **Why the extra resolve step.** Unlike Azure (resource group + name) and GCP
> (project + zone + name), **OCI addresses resources by OCID, not name**. So the
> binding first resolves the pool's OCID from its stable display name (a single
> `--query data[0].id --raw-output` call that prints just the OCID). An empty
> result means no pool → the **create path** (ensure the instance configuration,
> then create the pool); a non-empty result → the **resize path** (`update --size
> N`). This is what keeps a re-burst idempotent on the stable `pool_name`. The
> per-node shape — including `spot` (`preemptibleInstanceConfig`) and the
> `seed_peer` (cloud-init `user_data`) — is built into the instance-configuration
> JSON, which is where those config fields surface.

`bring_up` lists what is up after scaling and reports best-effort against the
requested count. `destroy` uses **DetachInstancePoolInstance** with
`--is-decrement-size` (shrink the pool so it is not replaced) +
`--is-auto-terminate` (delete the instance + its boot volume) — the per-node
teardown the seam contract wants. The command shapes are grounded in
[research/oci-cli-compute.md](research/oci-cli-compute.md) (the cited `oci` guide —
§2.10 Instance Pools + autoscaling, §2.8 preemptible).

### The command runner — testable without `oci`

```cpp
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
```

`CommandRunner` is **shared, vendor-neutral** infrastructure
(`include/zetta/command_runner.hpp`) — the same exec seam the Azure `az` and GCP
`gcloud` bindings use (see
[cloud-provisioner-pattern.md](cloud-provisioner-pattern.md)). The runner takes
**argv** (program + arguments, no shell — so no shell-injection risk) and returns
`{ exit_code, stdout_text, stderr_text }`. The default (`default_command_runner()`)
`fork`/`execvp`s `oci` and captures its merged output; a test injects a fake that
asserts on the argv and returns canned output (resolved OCIDs as `--raw-output`
stdout, the listing as the OCI `{ "data": [ … ] }` envelope). That makes the
**command construction + JSON parsing** — the parts most likely to drift — fully
unit-tested. The actual `oci` exec is the only path CI cannot run; a production
deployment installs and authenticates the CLI (API key / session token / instance
principal).

The `list` output is parsed at the **`ProvisionedNode` boundary**
(`ProvisionedNode::from_json`), unwrapping OCI's `{ "data": [ … ] }` envelope and
skipping malformed entries — a production binding projects `instance-pool
list-instances` output into the `{ node_id, address, capability }` shape
(correlating instances to cluster node ids via tags / instance metadata), and the
reference binding consumes that shape directly.

## What this slice does **not** do

- **No live-cloud CI.** The acceptance criterion ("provision N nodes that join the
  cluster and are torn down against a configured OCI compartment") is realized as
  the full *shape* — the seam, config + schema, lifecycle, identity wiring, and the
  `oci` binding — verified against fakes. Running it against a real tenancy is a
  manual / deployment step (install + auth `oci`).
- **No persistence / replication.** `OciConfig` and the in-flight burst are
  in-memory; the config's JSON form exists for forward use (configuring /
  replicating a backend).
- **No autoscaler wiring.** The autoscaler / job coordinator that *drives*
  `provision`/`teardown` (and the [cost-cap approval gate](burst-approval-gate.md)
  that wraps it) is a separate, later slice. `OciProvisioner` drops in wherever one
  holds a `Provisioner&`.

## Tests

`tests/test_oci_provisioner.cpp` mirrors the Azure / GCP tests — the config
boundary, the lifecycle, and the reference binding:

- **`OciConfig` JSON boundary** — round-trips a minimal and a full config; the
  defaults (`pool_name`, `shape`, `ocpus`, `memory_in_gbs`, `spot`); and rejects
  malformed documents (non-object, each missing/empty required field, wrong-typed
  `compartment_id` / `memory_in_gbs` / `spot`, negative `ocpus`).
- **lifecycle (fake control plane)** — provision brings up nodes **and mints
  identities** for exactly what came up; best-effort when the pool is short;
  zero-count is a no-op; **no mint** when nothing comes up; teardown
  **bulk-revokes when the pool drains** (once, on the last node) and an unknown id
  is an idempotent no-op; `provisioned()` reflects a preemptible reclaim; and it
  works with no identity hooks wired. A dedicated test composes it with a **real
  `BurstIdentityManager`** (burst → mint per node → teardown → bulk revoke).
- **`OciCliControlPlane` (fake command runner)** — the **create path** (resolve
  finds no pool → instance-configuration create with the per-node launch JSON +
  instance-pool create + list); the **resize path** (resolve finds a pool → update
  `--size`, no create); a Spot config adds `preemptibleInstanceConfig` + the
  cloud-init seed to the instance-details JSON; best-effort when fewer instances
  come up; a create failure surfaces in the `detail`; `destroy` builds
  `instance-pool-instance detach --is-auto-terminate --is-decrement-size` (and is a
  no-op when no pool resolves); `list` unwraps the `data` envelope, parses, and
  skips malformed entries, returning empty when no pool resolves or the command
  fails.

## See also

- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the recipe this
  backend follows (the four pieces + shared infra + the lifecycle→CLI mapping).
- [azure-provisioner.md](azure-provisioner.md) — the **reference** backend.
- [gcp-provisioner.md](gcp-provisioner.md) — the sibling GCP backend.
- [provisioner.md](provisioner.md) — the seam + the given-hardware Local backend.
- [burst-identity.md](burst-identity.md) — the `BurstIdentityManager` the
  mint/revoke hooks bind to.
- [burst-approval-gate.md](burst-approval-gate.md) — the cost-cap decorator that
  can wrap any backend.
- [research/oci-cli-compute.md](research/oci-cli-compute.md) — the cited `oci`
  command basis (the Instance Pool + preemptible slice the binding uses).
