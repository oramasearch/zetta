# Kubernetes provisioner (cloud burst)

The **`K8sProvisioner`** (`include/zetta/cloud/k8s_provisioner.hpp`) is the Kubernetes
cloud-burst backend behind the [`Provisioner`](provisioner.md) seam — a **mirror**
of the reference [`AzureProvisioner`](azure-provisioner.md) (`#376`) and the
[`GcpProvisioner`](gcp-provisioner.md) (`#378`) / [`OciProvisioner`](oci-provisioner.md)
(`#432`) backends, following the
[cloud-provisioner CLI pattern](cloud-provisioner-pattern.md). It realizes
**ARC-13 / ADR-20** in the native distributed accelerated-compute pass for UC-1
churn (`#264`, `#434`).

The lifecycle is identical on every backend: **burst** N ephemeral nodes → **mint**
bulk identities so they can join the cluster → run cluster-parallel → **tear down**
+ **revoke** those identities. `K8sProvisioner` is structurally identical to the
cloud backends — only the config type and the CLI binding differ.

> **Kubernetes is not a cloud — the cluster already exists.** The cloud backends
> create VMs; the Kubernetes backend bursts **worker pods into an existing
> cluster**. The "burst pool" is a **Deployment** scaled to N replicas — the
> self-healing, maintain-N analog of a VM Scale Set / Auto Scaling Group / Managed
> Instance Group / Instance Pool — and the cluster is the kubeconfig **context**,
> configured out-of-band (`az aks get-credentials` / `gcloud container clusters
> get-credentials` / `aws eks update-kubeconfig` / `oci ce cluster
> create-kubeconfig`). This is distinct from the `DeploymentTarget` placement dial
> (`local → kubernetes → vpc → cloud`, see [deployments.md](../pipeline/deployments.md)), which
> is about *where a graph runs*; `K8sProvisioner` provisions *compute nodes* for the
> distributed-compute fleet.

## How it fits

```
                       Provisioner (vendor-neutral seam)
                                   ▲
                                   │ implements
                          K8sProvisioner ─────────────────► IdentityHooks (optional)
                          burst lifecycle + identity         mint on provision /
                                   │ delegates K8s work       revoke on teardown
                                   ▼                          (→ BurstIdentityManager)
                          K8sControlPlane (seam)
                                   ▲
                                   │ implements
                          K8sCliControlPlane ─────────────► CommandRunner
                          shells out to the `kubectl` CLI     exec argv, capture JSON
```

`K8sProvisioner` owns the **burst orchestration** (provision → mint → teardown →
revoke) but **none of the Kubernetes mechanics**. It delegates those to an injected
`K8sControlPlane`, so it stays in `zetta-core` with **no Kubernetes client library**
and is fully unit-testable against a fake. The reference `K8sControlPlane` is
`K8sCliControlPlane`, which shells out to the `kubectl` CLI (no client-go / REST
binding) — a client-library binding could replace it without touching the
provisioner.

> **"No client library behind the seam."** Same rule the `Provisioner` header
> states and the same isolation SDL/ImGui and CUDA get. The `kubectl` binding is
> plain subprocess + JSON parsing — no Kubernetes headers, no new build dependency.
> The shared `CommandRunner` (`include/zetta/cloud/command_runner.hpp`) is the same exec
> seam the Azure `az`, GCP `gcloud`, and OCI `oci` bindings use.

## The burst lifecycle

| Operation | Behaviour |
|---|---|
| `provision(request)` | Calls `control_plane.bring_up(config, request)` to scale the burst Deployment to `request.count` (best-effort — node capacity / quota may yield fewer). When any pod comes up and a mint hook is wired, **mints bulk identities** for the returned pods so they can authenticate and join. `count` 0 is a satisfied no-op that touches neither the control plane nor the hooks. |
| `teardown(node_id)` | Calls `control_plane.destroy(config, node_id)` — deletes the pod by name (idempotent — unknown id → `false`). When a successful teardown drains the pool to **empty**, **bulk-revokes** the job's identities (the revoke hook). |
| `provisioned()` | Delegates to `control_plane.list(config)`, so it reflects **live capacity** — an evicted pod disappears without a teardown call, exactly as `LocalProvisioner` reflects self-healing membership. |

