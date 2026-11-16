# GCP provisioner (cloud burst)

The **`GcpProvisioner`** (`include/zetta/cloud/gcp_provisioner.hpp`) is the GCP
cloud-burst backend behind the [`Provisioner`](provisioner.md) seam — the first
**mirror** of the reference [`AzureProvisioner`](azure-provisioner.md) (`#376`),
following the [cloud-provisioner CLI pattern](cloud-provisioner-pattern.md). It
realizes **ARC-13 / ADR-20** in the native distributed accelerated-compute pass
for UC-1 churn (`#264`, `#378`).

The lifecycle is identical on every cloud: **burst** N ephemeral nodes → **mint**
bulk identities so they can join the cluster → run cluster-parallel → **tear down**
+ **revoke** those identities. `GcpProvisioner` is structurally identical to
`AzureProvisioner` — only the config type and the CLI binding differ.

## How it fits

```
                       Provisioner (vendor-neutral seam)
                                   ▲
                                   │ implements
                          GcpProvisioner ─────────────────► IdentityHooks (optional)
                          burst lifecycle + identity         mint on provision /
                                   │ delegates GCP work       revoke on teardown
                                   ▼                          (→ BurstIdentityManager)
                          GcpControlPlane (seam)
                                   ▲
                                   │ implements
                          GcpCliControlPlane ─────────────► CommandRunner
                          shells out to the `gcloud` CLI      exec argv, capture JSON
```

`GcpProvisioner` owns the **burst orchestration** (provision → mint → teardown →
revoke) but **none of the GCP mechanics**. It delegates those to an injected
`GcpControlPlane`, so it stays in `zetta-core` with **no cloud SDK** and is fully
unit-testable against a fake. The reference `GcpControlPlane` is
`GcpCliControlPlane`, which shells out to the `gcloud` CLI (no GCP C++ SDK) — a
REST or SDK binding could replace it without touching the provisioner.

> **"No cloud SDK lives behind the seam."** This is the same rule the
> `Provisioner` header states and the same isolation SDL/ImGui and CUDA get. The
> `gcloud` binding is plain subprocess + JSON parsing — no GCP headers, no new
> build dependency. The shared `CommandRunner` (`include/zetta/cloud/command_runner.hpp`)
> is the same exec seam the Azure `az` binding uses.

## The burst lifecycle

| Operation | Behaviour |
|---|---|
| `provision(request)` | Calls `control_plane.bring_up(config, request)` to scale the burst pool (a Managed Instance Group) to `request.count` (best-effort — Spot capacity / quota may yield fewer). When any node comes up and a mint hook is wired, **mints bulk identities** for the returned nodes so they can authenticate and join. `count` 0 is a satisfied no-op that touches neither the control plane nor the hooks. |
| `teardown(node_id)` | Calls `control_plane.destroy(config, node_id)` (idempotent — unknown id → `false`). When a successful teardown drains the pool to **empty**, **bulk-revokes** the job's identities (the revoke hook). |
| `provisioned()` | Delegates to `control_plane.list(config)`, so it reflects **live capacity** — a Spot preemption disappears without a teardown call, exactly as `LocalProvisioner` reflects self-healing membership. |

The control plane is the **source of truth** for what is up: `provisioned()` and
the "did this teardown drain the pool?" check both read it, so the provisioner
keeps no parallel copy of fleet state (the
[everything-is-derived rule](../cluster/replication.md)).

### Bulk identity — minted on provision, revoked on full teardown

The issue's "join with bulk identity" is wired through two optional
**`IdentityHooks`** callables, not a hard dependency on
[`BurstIdentityManager`](burst-identity.md) — the same injection style as
`LocalProvisioner`'s `FleetSource`, and identical to the Azure reference:

```cpp
struct IdentityHooks {
    std::function<void(const std::vector<ProvisionedNode>&)> mint;   // on provision
    std::function<void()> revoke;                                    // on full teardown
};
```

Production binds them to a `BurstIdentityManager` for the job:

```cpp
BurstIdentityManager identities{node};
GcpProvisioner::IdentityHooks hooks;
hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
    identities.mint(job_id, nodes, Scope{/* the job's project */}, ttl_caps);
};
hooks.revoke = [&] { identities.revoke(job_id); };

GcpCliControlPlane gcloud;                          // the reference `gcloud` binding
GcpProvisioner provisioner{gcloud, config, hooks};
```

