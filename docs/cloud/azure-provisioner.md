# Azure provisioner (reference cloud burst)

The **`AzureProvisioner`** (`include/zetta/cloud/azure_provisioner.hpp`) is the
**reference** cloud-burst backend behind the [`Provisioner`](provisioner.md) seam
— the first concrete *cloud* backend (the given-hardware
[`LocalProvisioner`](provisioner.md#the-local-backend-localprovisioner), `#374`,
is the other). It realizes **ARC-13 / ADR-20** in the native distributed
accelerated-compute pass for UC-1 churn (`#264`, `#376`).

Azure goes first; **AWS (`#377`) and GCP (`#378`) mirror this shape** with their
own provisioner + control-plane pair. The lifecycle is the same on every cloud:
**burst** N ephemeral nodes → **mint** bulk identities so they can join the
cluster → run cluster-parallel → **tear down** + **revoke** those identities.

## How it fits

```
                       Provisioner (vendor-neutral seam)
                                   ▲
                                   │ implements
                         AzureProvisioner ───────────────► IdentityHooks (optional)
                          burst lifecycle + identity        mint on provision /
                                   │ delegates Azure work    revoke on teardown
                                   ▼                         (→ BurstIdentityManager)
                          AzureControlPlane (seam)
                                   ▲
                                   │ implements
                          AzCliControlPlane ──────────────► CommandRunner
                          shells out to the `az` CLI         exec argv, capture JSON
```

`AzureProvisioner` owns the **burst orchestration** (provision → mint → teardown →
revoke) but **none of the Azure mechanics**. It delegates those to an injected
`AzureControlPlane`, so it stays in `zetta-core` with **no cloud SDK** and is
fully unit-testable against a fake. The reference `AzureControlPlane` is
`AzCliControlPlane`, which shells out to the `az` CLI (no Azure C++ SDK) — a REST
or SDK binding could replace it without touching the provisioner.

> **"No cloud SDK lives behind the seam."** This is the same rule the
> `Provisioner` header states and the same isolation SDL/ImGui and CUDA get. The
> `az` binding is plain subprocess + JSON parsing — no Azure headers, no new build
> dependency.

## The burst lifecycle

| Operation | Behaviour |
|---|---|
| `provision(request)` | Calls `control_plane.bring_up(config, request)` to scale the burst pool to `request.count` (best-effort — Spot capacity / quota may yield fewer). When any node comes up and a mint hook is wired, **mints bulk identities** for the returned nodes so they can authenticate and join. `count` 0 is a satisfied no-op that touches neither the control plane nor the hooks. |
| `teardown(node_id)` | Calls `control_plane.destroy(config, node_id)` (idempotent — unknown id → `false`). When a successful teardown drains the pool to **empty**, **bulk-revokes** the job's identities (the revoke hook). |
| `provisioned()` | Delegates to `control_plane.list(config)`, so it reflects **live capacity** — a Spot eviction disappears without a teardown call, exactly as `LocalProvisioner` reflects self-healing membership. |

The control plane is the **source of truth** for what is up: `provisioned()` and
the "did this teardown drain the pool?" check both read it, so the provisioner
keeps no parallel copy of fleet state (the
[everything-is-derived rule](../cluster/replication.md)).

### Bulk identity — minted on provision, revoked on full teardown

The issue's "join with bulk identity" is wired through two optional
**`IdentityHooks`** callables, not a hard dependency on
[`BurstIdentityManager`](burst-identity.md) — the same injection style as
`LocalProvisioner`'s `FleetSource` and `BurstApprovalGate`'s `CostEstimator`:

```cpp
struct IdentityHooks {
    std::function<void(const std::vector<ProvisionedNode>&)> mint;   // on provision
    std::function<void()> revoke;                                    // on full teardown
};
```

Production binds them to a `BurstIdentityManager` for the job:

```cpp
BurstIdentityManager identities{node};
AzureProvisioner::IdentityHooks hooks;
hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
    identities.mint(job_id, nodes, Scope{/* the job's project */}, ttl_caps);
};
hooks.revoke = [&] { identities.revoke(job_id); };

AzCliControlPlane azure_cli;                       // the reference `az` binding
AzureProvisioner provisioner{azure_cli, config, hooks};
```

`BurstIdentityManager::mint` is **idempotent + additive** and `revoke(job_id)` is
**bulk** (drop every identity for the job), so this maps cleanly onto the
per-node seam: mint for whatever a burst brought up, and revoke the whole job once
the last node is torn down. Because the burst credentials are **short-lived**
(TTL'd capabilities, see [access-control.md](../governance/access-control.md)), a node lost to a
Spot eviction — which never calls `teardown` — has its credential expire on its
own; bulk revoke is the clean-shutdown path, the TTL is the backstop.

## Configuration (`AzureConfig`)

`AzureConfig` is where on Azure to burst, how to size and price the nodes, and how
each joins the cluster. Only the subscription, resource group, and region are
required; everything else has a sensible default.

| Field | Type | Required | Meaning |
|---|---|:--:|---|
| `subscription_id` | `string` | ✓ | the Azure subscription the pool is created in |
| `resource_group` | `string` | ✓ | owns the burst pool; the unit deleted on full teardown |
| `location` | `string` | ✓ | the Azure region (e.g. `eastus`) |
| `pool_name` | `string` | | VMSS / Batch pool name; stable → idempotent create/delete (default `zetta-burst`) |
| `vm_size` | `string` | | VM SKU; empty → the control plane maps the capability spec to one |
| `image` | `string` | | VM image reference; empty → the control plane's default |
| `spot` | `bool` | | burst Spot / low-priority (cheaper, evictable) capacity (default `false`) |
| `max_price` | `number` | | Spot max $/node-hour; **-1 = up to on-demand** (never price-evicted), Azure's convention (default `-1`) |
| `seed_peer` | `string` | | cluster seed `host:port` injected into node bootstrap so each node auto-joins |
| `managed_identity` | `string` | | optional user-assigned managed identity granted to burst nodes |
| `tenant_id` | `string` | | optional Azure AD tenant id |

### JSON boundary + schema

`AzureConfig` has a **validated JSON boundary** (`to_json` / non-throwing
`from_json`), the same pattern as [`NodeCapability`](provisioner.md) and
[`BurstCap`](burst-approval-gate.md): `from_json` returns `std::nullopt` on
anything malformed — not an object, a missing/empty required field, a wrong-typed
field, or a `max_price` below the `-1` Azure floor — so a misconfigured burst
fails fast instead of bringing up the wrong (or unbounded) fleet. The shape is
documented by [`schemas/azure_config.schema.json`](../../schemas/azure_config.schema.json),
which the `from_json` checks **mirror** — keep the two in sync. Optional fields
are emitted by `to_json` only when set, so a round-trip is stable and a minimal
config reads cleanly.

## The reference `az` binding (`AzCliControlPlane`)

`AzCliControlPlane` (`include/zetta/cloud/azure_cli_control_plane.hpp`) is the reference
`AzureControlPlane`. It borrows the **VMSS + Spot shape** (not the SDK): a stable,
named scale set *is* the burst pool, scaling it up is the burst, deleting
instances is teardown. It builds `az vmss` argv, runs them through an injected
**`CommandRunner`**, and parses the JSON the CLI returns.

| `AzureControlPlane` call | `az` command (shape) |
|---|---|
| `bring_up` | `az vmss create … --orchestration-mode Flexible --instance-count N [--vm-sku … --image … --priority Spot --eviction-policy Delete --max-price … --assign-identity … --custom-data "ZETTA_SEED_PEER=… ZETTA_CLUSTER_TOKEN=…"]`, then `list` (the join token rides the custom-data blob only on a secure cluster — #670) |
| `destroy` | `az vmss delete-instances … --instance-ids <node_id>` |
| `list` | `az vmss list-instances … --output json` |

`--orchestration-mode Flexible` is the recommended mode for self-managed scaled
workers (independent VMs under one scale set) — the burst-cluster shape. The
command shapes are grounded in
[research/azure-cli-compute.md](../research/azure-cli-compute.md) (the cited `az`
guide); a true autoscaler resize on an existing pool would use
`az vmss scale --new-capacity`.

`bring_up` scales the pool then **lists** what is up and reports best-effort
against the requested count; the stable `pool_name` makes a re-burst scale the
existing scale set rather than spawn a duplicate. The seed peer is handed to each
node via `--custom-data` so it auto-joins at boot (a production deployment
supplies a full cloud-init; the reference carries the seed address illustratively).

### The command runner — testable without `az`

```cpp
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
```

`CommandRunner` is **shared, vendor-neutral** infrastructure
(`include/zetta/cloud/command_runner.hpp`) — the same exec seam the AWS / GCP CLI
bindings will run through (see
[cloud-provisioner-pattern.md](cloud-provisioner-pattern.md)). The runner takes
**argv** (program + arguments, no shell — so no shell-injection risk) and returns
`{ exit_code, stdout_text, stderr_text }`. The default
(`default_command_runner()`) `fork`/`execvp`s the program and captures its merged
output; a test injects a fake that asserts on the argv and returns canned JSON.
That makes the **command construction + JSON parsing** — the parts most likely to
drift — fully unit-tested. The actual `az` exec is the only path CI cannot run; a
production deployment installs and authenticates the CLI (service principal /
managed identity).

The `list` output is parsed at the **`ProvisionedNode` boundary**
(`ProvisionedNode::from_json`), skipping malformed entries — a production binding
projects `az vmss list-instances` output into the `{ node_id, address, capability }`
shape (correlating Azure instances to cluster node ids via tags / instance
metadata), and the reference binding consumes that shape directly.

## What this slice does **not** do

- **No live-cloud CI.** The acceptance criterion ("provision N nodes that join the
  cluster and are torn down against a configured Azure subscription") is realized
  as the full *shape* — the seam, config + schema, lifecycle, identity wiring, and
  the `az` binding — verified against fakes. Running it against a real subscription
  is a manual / deployment step (install + auth `az`).
- **No persistence / replication.** `AzureConfig` and the in-flight burst are
  in-memory; the config's JSON form exists for forward use (configuring /
  replicating a backend).
- **Not yet driven by a live autoscaler.** The
  [autoscaler control loop](../compute/job-autoscaler.md) (#371) and
  [job coordinator](../compute/job-coordinator.md) are built and act through any
  `Provisioner&` (optionally wrapped by the
  [cost-cap approval gate](burst-approval-gate.md)); wiring them to drive *this*
  backend in a live burst is the remaining slice. `AzureProvisioner` drops in
  wherever one holds a `Provisioner&`.

## Tests

`tests/cloud/test_azure_provisioner.cpp` covers the config boundary, the lifecycle, and
the reference binding:

- **`AzureConfig` JSON boundary** — round-trips a minimal and a full config; the
  defaults (`pool_name`, `spot`, `max_price = -1`); and rejects malformed
  documents (non-object, each missing/empty required field, wrong-typed `spot` /
  `max_price`, and a `max_price` below `-1`).
- **lifecycle (fake control plane)** — provision brings up nodes **and mints
  identities** for exactly what came up; best-effort when the pool is short
  (mint what came up, `satisfied == false`); zero-count is a no-op that touches
  neither the control plane nor the hooks; **no mint** when nothing comes up;
  teardown **bulk-revokes when the pool drains** (once, on the last node) and an
  unknown id is an idempotent no-op that does not revoke; `provisioned()` reflects
  a Spot eviction; and it works with no identity hooks wired.
- **`AzCliControlPlane` (fake command runner)** — `bring_up` builds the
  `az vmss create` argv (subscription / resource group / name / location / count /
  SKU) then lists; a Spot config adds the `--priority Spot` / `--max-price` /
  `--custom-data` seed flags; best-effort when fewer instances come up; a create
  failure surfaces in the `detail`; `destroy` builds `delete-instances` and maps a
  non-zero exit to `false`; `list` parses the instances and skips malformed
  entries, and returns empty on a command failure.

See also [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) (how AWS /
GCP mirror this as the **reference**), [provisioner.md](provisioner.md) (the seam +
the Local backend), [burst-identity.md](burst-identity.md) (the
`BurstIdentityManager` the mint/revoke hooks bind to), and
[burst-approval-gate.md](burst-approval-gate.md) (the cost-cap decorator that can
wrap any backend).