The control plane is the **source of truth** for what is up: `provisioned()` and
the "did this teardown drain the pool?" check both read it, so the provisioner
keeps no parallel copy of fleet state (the
[everything-is-derived rule](../cluster/replication.md)).

> **A note on the Deployment's self-healing.** On a real cluster a Deployment
> maintains its replica count, so deleting one pod is rescheduled by the controller;
> the burst's desired size is governed by `provision` (which scales the Deployment),
> and a full drain happens when the autoscaler scales it to zero. The per-pod
> `destroy` is the "this node is going away" signal. The fake control plane in tests
> models a simple decrementing pool, like every other backend's test, so the
> drain → revoke lifecycle is exercised identically.

### Bulk identity — minted on provision, revoked on full teardown

The issue's "join with bulk identity" is wired through two optional
**`IdentityHooks`** callables, not a hard dependency on
[`BurstIdentityManager`](burst-identity.md) — identical to the cloud references:

```cpp
struct IdentityHooks {
    std::function<void(const std::vector<ProvisionedNode>&)> mint;   // on provision
    std::function<void()> revoke;                                    // on full teardown
};
```

Production binds them to a `BurstIdentityManager` for the job:

```cpp
BurstIdentityManager identities{node};
K8sProvisioner::IdentityHooks hooks;
hooks.mint = [&](const std::vector<ProvisionedNode>& pods) {
    identities.mint(job_id, pods, Scope{/* the job's project */}, ttl_caps);
};
hooks.revoke = [&] { identities.revoke(job_id); };

K8sCliControlPlane kubectl;                       // the reference `kubectl` binding
K8sProvisioner provisioner{kubectl, config, hooks};
```

This is the zetta-side identity (a short-lived service `Principal` + `AccessKey` per
pod, scoped to the job), distinct from the pod's in-cluster `ServiceAccount` (set via
`config.service_account`). `tests/cloud/test_k8s_provisioner.cpp` exercises the mint/revoke
wiring against a real `BurstIdentityManager`.

## Configuration (`K8sConfig`)

`K8sConfig` is where in the cluster to burst, what image the pods run, and how each
joins. Only the container image is required; everything else has a sensible default.

| Field | Type | Required | Meaning |
|---|---|:--:|---|
| `image` | `string` | ✓ | the container image the worker pods run |
| `namespace` | `string` | | the namespace the Deployment + pods live in (default `default`) |
| `deployment_name` | `string` | | the burst Deployment (pool) name; stable → idempotent (default `zetta-burst`) |
| `context` | `string` | | the kubeconfig context to target; empty → the current context |
| `service_account` | `string` | | the pods' Kubernetes ServiceAccount; empty → the namespace default |
| `spot` | `bool` | | schedule the pods onto spot/preemptible nodes (a nodeSelector) (default `false`) |
| `seed_peer` | `string` | | cluster seed `host:port` injected as the `ZETTA_SEED_PEER` env var so each pod auto-joins |

> The C++ field is `namespace_name` (since `namespace` is a C++ keyword); the JSON
> key is `namespace`. There is no `region` / `account` — the cluster is the
> kubeconfig context, set up out-of-band.

### JSON boundary + schema

`K8sConfig` has a **validated JSON boundary** (`to_json` / non-throwing `from_json`),
the same pattern as [`AzureConfig`](azure-provisioner.md) /
[`GcpConfig`](gcp-provisioner.md) / [`OciConfig`](oci-provisioner.md): `from_json`
returns `std::nullopt` on anything malformed — not an object, a missing/empty
required `image`, or a wrong-typed field — so a misconfigured burst fails fast
instead of bringing up the wrong (or no) fleet. The shape is documented by
[`schemas/k8s_config.schema.json`](../../schemas/k8s_config.schema.json), which the
`from_json` checks **mirror** — keep the two in sync. Optional string fields are
emitted by `to_json` only when set; `spot` is always emitted.

