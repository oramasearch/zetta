# Adding a cloud-burst provisioner (the CLI pattern)

This is the **pattern every cloud-burst backend follows** — the shape
[`AzureProvisioner`](azure-provisioner.md) established as the **reference**
(ARC-13 / ADR-20, `#376`) and that [`AwsProvisioner`](aws-provisioner.md) (`#377`),
[`GcpProvisioner`](gcp-provisioner.md) (`#378`),
[`OciProvisioner`](oci-provisioner.md) (`#432`),
[`K8sProvisioner`](k8s-provisioner.md) (`#434`), and
[`BareMetalProvisioner`](bare-metal-provisioner.md) (`#436`) **mirror**. Read
[azure-provisioner.md](azure-provisioner.md) first; this page is the recipe for the
*next* vendor.

> **We are sticking with the CLI for now.** Each backend drives its target by
> shelling out to the vendor CLI (`az`, `gcloud`, `oci`, `kubectl`, `docker`/`podman`,
> `aws`) — **no cloud SDK / client library** in `zetta-core`, honoring the seam's "no
> cloud SDK behind this header" rule. A REST or SDK binding can replace the CLI later
> **without touching anything above the control-plane seam** (see
> [Swapping the transport](#swapping-the-transport)).

## The four pieces (per vendor)

A cloud backend is four small pieces plus the shared infrastructure below. Name
them `<Vendor>` — `Aws` (built), `Gcp` (built), `Oci` (built), `K8s` (built),
`BareMetal` (built):

1. **`<Vendor>Provisioner : BurstProvisioner<<Vendor>Config, <Vendor>ControlPlane>`**
   — the burst **orchestration** is **shared, not copied**: the
   `BurstProvisioner<Config, ControlPlane>` class template
   ([`burst_provisioner.hpp`](../../include/zetta/cloud/burst_provisioner.hpp)) implements
   `provision` → `control_plane.bring_up` + mint identities; `teardown` →
   `control_plane.destroy` + bulk-revoke when the pool drains; `provisioned` →
   `control_plane.list`, once for every vendor. A `<Vendor>Provisioner` is then just
   `: public BurstProvisioner<<Vendor>Config, <Vendor>ControlPlane>` with
   `using BurstProvisioner::BurstProvisioner;` — it carries the config type and owns
   *no* orchestration or cloud mechanics.
2. **`<Vendor>ControlPlane`** (an injectable seam, like `AzureControlPlane`) —
   `bring_up` / `destroy` / `list`. Lets the provisioner stay in `zetta-core` and
   be unit-tested against a fake. *Vendor-specific in the binding, identical in
   shape.*
3. **`<Vendor>CliControlPlane : <Vendor>ControlPlane`** — the **reference CLI
   binding**. Builds the vendor's CLI argv, runs it through the shared
   `CommandRunner`, and parses the JSON it returns at the **`ProvisionedNode`
   boundary**. *This is the only genuinely vendor-specific code.*
4. **`<Vendor>Config`** — where to burst, how to size/price, how to join; a value
   type with a **validated JSON boundary** mirroring
   `schemas/<vendor>_config.schema.json`. Required: the account/project + region
   + resource scope; everything else optional with defaults.

### Shared infrastructure — reuse, do not re-create

| Reuse | From | Why it is shared |
|---|---|---|
| `Provisioner` seam + `ProvisionedNode` / `ProvisionRequest` / `ProvisionResult` | [`provisioner.hpp`](../../include/zetta/cloud/provisioner.hpp) | the vendor-neutral contract + DTOs (with their `from_json` boundary) |
| `NodeCapability` (cores / memory / GPU) | [`node_capability.hpp`](../../include/zetta/cloud/node_capability.hpp) | the per-node spec a request asks for; map it to a vendor SKU in the binding |
| **`CommandRunner` / `CommandResult` / `default_command_runner`** | [`command_runner.hpp`](../../include/zetta/cloud/command_runner.hpp) | the **shared, vendor-neutral** exec seam every CLI binding runs through (argv, no shell; `fork`/`execvp`; testable with a fake) |
| `IdentityHooks` (mint / revoke) | the provisioner, bound to [`BurstIdentityManager`](burst-identity.md) | bulk burst identity is vendor-independent — reuse the `Principal`/`AccessKey` model, not a per-cloud identity system |
| `BurstApprovalGate` | [`burst-approval-gate.md`](burst-approval-gate.md) | the cost-cap gate is a `Provisioner` **decorator** — it wraps *any* backend unchanged |

So a new vendor backend is, in practice, **one CLI binding + one config + a schema
+ a two-line `Provisioner` subclass + a test**. The orchestration is the shared
`BurstProvisioner` template and the JSON-boundary helpers are
`src/core/json_boundary.hpp`; everything else is already built.

## The lifecycle → CLI mapping

Every cloud exposes the same four operations over a **named, stable burst pool**
(a scale set / auto-scaling group / managed instance group). The stable name is
what makes create/scale/delete **idempotent**.

| Operation | Azure (`az`, built) | GCP (`gcloud`, built `#378`) | OCI (`oci`, built `#432`) | AWS (`aws`, `#377`) |
|---|---|---|---|---|
| **Burst pool primitive** | VM Scale Set (Flexible) | Managed Instance Group (MIG) | Instance Pool (from Instance Configuration) | EC2 Auto Scaling Group |
| **create / scale to N** | `az vmss create … --instance-count N` then `az vmss scale --new-capacity N` | `gcloud compute instance-groups managed create … --size N` then `… resize --size N` | `oci compute-management instance-pool create --instance-configuration-id … --size N` then `… instance-pool update --size N` | `aws autoscaling create-auto-scaling-group …` then `… set-desired-capacity --desired-capacity N` |
| **list instances** | `az vmss list-instances -o json` | `gcloud compute instance-groups managed list-instances <pool> --format json` | `oci compute-management instance-pool list-instances --instance-pool-id <pool>` | `aws ec2 describe-instances --filters tag:aws:autoscaling:groupName=<pool>` |
| **destroy one** | `az vmss delete-instances --instance-ids <id>` | `gcloud compute instance-groups managed delete-instances <pool> --instances <name>` | `oci compute-management instance-pool-instance detach --instance-id <id> --is-decrement-size --is-auto-terminate` | `aws autoscaling terminate-instance-in-auto-scaling-group --instance-id <id> --should-decrement-desired-capacity` |
| **teardown pool** | delete the resource group / scale set | `gcloud compute instance-groups managed delete <pool>` | `oci compute-management instance-pool update --size 0` then `… instance-pool terminate` | `aws autoscaling delete-auto-scaling-group --force-delete` |
| **Spot / preemptible** | `--priority Spot --eviction-policy Delete --max-price` | instance template `--provisioning-model=SPOT` | instance configuration `preemptibleInstanceConfig` (in launchDetails) | Spot via the launch template / `--mixed-instances-policy` |
| **node join (bootstrap)** | `--custom-data` (cloud-init) | instance-template `--metadata startup-script` | instance configuration `metadata.user_data` (cloud-init) | launch-template user-data |
| **per-node identity** | user-assigned **managed identity** (`--assign-identity`) | **service account** (instance template `--service-account`) | **instance principal** (dynamic group) / instance-config metadata | **IAM instance profile** (launch template) |
| **eviction signal** | Scheduled Events metadata | metadata `preempted` flag | instance-metadata preemption notice | EC2 instance-metadata / Spot interruption notice |

**Kubernetes** (`kubectl`, built `#434`) is the same shape over an **existing
cluster** rather than VMs — the burst pool is a **Deployment** of worker pods (the
kubeconfig context is the "account", set up out-of-band). The same four operations:

| Operation | Kubernetes (`kubectl`) |
|---|---|
| **Burst pool primitive** | Deployment (scaled to N replicas) |
| **create / scale to N** | `kubectl create deployment <name> --image <img>` then `kubectl scale deployment/<name> --replicas N` |
| **list instances** | `kubectl get pods --selector app=<name> --output json` |
| **destroy one** | `kubectl delete pod <pod-name>` |
| **teardown pool** | `kubectl scale deployment/<name> --replicas 0` then `kubectl delete deployment <name>` |
| **Spot / preemptible** | `kubectl patch deployment <name> --patch <nodeSelector>` onto a spot node pool |
| **node join (bootstrap)** | `kubectl set env deployment/<name> ZETTA_SEED_PEER=… ZETTA_CLUSTER_TOKEN=…` (token only on a secure cluster) |
| **per-node identity** | **ServiceAccount** (`kubectl set serviceaccount deployment/<name> <sa>`) |
| **eviction signal** | pod eviction / `kubectl get pods` no longer lists it |

**Bare metal** (`docker` / `podman`, built `#436`) is the same shape over the **local
host** rather than a fleet — the burst pool is a set of **local worker containers**
marked by a **label** (there is no scale-set; the binding launches the shortfall and
counts by label). The same four operations:

| Operation | Bare metal (`docker` / `podman`) |
|---|---|
| **Burst pool primitive** | labelled local worker containers |
| **create / scale to N** | `<runtime> ps --filter label=<pool>` to count, then `<runtime> run -d --label <pool> … <image>` for the shortfall |
| **list instances** | `<runtime> ps --filter label=<pool> --format '{{json .}}'` (NDJSON) |
| **destroy one** | `<runtime> rm -f <id>` |
| **teardown pool** | `<runtime> rm -f $(<runtime> ps -q --filter label=<pool>)` |
| **Spot / preemptible** | n/a (local host) — `--gpus all` gives workers the host GPUs instead |
| **node join (bootstrap)** | `--env ZETTA_SEED_PEER=…` (+ `--env ZETTA_CLUSTER_TOKEN=…` on a secure cluster; + `--network host` to reach the cluster) |
| **per-node identity** | the zetta-side burst identity only (no host identity); a `--user` could be added |
| **eviction signal** | the worker container exits / `ps` no longer lists it |

The eviction column is why `provisioned()` reads the control plane live on every
call: a Spot/preemptible reclaim removes a node **without** a `teardown`, and the
list must reflect that (the short-lived burst credential expires on its own — the
TTL is the backstop, the bulk revoke is the clean-shutdown path).

**Secure-cluster join (#670).** The bootstrap seam carries **two** values, not
one: the cluster seed address in `ZETTA_SEED_PEER`, and the cluster join token in
`ZETTA_CLUSTER_TOKEN`, so the node comes up with the equivalent of `zetta --peer
<seed> --join-token <token>` and joins the **mTLS** cluster (inter-node mTLS is
always on — #799 removed the `--cluster-plaintext` opt-out; see
[cluster security](../cluster/security.md)). Both are filled at fleet wire time
from this node's own live cluster security (`ProvisionerFleet::wire` takes a
`default_cluster_token`); the config's `join_token` mirrors its `seed_peer`, with
one deliberate difference — the token is the cluster **secret**, so it is **not**
part of the config's JSON boundary (no schema field, never `to_json`/`from_json`)
and so never lands in the detected provisioner config or `system.provisioners`. A
`Cluster` with empty security (the embedded bare-`Cluster` test/host path) injects
no token. The reference bindings carry the token on the
same visible seam as the seed to illustrate the plumbing; a production binding
sources it from the cloud's secret store instead of a describe-able tag / literal.

### Concrete binding shapes

The reference Azure binding (`src/core/cloud/azure_cli_control_plane.cpp`) is the
template; the built GCP (`gcp_cli_control_plane.cpp`) and OCI
(`oci_cli_control_plane.cpp`) bindings build the analogous argv, and AWS follows.
Sketches (illustrative — real bindings add a launch template / instance template,
networking, and tags):

```text
# GCP bring_up: ensure the MIG exists at N, then list its instances.
gcloud compute instance-groups managed create <pool> --size <N> \
    --template <instance-template> --zone <zone>
gcloud compute instance-groups managed resize <pool> --size <N> --zone <zone>
gcloud compute instance-groups managed list-instances <pool> --zone <zone> --format json

# OCI bring_up: resolve the pool OCID by name, create-or-resize it, then list.
# (OCI is OCID-addressed, so the binding resolves name -> OCID first.)
POOL=$(oci compute-management instance-pool list --compartment-id <c> \
    --display-name <pool> --query 'data[0].id' --raw-output)
# empty -> create path: ensure the instance configuration, then create the pool
oci compute-management instance-configuration create --compartment-id <c> \
    --display-name <pool>-config --instance-details file://launch.json
oci compute-management instance-pool create --compartment-id <c> --display-name <pool> \
    --instance-configuration-id <cfg> --placement-configurations file://placement.json --size <N>
# non-empty -> resize path: oci compute-management instance-pool update --instance-pool-id $POOL --size <N>
oci compute-management instance-pool list-instances --instance-pool-id <pool-ocid>

# Kubernetes bring_up: ensure the Deployment, shape its pods, scale it, then list.
kubectl create deployment <name> --image <img> --namespace <ns>
kubectl set env deployment/<name> ZETTA_SEED_PEER=<seed> ZETTA_CLUSTER_TOKEN=<token> --namespace <ns>
kubectl scale deployment/<name> --replicas <N> --namespace <ns>
kubectl get pods --selector app=<name> --output json --namespace <ns>

# Bare metal bring_up: count labelled workers, run the shortfall, then list.
docker ps --filter label=<pool> --format '{{json .}}'   # current count
docker run -d --label <pool> --name <prefix>-<uuid> --env ZETTA_SEED_PEER=<seed> --env ZETTA_CLUSTER_TOKEN=<token> <img>
docker ps --filter label=<pool> --format '{{json .}}'   # NDJSON, one object per line

# AWS bring_up: ensure the ASG exists at N, then list its instances.
aws autoscaling create-auto-scaling-group --auto-scaling-group-name <pool> \
    --launch-template LaunchTemplateName=<lt> --min-size 0 --max-size <N> \
    --desired-capacity <N> --vpc-zone-identifier <subnets>
aws autoscaling set-desired-capacity --auto-scaling-group-name <pool> --desired-capacity <N>
aws ec2 describe-instances \
    --filters Name=tag:aws:autoscaling:groupName,Values=<pool> Name=instance-state-name,Values=running \
    --query 'Reservations[].Instances[].{node_id:InstanceId,address:PrivateIpAddress}' --output json
```

> **OCI wrinkle — OCID, not name.** Azure (resource group + name) and GCP (project
> + zone + name) address the pool by name in every call. OCI resources are keyed by
> **OCID**, so the binding first resolves the pool's OCID from its stable display
> name (`--query data[0].id --raw-output`), then create-or-resizes. This name→OCID
> resolution is the one structural difference a vendor binding may need; it stays
> entirely inside the `<Vendor>ControlPlane`.

Each binding must **project the CLI's output into the `ProvisionedNode` shape**
(`{ node_id, address, capability }`) before parsing it with
`ProvisionedNode::from_json` — the same way the Azure binding consumes a
`ProvisionedNode`-shaped array. Use the CLI's own projection (`--query` for `az` /
`aws` / `oci`, `--format` for `gcloud` / `docker`, `-o jsonpath` / a go-template for
`kubectl`) so the binding's job is just the parse + skip-malformed at the validated
boundary (the `oci` binding unwraps the `{ "data": [ … ] }` envelope, the `kubectl`
binding the `{ "items": [ … ] }` envelope, and the `docker`/`podman` binding parses
**NDJSON** — one object per line). The `node_id` must be the **zetta cluster node id**
the joined node reports (correlate Azure/GCE/OCI/EC2 instances, K8s pods, or local
containers to
cluster members via a tag / label / instance metadata), not the raw cloud instance
id.

## Step-by-step: add `<Vendor>Provisioner`

1. **Ask for the vendor deep-research prompt first** (the `perplexity` skill) — the
   agreed per-vendor workflow. It surfaces the exact CLI verbs, Spot/preemption
   semantics, identity mechanism, and quota/idempotency footguns. Archive the
   answer under `docs/research/<vendor>-cli-compute.md` with a provenance header
   (as [`docs/research/azure-cli-compute.md`](../research/azure-cli-compute.md) does).
2. **`include/zetta/<vendor>_config.hpp` + `src/core/<vendor>_config.cpp`** (or fold
   the config into the provisioner header, as Azure does) — the value type +
   `to_json` / non-throwing `from_json`. Mirror it with
   **`schemas/<vendor>_config.schema.json`** and keep the two in sync.
3. **`include/zetta/<vendor>_provisioner.hpp` + `.cpp`** — `<Vendor>ControlPlane`
   seam + `<Vendor>Provisioner`. Copy `AzureProvisioner`'s structure; only the
   config type changes. Keep `IdentityHooks` and the "revoke on drain" logic.
4. **`include/zetta/<vendor>_cli_control_plane.hpp` + `.cpp`** — `#include
   "zetta/cloud/command_runner.hpp"` and build the vendor argv. Parse `list` output at
   the `ProvisionedNode` boundary. This is the only file with real vendor specifics.
5. **`tests/test_<vendor>_provisioner.cpp`** — mirror
   `tests/cloud/test_azure_provisioner.cpp`: config JSON boundary (round-trip + malformed
   rejection); the lifecycle against a **fake control plane** (provision+mint,
   best-effort, zero-count no-op, no-mint-when-empty, bulk-revoke on drain,
   idempotent teardown, eviction reflected); and the binding against a **fake
   `CommandRunner`** (argv construction, Spot flags, best-effort, failure detail,
   destroy outcome, list parse + skip-malformed).
6. **Wire the build** — add the two `.cpp` to `zetta-core` in `CMakeLists.txt` and
   the test to `tests/CMakeLists.txt`.
7. **Docs in the same commit** — a `docs/<vendor>-provisioner.md` (mirror
   [azure-provisioner.md](azure-provisioner.md)), linked from `CLAUDE.md`,
   `README.md`, `docs/README.md`, and the cloud-burst bullet in
   [provisioner.md](provisioner.md).

## What CI can and cannot verify

The acceptance criterion ("provision N nodes that join the cluster and are torn
down against a configured account") is realized as the full **shape**, verified
against fakes — CI is CPU-only and macOS-first and cannot reach a live cloud. The
**fake `CommandRunner`** makes the argv construction + JSON parsing — the parts
most likely to drift — fully unit-tested; the only path CI cannot run is the actual
CLI exec. A production deployment installs and authenticates the vendor CLI
(`az login` / `gcloud auth` / `oci setup config` / `kubectl` kubeconfig /
`docker`/`podman` install / `aws configure`) on the host that drives the burst.

## Swapping the transport

The CLI is a deliberate *first* transport, not a permanent one. Because the
`<Vendor>ControlPlane` is a seam, replacing "shell out to the CLI" with "call the
ARM/EC2/Compute REST API directly" (a bearer token + HTTPS, still no heavy SDK) is
a **new `<Vendor>ControlPlane` implementation and nothing else** — `<Vendor>Provisioner`,
the config, the identity wiring, the cost-cap gate, and every test above are
unchanged. That is the whole point of putting the seam there. Until then, **CLI
for every vendor**.

See also [azure-provisioner.md](azure-provisioner.md) (the reference backend),
[provisioner.md](provisioner.md) (the seam + the given-hardware Local backend),
[burst-identity.md](burst-identity.md) (the identity hooks' target), and
[research/azure-cli-compute.md](../research/azure-cli-compute.md) (the cited `az`
command basis).
