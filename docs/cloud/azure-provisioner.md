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
| `managed_identity` | `string` | on a secure cluster | user-assigned managed identity (resource id) granted to burst nodes; each node reads the join token from `key_vault` with it (#1316) |
| `key_vault` | `string` | on a secure cluster | the Key Vault the join token is stored in, as the secret `<pool_name>-cluster-token` (#1316) |
| `tenant_id` | `string` | | optional Azure AD tenant id |

Inter-node mTLS is always on (#799), so every burst from a running node is a
secure-cluster burst: without `key_vault` and `managed_identity`, `bring_up`
refuses with a detail naming both and creates nothing. See
[the join token](#the-join-token--key-vault-and-a-managed-identity-1316).

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
| `bring_up` | on a secure cluster first `az keyvault secret set --vault-name <key_vault> --name <pool>-cluster-token --file <private file> --encoding utf-8 --output none` (fail-closed — #1316); then `az group create`, then `az vmss create … --orchestration-mode Flexible --instance-count N [--vm-sku … --image … --priority Spot --eviction-policy Delete --max-price … --assign-identity <managed_identity> --custom-data <node bootstrap script>]`, falling back across SKUs on a capacity shortage. The custom-data is the [node bootstrap script](node-bootstrap-script.md): the seed and the secret's name, never the token |
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
existing scale set rather than spawn a duplicate. Each node gets the
[node bootstrap script](node-bootstrap-script.md) as `--custom-data`: cloud-init
runs it at first boot, it fetches the join token from Key Vault, and starts the
engine with the seed peer and `ZETTA_CLUSTER_TOKEN_FILE`.

### The join token — Key Vault and a managed identity (#1316)

Custom-data is not a secret store: every process on the VM can read it from the
metadata endpoint `169.254.169.254`, and it persists on the VM's disk
(`/var/lib/waagent/CustomData`). So the token never rides it. On a secure cluster
`bring_up`:

1. stores the token as the Key Vault secret `<pool_name>-cluster-token` in
   `key_vault` (`az keyvault secret set --file <private file>`; the value is never
   an argument, and `--output none` keeps `az` from printing it back). If that
   fails, the burst stops before any resource group or VM is created;
2. creates the scale set with `--assign-identity <managed_identity>` and the boot
   script as custom-data. The script names the vault, the secret and the identity;
3. at boot, the script asks the metadata endpoint for a Key Vault access token for
   that identity (`resource=https://vault.azure.net`, `msi_res_id=<identity>`),
   GETs `https://<key_vault>.vault.azure.net/secrets/<pool>-cluster-token`, and
   writes the value to `/etc/zetta/cluster-token` (0400).

Every burst writes the secret again (a new version), so a rotated cluster token
reaches the nodes that come up after it.

**What the operator grants.** Create the vault and the user-assigned identity
once; neither is created by the burst.

| Principal | Needs | RBAC-mode vault (recommended) | Access-policy vault |
|---|---|---|---|
| The control plane (the identity `az` runs as on the zetta node) | write the secret | **Key Vault Secrets Officer** on the vault, or on the secret once it exists | secret permission `set` |
| The control plane | attach the identity to the scale set | **Managed Identity Operator** on the user-assigned identity | (same) |
| The burst nodes' user-assigned identity (`managed_identity`) | read the secret | **Key Vault Secrets User** on the secret `<pool>-cluster-token` (or the vault) | secret permission `get` |

```sh
az identity create -g ops -n zetta-burst
az keyvault create -g ops -n zetta-kv --enable-rbac-authorization true
# the burst nodes read the token
az role assignment create --role "Key Vault Secrets User" \
  --assignee-object-id "$(az identity show -g ops -n zetta-burst --query principalId -o tsv)" \
  --assignee-principal-type ServicePrincipal \
  --scope "$(az keyvault show -n zetta-kv --query id -o tsv)"
# the control plane writes it
az role assignment create --role "Key Vault Secrets Officer" --assignee <control-plane principal> \
  --scope "$(az keyvault show -n zetta-kv --query id -o tsv)"
```

Then set `key_vault: "zetta-kv"` and `managed_identity: "<the identity's resource
id>"` in the provisioner config. The identity is user-assigned so the grant exists
before the VM boots; the script still retries for ten minutes while a fresh role
assignment propagates. The VM image needs `curl` and a POSIX `sh` (Ubuntu has both).

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
  SKU) then lists; a Spot config adds the `--priority Spot` / `--max-price`
  flags; on a secure cluster the token goes to Key Vault first through a private
  `--file`, the custom-data is a boot script naming the vault, the secret and the
  identity and never the token, a missing `key_vault` / `managed_identity` creates
  nothing, and a Key Vault refusal stops the burst before the resource group
  (#1316); best-effort when fewer instances come up; a create
  failure surfaces in the `detail`; `destroy` builds `delete-instances` and maps a
  non-zero exit to `false`; `list` parses the instances and skips malformed
  entries, and returns empty on a command failure.

See also [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) (how AWS /
GCP mirror this as the **reference**), [provisioner.md](provisioner.md) (the seam +
the Local backend), [burst-identity.md](burst-identity.md) (the
`BurstIdentityManager` the mint/revoke hooks bind to), and
[burst-approval-gate.md](burst-approval-gate.md) (the cost-cap decorator that can
wrap any backend).