## The reference `kubectl` binding (`K8sCliControlPlane`)

`K8sCliControlPlane` (`include/zetta/cloud/k8s_cli_control_plane.hpp`) is the reference
`K8sControlPlane`. It borrows the **Deployment + scale shape** (not a client
library): a stable, named Deployment *is* the burst pool, scaling it is the burst,
deleting a pod is teardown. It builds `kubectl` argv, runs them through an injected
**`CommandRunner`**, and parses the JSON the CLI returns.

| `K8sControlPlane` call | `kubectl` command (shape) |
|---|---|
| `bring_up` | `kubectl create deployment <name> --image <image>` (ensure); then per-pod shape — `kubectl set env deployment/<name> ZETTA_SEED_PEER=…`, `kubectl set env deployment/<name> ZETTA_IDENTITY_ROOT=<public key>` (#1343); on a secure cluster `kubectl apply --server-side --field-manager zetta --filename <private file>` (Secret `<name>-cluster-token`, key `ZETTA_CLUSTER_TOKEN`) then `kubectl set env deployment/<name> --from=secret/<name>-cluster-token` (a secretKeyRef — #670, #770), `kubectl set serviceaccount deployment/<name> <sa>`, `kubectl patch deployment <name> --patch <nodeSelector>` (spot); then `kubectl scale deployment/<name> --replicas N`; then `kubectl get pods --selector app=<name> --output json` |
| `destroy` | `kubectl delete pod <pod-name>` |
| `list` | `kubectl get pods --selector app=<name> --output json` |

Every call carries the kubeconfig scope (`--namespace <ns>`, and `--context <ctx>`
when pinned). `bring_up` is **create-or-scale**: it ensures the Deployment exists (an
idempotent ensure — an "already exists" failure is the steady state), applies the
per-pod shape onto its template, then `scale`s it to N (the authoritative, idempotent
burst step + the gate) and lists the pods, reporting best-effort against the
requested count. The pods are labelled `app=<deployment_name>` by `kubectl create
deployment`, which is what `list` and the Deployment select on. Spot scheduling is a
nodeSelector patch (illustrative label `zetta.io/capacity=spot` — production uses the
cluster's actual spot node-pool label, e.g. `cloud.google.com/gke-spot`); the seed
peer rides a `ZETTA_SEED_PEER` env var (and, on a secure cluster, the join token a `ZETTA_CLUSTER_TOKEN`
env var read from the Secret `<deployment>-cluster-token` through a secretKeyRef —
never a literal in the Deployment spec, never a `kubectl` argument — #670, #770); the per-pod identity is a `ServiceAccount`.
The command shapes are grounded in
[research/kubectl-workloads.md](../research/kubectl-workloads.md) (the cited `kubectl`
guide — §3.2 Deployments, §5.1 scaling, §6.2 nodeSelector targeting).

### The command runner — testable without `kubectl`

```cpp
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
```

`CommandRunner` is **shared, vendor-neutral** infrastructure
(`include/zetta/cloud/command_runner.hpp`) — the same exec seam the cloud CLI bindings use
(see [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md)). The runner takes
**argv** (program + arguments, no shell — so no shell-injection risk) and returns
`{ exit_code, stdout_text, stderr_text }`. The default (`default_command_runner()`)
`fork`/`execvp`s `kubectl` and captures its merged output; a test injects a fake that
asserts on the argv and returns canned JSON. That makes the **command construction +
JSON parsing** — the parts most likely to drift — fully unit-tested. The actual
`kubectl` exec is the only path CI cannot run; a production deployment installs
`kubectl` and authenticates a context.

The `list` output is parsed at the **`ProvisionedNode` boundary**
(`ProvisionedNode::from_json`), unwrapping `kubectl get`'s `{ "items": [ … ] }`
envelope and skipping malformed entries — a production binding projects `kubectl get
pods` output into the `{ node_id, address, capability }` shape (pod name + pod IP,
e.g. via `-o jsonpath` / a go-template), and the reference binding consumes that
shape directly.

## What this slice does **not** do

- **No live-cluster CI.** The acceptance criterion ("provision N pods that join the
  cluster and are torn down against a configured context") is realized as the full
  *shape* — the seam, config + schema, lifecycle, identity wiring, and the `kubectl`
  binding — verified against fakes. Running it against a real cluster is a manual /
  deployment step (install + auth `kubectl`).
- **No persistence / replication.** `K8sConfig` and the in-flight burst are
  in-memory; the config's JSON form exists for forward use (configuring /
  replicating a backend).
- **Not yet driven by a live autoscaler.** The
  [autoscaler control loop](../compute/job-autoscaler.md) (#371) and
  [job coordinator](../compute/job-coordinator.md) are built and act through any
  `Provisioner&` (optionally wrapped by the
  [cost-cap approval gate](burst-approval-gate.md)); wiring them to drive *this*
  backend in a live burst is the remaining slice. `K8sProvisioner` drops in
  wherever one holds a `Provisioner&`.

## Tests

`tests/cloud/test_k8s_provisioner.cpp` mirrors the cloud tests — the config boundary, the
lifecycle, and the reference binding:

- **`K8sConfig` JSON boundary** — round-trips a minimal and a full config (the
  `namespace` JSON key ↔ `namespace_name` field); the defaults (`namespace`,
  `deployment_name`, `spot`); and rejects malformed documents (non-object, missing/
  empty `image`, wrong-typed `image` / `namespace` / `spot`).
- **lifecycle (fake control plane)** — provision brings up pods **and mints
  identities** for exactly what came up; best-effort when the pool is short;
  zero-count is a no-op; **no mint** when nothing comes up; teardown **bulk-revokes
  when the pool drains** (once, on the last pod) and an unknown id is an idempotent
  no-op; `provisioned()` reflects an eviction; and it works with no identity hooks
  wired. A dedicated test composes it with a **real `BurstIdentityManager`** (burst →
  mint per pod → teardown → bulk revoke).
- **`K8sCliControlPlane` (fake command runner)** — `bring_up` creates the
  Deployment (`--image` / `--namespace`), scales it (`scale deployment/<name>
  --replicas`), then lists (`get pods --selector app=<name> --output json`); a
  seed/service-account/spot config adds `set env ZETTA_SEED_PEER`
  (+ the `<name>-cluster-token` Secret applied from a private file and `set env
  --from=secret/…` on a secure cluster, with the token in no argv — #670, #770) / `set
  serviceaccount` / a `patch` nodeSelector; best-effort when fewer pods come up; a
  scale failure surfaces in the `detail`; `destroy` builds `delete pod <name>` and
  maps a non-zero exit to `false`; `list` unwraps the `items` envelope, parses, and
  skips malformed entries, returning empty on a command failure; and a pinned
  `context` is passed on every call.

## See also

- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the recipe this
  backend follows (the four pieces + shared infra + the lifecycle→CLI mapping).
- [azure-provisioner.md](azure-provisioner.md) — the **reference** backend.
- [gcp-provisioner.md](gcp-provisioner.md) / [oci-provisioner.md](oci-provisioner.md)
  — the sibling cloud backends.
- [provisioner.md](provisioner.md) — the seam + the given-hardware Local backend.
- [burst-identity.md](burst-identity.md) — the `BurstIdentityManager` the
  mint/revoke hooks bind to.
- [research/kubectl-workloads.md](../research/kubectl-workloads.md) — the cited
  `kubectl` command basis (the Deployment + scale slice the binding uses).