`BurstIdentityManager::mint` is **idempotent + additive** and `revoke(job_id)` is
**bulk** (drop every identity for the job), so this maps cleanly onto the per-node
seam: mint for whatever a burst brought up, and revoke the whole job once the last
node is torn down. Because the burst credentials are **short-lived** (TTL'd
capabilities, see [access-control.md](../governance/access-control.md)), a node lost to a Spot
preemption — which never calls `teardown` — has its credential expire on its own;
bulk revoke is the clean-shutdown path, the TTL is the backstop. This is the same
[`burst-identity`](burst-identity.md) wiring the Azure reference uses; `tests/
test_gcp_provisioner.cpp` exercises it against a real `BurstIdentityManager`.

## Configuration (`GcpConfig`)

`GcpConfig` is where on GCP to burst, how to size and price the nodes, and how each
joins the cluster. Only the project and zone are required; everything else has a
sensible default.

| Field | Type | Required | Meaning |
|---|---|:--:|---|
| `project_id` | `string` | ✓ | the GCP project the pool is created in |
| `zone` | `string` | ✓ | the Compute Engine zone the MIG runs in (e.g. `us-central1-a`) |
| `pool_name` | `string` | | Managed Instance Group name; stable → idempotent create/delete (default `zetta-burst`) |
| `instance_template` | `string` | | the instance template the MIG instantiates (default `zetta-burst-template`) |
| `machine_type` | `string` | | Compute Engine machine type; empty → the control plane maps the capability spec to one |
| `image` | `string` | | image family the nodes boot from (e.g. `debian-12`); empty → the control plane's default |
| `network` | `string` | | VPC network; empty → the project default |
| `subnetwork` | `string` | | subnetwork within the region; empty → the auto subnet |
| `service_account` | `string` | | the service account granted to burst nodes (the cloud-native side of the per-node identity); on a secure cluster it reads the join token from Secret Manager (#1316). Empty → the project's default compute service account |
| `spot` | `bool` | | burst Spot / preemptible (cheaper, evictable) capacity (default `false`) |
| `seed_peer` | `string` | | cluster seed `host:port` injected into each node's startup-script so it auto-joins |

> **GCP vs. Azure config.** GCP has no `max_price` (Spot pricing is automatic, not a
> per-node bid like Azure's), and Spot is a property of the **instance template**
> (`--provisioning-model=SPOT`), not the pool-create call — so the binding applies
> it when ensuring the template. Otherwise the field set mirrors `AzureConfig`.

### JSON boundary + schema

`GcpConfig` has a **validated JSON boundary** (`to_json` / non-throwing `from_json`),
the same pattern as [`AzureConfig`](azure-provisioner.md) and
[`NodeCapability`](provisioner.md): `from_json` returns `std::nullopt` on anything
malformed — not an object, a missing/empty required field, or a wrong-typed field —
so a misconfigured burst fails fast instead of bringing up the wrong (or no) fleet.
The shape is documented by [`schemas/gcp_config.schema.json`](../../schemas/gcp_config.schema.json),
which the `from_json` checks **mirror** — keep the two in sync. Optional fields are
emitted by `to_json` only when set, so a round-trip is stable and a minimal config
reads cleanly.

## The reference `gcloud` binding (`GcpCliControlPlane`)

`GcpCliControlPlane` (`include/zetta/cloud/gcp_cli_control_plane.hpp`) is the reference
`GcpControlPlane`. It borrows the **Managed Instance Group + Spot shape** (not the
SDK): a stable, named MIG *is* the burst pool, scaling it up is the burst, deleting
instances is teardown. It builds `gcloud compute` argv, runs them through an
injected **`CommandRunner`**, and parses the JSON the CLI returns.

| `GcpControlPlane` call | `gcloud` command (shape) |
|---|---|
| `bring_up` | on a secure cluster first `gcloud secrets create <pool>-cluster-token --project … --replication-policy automatic --data-file <private file>`, or `gcloud secrets versions add <pool>-cluster-token --project … --data-file <private file>` when it exists (fail-closed — #1316); then `gcloud compute instance-templates create <template> [--machine-type … --image-family … --network … --subnet … --service-account … --provisioning-model SPOT --instance-termination-action STOP --scopes cloud-platform --metadata-from-file startup-script=<file>]`, then `gcloud compute instance-groups managed create <pool> --template <template> --size N`, then `… managed resize <pool> --size N`, then `list`. The startup-script is the [node bootstrap script](node-bootstrap-script.md): the seed and the secret's name, never the token |
| `destroy` | `gcloud compute instance-groups managed delete-instances <pool> --instances <node_id> --quiet` |
| `list` | `gcloud compute instance-groups managed list-instances <pool> --format json` |

`bring_up` is **create-or-resize**: it ensures the instance template (an idempotent
ensure — an "already exists" failure is the steady state, immutable templates are
reused across bursts), then *both* creates and resizes the MIG to N. A fresh MIG is
created (create succeeds); an existing one is scaled (create fails "already exists",
resize succeeds) — the pool is ready iff **either** succeeded, which is what makes a
re-burst idempotent on the stable `pool_name`. It then **lists** what is up and
reports best-effort against the requested count. Each node gets the
[node bootstrap script](node-bootstrap-script.md) as its `startup-script`
metadata: the guest agent runs it at boot, it fetches the join token from Secret
Manager, and starts the engine with the seed peer and `ZETTA_CLUSTER_TOKEN_FILE`.
The command shapes are grounded in
[research/gcp-cli-compute.md](../research/gcp-cli-compute.md) (the cited `gcloud`
guide — MIG create / resize / list-instances / delete-instances, and
`--provisioning-model=SPOT`; §2.7–2.8).

### The command runner — testable without `gcloud`

```cpp
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
```

`CommandRunner` is **shared, vendor-neutral** infrastructure
(`include/zetta/cloud/command_runner.hpp`) — the same exec seam the Azure `az` binding
uses and the AWS `aws` binding will (see
[cloud-provisioner-pattern.md](cloud-provisioner-pattern.md)). The runner takes
**argv** (program + arguments, no shell — so no shell-injection risk) and returns
`{ exit_code, stdout_text, stderr_text }`. The default (`default_command_runner()`)
`fork`/`execvp`s `gcloud` and captures its merged output; a test injects a fake
that asserts on the argv and returns canned JSON. That makes the **command
construction + JSON parsing** — the parts most likely to drift — fully unit-tested.
The actual `gcloud` exec is the only path CI cannot run; a production deployment
installs and authenticates the CLI (`gcloud auth` / a service account).

The `list` output is parsed at the **`ProvisionedNode` boundary**
(`ProvisionedNode::from_json`), skipping malformed entries — a production binding
projects `gcloud … list-instances --format json` output into the
`{ node_id, address, capability }` shape (correlating GCE instances to cluster node
ids via labels / instance metadata), and the reference binding consumes that shape
directly.

## What this slice does **not** do

- **No live-cloud CI.** The acceptance criterion ("provision N nodes that join the
  cluster and are torn down against a configured GCP project") is realized as the
  full *shape* — the seam, config + schema, lifecycle, identity wiring, and the
  `gcloud` binding — verified against fakes. Running it against a real project is a
  manual / deployment step (install + auth `gcloud`).
- **No persistence / replication.** `GcpConfig` and the in-flight burst are
  in-memory; the config's JSON form exists for forward use (configuring /
  replicating a backend).
- **Not yet driven by a live autoscaler.** The
  [autoscaler control loop](../compute/job-autoscaler.md) (#371) and
  [job coordinator](../compute/job-coordinator.md) are built and act through any
  `Provisioner&` (optionally wrapped by the
  [cost-cap approval gate](burst-approval-gate.md)); wiring them to drive *this*
  backend in a live burst is the remaining slice. `GcpProvisioner` drops in
  wherever one holds a `Provisioner&`.

### The join token — Secret Manager and the node service account (#1316)

Instance metadata is not a secret store: any principal that can describe the
template or the instance can read it, and so can every process on the VM through
the metadata server. Templates are immutable, so whatever goes in one stays there.
So the token never rides it. On a secure cluster `bring_up`:

1. stores the token in Secret Manager as the secret `<pool_name>-cluster-token` in
   `project_id`: `gcloud secrets create … --data-file <private file>` on the first
   burst, `gcloud secrets versions add … --data-file` on later ones (so a rotated
   token reaches new nodes). If both fail, the burst stops before the template or
   the MIG is touched;
2. creates the template with `--scopes cloud-platform` (Secret Manager needs that
   OAuth scope on the node's access token; IAM then decides what it may read) and
   the boot script as `startup-script`, which names the project and the secret;
3. at boot, the script takes an access token for the instance's service account
   from the metadata server, calls Secret Manager's
   `projects/<project>/secrets/<pool>-cluster-token/versions/latest:access`,
   base64-decodes the payload, and writes it to `/etc/zetta/cluster-token` (0400).

**What the operator grants.**

| Principal | Needs | Role |
|---|---|---|
| The control plane (the account `gcloud` runs as on the zetta node) | create the secret and add versions | `roles/secretmanager.admin` on the project; or create `<pool>-cluster-token` once yourself and grant **`roles/secretmanager.secretVersionAdder`** on that secret only (the create then fails and `bring_up` adds a version) |
| The burst nodes' service account (`service_account`, or the default compute service account) | read the latest version | **`roles/secretmanager.secretAccessor`** on the secret `<pool>-cluster-token` |

```sh
gcloud secrets create zetta-burst-cluster-token --project example-churn \
  --replication-policy automatic   # once, no version yet
gcloud secrets add-iam-policy-binding zetta-burst-cluster-token --project example-churn \
  --member serviceAccount:burst@example-churn.iam.gserviceaccount.com \
  --role roles/secretmanager.secretAccessor
gcloud secrets add-iam-policy-binding zetta-burst-cluster-token --project example-churn \
  --member serviceAccount:<control-plane account> \
  --role roles/secretmanager.secretVersionAdder
```

The Secret Manager API must be enabled on the project
(`gcloud services enable secretmanager.googleapis.com`). The VM image needs
`curl`, `base64` and a POSIX `sh` (the Debian and Ubuntu images have them).

**A template from before #1316 still holds the token.** The binding reuses an
existing template of the same name (templates are immutable), so one created by an
earlier build keeps the token in its `startup-script` metadata and keeps handing it
out. Delete it (`gcloud compute instance-templates delete <template>`) so the next
burst creates a clean one, and rotate the cluster token, since it was readable by
anyone who could describe that template.

## Tests

`tests/cloud/test_gcp_provisioner.cpp` mirrors the Azure test — the config boundary, the
lifecycle, and the reference binding:

- **`GcpConfig` JSON boundary** — round-trips a minimal and a full config; the
  defaults (`pool_name`, `instance_template`, `spot`); and rejects malformed
  documents (non-object, each missing/empty required field, wrong-typed
  `project_id` / `spot`).
- **lifecycle (fake control plane)** — provision brings up nodes **and mints
  identities** for exactly what came up; best-effort when the pool is short; zero-
  count is a no-op; **no mint** when nothing comes up; teardown **bulk-revokes when
  the pool drains** (once, on the last node) and an unknown id is an idempotent
  no-op; `provisioned()` reflects a Spot preemption; and it works with no identity
  hooks wired. A dedicated test composes it with a **real `BurstIdentityManager`**
  (burst → mint per node → teardown → bulk revoke).
- **`GcpCliControlPlane` (fake command runner)** — `bring_up` ensures the instance
  template (machine type / image / service account flags), creates + resizes the
  MIG, then lists; a Spot config adds `--provisioning-model SPOT` /
  `--instance-termination-action` to the template; on a secure cluster the token
  goes to Secret Manager first through a private `--data-file` (create, or
  `versions add` on a re-burst), the template gets `--scopes cloud-platform` and a
  startup-script that names the secret and never holds the token, and a Secret
  Manager refusal stops the burst before the template (#1316);
  create-or-resize succeeds when only the resize does (re-burst); best-effort when
  fewer instances come up; a create+resize failure surfaces in the `detail`;
  `destroy` builds `delete-instances --quiet` and maps a non-zero exit to `false`;
  `list` parses the instances and skips malformed entries, and returns empty on a
  command failure.

## See also

- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the recipe this
  backend follows (the four pieces + shared infra + the lifecycle→CLI mapping).
- [azure-provisioner.md](azure-provisioner.md) — the **reference** backend this
  mirrors.
- [provisioner.md](provisioner.md) — the seam + the given-hardware Local backend.
- [burst-identity.md](burst-identity.md) — the `BurstIdentityManager` the
  mint/revoke hooks bind to.
- [burst-approval-gate.md](burst-approval-gate.md) — the cost-cap decorator that
  can wrap any backend.
- [research/gcp-cli-compute.md](../research/gcp-cli-compute.md) — the cited `gcloud`
  command basis (the Managed Instance Group + Spot slice the binding uses).
