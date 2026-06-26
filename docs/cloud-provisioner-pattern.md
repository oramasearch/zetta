# Adding a cloud-burst provisioner (the CLI pattern)

This is the **pattern every cloud-burst backend follows** — the shape
[`AzureProvisioner`](azure-provisioner.md) established as the **reference**
(ARC-13 / ADR-20, `#376`) and that `AwsProvisioner` (`#377`) and `GcpProvisioner`
(`#378`) **mirror**. Read [azure-provisioner.md](azure-provisioner.md) first; this
page is the recipe for the *next* vendor.

> **We are sticking with the CLI for now.** Each backend drives its cloud by
> shelling out to the vendor CLI (`az`, `aws`, `gcloud`) — **no cloud SDK** in
> `zetta-core`, honoring the seam's "no cloud SDK behind this header" rule. A REST
> or SDK binding can replace the CLI later **without touching anything above the
> control-plane seam** (see [Swapping the transport](#swapping-the-transport)).

## The four pieces (per vendor)

A cloud backend is four small pieces plus the shared infrastructure below. Name
them `<Vendor>` — `Aws`, `Gcp`:

1. **`<Vendor>Provisioner : Provisioner`** — the burst **orchestration**, copied
   structurally from `AzureProvisioner`: `provision` → `control_plane.bring_up` +
   mint identities; `teardown` → `control_plane.destroy` + bulk-revoke when the
   pool drains; `provisioned` → `control_plane.list`. **This class is almost
   identical across vendors** — only the config type it carries differs. It owns
   *no* cloud mechanics.
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
| `Provisioner` seam + `ProvisionedNode` / `ProvisionRequest` / `ProvisionResult` | [`provisioner.hpp`](../include/zetta/provisioner.hpp) | the vendor-neutral contract + DTOs (with their `from_json` boundary) |
| `NodeCapability` (cores / memory / GPU) | [`node_capability.hpp`](../include/zetta/node_capability.hpp) | the per-node spec a request asks for; map it to a vendor SKU in the binding |
| **`CommandRunner` / `CommandResult` / `default_command_runner`** | [`command_runner.hpp`](../include/zetta/command_runner.hpp) | the **shared, vendor-neutral** exec seam every CLI binding runs through (argv, no shell; `fork`/`execvp`; testable with a fake) |
| `IdentityHooks` (mint / revoke) | the provisioner, bound to [`BurstIdentityManager`](burst-identity.md) | bulk burst identity is vendor-independent — reuse the `Principal`/`AccessKey` model, not a per-cloud identity system |
| `BurstApprovalGate` | [`burst-approval-gate.md`](burst-approval-gate.md) | the cost-cap gate is a `Provisioner` **decorator** — it wraps *any* backend unchanged |

So a new vendor backend is, in practice, **one CLI binding + one config + a schema
+ a near-copy of the provisioner + a test**. Everything else is already built.

## The lifecycle → CLI mapping

Every cloud exposes the same four operations over a **named, stable burst pool**
(a scale set / auto-scaling group / managed instance group). The stable name is
what makes create/scale/delete **idempotent**.

| Operation | Azure (`az`, built) | AWS (`aws`, `#377`) | GCP (`gcloud`, `#378`) |
|---|---|---|---|
| **Burst pool primitive** | VM Scale Set (Flexible) | EC2 Auto Scaling Group | Managed Instance Group (MIG) |
| **create / scale to N** | `az vmss create … --orchestration-mode Flexible --instance-count N` then `az vmss scale --new-capacity N` | `aws autoscaling create-auto-scaling-group …` then `aws autoscaling set-desired-capacity --desired-capacity N` | `gcloud compute instance-groups managed create … --size N` then `… resize --size N` |
| **list instances** | `az vmss list-instances -o json` | `aws ec2 describe-instances --filters tag:aws:autoscaling:groupName=<pool>` (or `aws autoscaling describe-auto-scaling-instances`) | `gcloud compute instance-groups managed list-instances <pool> --format json` |
| **destroy one** | `az vmss delete-instances --instance-ids <id>` | `aws autoscaling terminate-instance-in-auto-scaling-group --instance-id <id> --should-decrement-desired-capacity` | `gcloud compute instance-groups managed delete-instances <pool> --instances <name>` |
| **teardown pool** | delete the resource group / scale set | `aws autoscaling delete-auto-scaling-group --force-delete` | `gcloud compute instance-groups managed delete <pool>` |
| **Spot / preemptible** | `--priority Spot --eviction-policy Delete --max-price` | Spot via the launch template / `--mixed-instances-policy` (or `aws ec2 create-fleet`) | instance template `--provisioning-model=SPOT` |
| **node join (bootstrap)** | `--custom-data` (cloud-init) | launch-template user-data | instance-template `--metadata startup-script` |
| **per-node identity** | user-assigned **managed identity** (`--assign-identity`) | **IAM instance profile** (launch template) | **service account** (instance template `--service-account`) |
| **eviction signal** | Scheduled Events metadata | EC2 instance-metadata / Spot interruption notice | metadata `preempted` flag |

The eviction column is why `provisioned()` reads the control plane live on every
call: a Spot/preemptible reclaim removes a node **without** a `teardown`, and the
list must reflect that (the short-lived burst credential expires on its own — the
TTL is the backstop, the bulk revoke is the clean-shutdown path).

### Concrete binding shapes

The reference Azure binding (`src/core/azure_cli_control_plane.cpp`) is the
template. The AWS and GCP bindings build the analogous argv. Sketches (illustrative
— real bindings add a launch template / instance template, networking, and tags):

```text
# AWS bring_up: ensure the ASG exists at N, then list its instances.
aws autoscaling create-auto-scaling-group --auto-scaling-group-name <pool> \
    --launch-template LaunchTemplateName=<lt> --min-size 0 --max-size <N> \
    --desired-capacity <N> --vpc-zone-identifier <subnets>
aws autoscaling set-desired-capacity --auto-scaling-group-name <pool> --desired-capacity <N>
aws ec2 describe-instances \
    --filters Name=tag:aws:autoscaling:groupName,Values=<pool> Name=instance-state-name,Values=running \
    --query 'Reservations[].Instances[].{node_id:InstanceId,address:PrivateIpAddress}' --output json

# GCP bring_up: ensure the MIG exists at N, then list its instances.
gcloud compute instance-groups managed create <pool> --size <N> \
    --template <instance-template> --zone <zone>
gcloud compute instance-groups managed resize <pool> --size <N> --zone <zone>
gcloud compute instance-groups managed list-instances <pool> --zone <zone> --format json
```

Each binding must **project the CLI's output into the `ProvisionedNode` shape**
(`{ node_id, address, capability }`) before parsing it with
`ProvisionedNode::from_json` — the same way the Azure binding consumes a
`ProvisionedNode`-shaped array. Use the CLI's own projection (`--query` for `aws`,
`--format` for `gcloud`, `--query` for `az`) so the binding's job is just the parse
+ skip-malformed at the validated boundary. The `node_id` must be the **zetta
cluster node id** the joined node reports (correlate Azure/EC2/GCE instances to
cluster members via a tag / label / instance metadata), not the raw cloud instance
id.

## Step-by-step: add `<Vendor>Provisioner`

1. **Ask for the vendor deep-research prompt first** (the `perplexity` skill) — the
   agreed per-vendor workflow. It surfaces the exact CLI verbs, Spot/preemption
   semantics, identity mechanism, and quota/idempotency footguns. Archive the
   answer under `docs/research/<vendor>-cli-compute.md` with a provenance header
   (as [`docs/research/azure-cli-compute.md`](research/azure-cli-compute.md) does).
2. **`include/zetta/<vendor>_config.hpp` + `src/core/<vendor>_config.cpp`** (or fold
   the config into the provisioner header, as Azure does) — the value type +
   `to_json` / non-throwing `from_json`. Mirror it with
   **`schemas/<vendor>_config.schema.json`** and keep the two in sync.
3. **`include/zetta/<vendor>_provisioner.hpp` + `.cpp`** — `<Vendor>ControlPlane`
   seam + `<Vendor>Provisioner`. Copy `AzureProvisioner`'s structure; only the
   config type changes. Keep `IdentityHooks` and the "revoke on drain" logic.
4. **`include/zetta/<vendor>_cli_control_plane.hpp` + `.cpp`** — `#include
   "zetta/command_runner.hpp"` and build the vendor argv. Parse `list` output at
   the `ProvisionedNode` boundary. This is the only file with real vendor specifics.
5. **`tests/test_<vendor>_provisioner.cpp`** — mirror
   `tests/test_azure_provisioner.cpp`: config JSON boundary (round-trip + malformed
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
(`az login` / `aws configure` / `gcloud auth`) on the host that drives the burst.

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
[research/azure-cli-compute.md](research/azure-cli-compute.md) (the cited `az`
command basis).
