# AWS provisioner (cloud burst)

The **`AwsProvisioner`** (`include/zetta/aws_provisioner.hpp`) is the AWS
cloud-burst backend behind the [`Provisioner`](provisioner.md) seam — the AWS
sibling of the **reference** [`AzureProvisioner`](azure-provisioner.md) (`#376`)
and a peer of the given-hardware
[`LocalProvisioner`](provisioner.md#the-local-backend-localprovisioner) (`#374`).
It realizes **ARC-13 / ADR-20** in the native distributed accelerated-compute pass
for UC-1 churn (`#264`, `#377`).

It **follows the [cloud-provisioner pattern](cloud-provisioner-pattern.md)** that
Azure established: the lifecycle is the same on every cloud — **burst** N
ephemeral nodes → **mint** bulk identities so they can join the cluster → run
cluster-parallel → **tear down** + **revoke** those identities. Only the config
type and the CLI binding differ from Azure; GCP (`#378`) will mirror the same
shape.

## How it fits

```
                       Provisioner (vendor-neutral seam)
                                   ▲
                                   │ implements
                          AwsProvisioner ────────────────► IdentityHooks (optional)
                          burst lifecycle + identity        mint on provision /
                                   │ delegates AWS work      revoke on teardown
                                   ▼                         (→ BurstIdentityManager)
                          AwsControlPlane (seam)
                                   ▲
                                   │ implements
                          AwsCliControlPlane ─────────────► CommandRunner
                          shells out to the `aws` CLI        exec argv, capture JSON
```

`AwsProvisioner` owns the **burst orchestration** (provision → mint → teardown →
revoke) but **none of the AWS mechanics**. It is structurally a near-copy of
`AzureProvisioner` — only the config type it carries differs — and delegates the
cloud work to an injected `AwsControlPlane`, so it stays in `zetta-core` with **no
cloud SDK** and is fully unit-testable against a fake. The reference
`AwsControlPlane` is `AwsCliControlPlane`, which shells out to the `aws` CLI (no
AWS C++ SDK) — a REST or SDK binding could replace it without touching the
provisioner.

> **"No cloud SDK lives behind the seam."** The same rule the `Provisioner` header
> states and the same isolation SDL/ImGui and CUDA get. The `aws` binding is plain
> subprocess + JSON parsing — no AWS headers, no new build dependency.

## The burst lifecycle

| Operation | Behaviour |
|---|---|
| `provision(request)` | Calls `control_plane.bring_up(config, request)` to scale the burst pool to `request.count` (best-effort — Spot capacity / quota may yield fewer). When any node comes up and a mint hook is wired, **mints bulk identities** for the returned nodes so they can authenticate and join. `count` 0 is a satisfied no-op that touches neither the control plane nor the hooks. |
| `teardown(node_id)` | Calls `control_plane.destroy(config, node_id)` (idempotent — unknown id → `false`). When a successful teardown drains the pool to **empty**, **bulk-revokes** the job's identities (the revoke hook). |
| `provisioned()` | Delegates to `control_plane.list(config)`, so it reflects **live capacity** — a Spot interruption disappears without a teardown call, exactly as `LocalProvisioner` reflects self-healing membership. |

The control plane is the **source of truth** for what is up: `provisioned()` and
the "did this teardown drain the pool?" check both read it, so the provisioner
keeps no parallel copy of fleet state (the
[everything-is-derived rule](replication.md)).

### Bulk identity — minted on provision, revoked on full teardown

The issue's "join with bulk identity" is wired through two optional
**`IdentityHooks`** callables, not a hard dependency on
[`BurstIdentityManager`](burst-identity.md) — identical to the reference Azure
backend (the wiring is vendor-independent):

```cpp
struct IdentityHooks {
    std::function<void(const std::vector<ProvisionedNode>&)> mint;   // on provision
    std::function<void()> revoke;                                    // on full teardown
};
```

Production binds them to a `BurstIdentityManager` for the job:

```cpp
BurstIdentityManager identities{node};
AwsProvisioner::IdentityHooks hooks;
hooks.mint = [&](const std::vector<ProvisionedNode>& nodes) {
    identities.mint(job_id, nodes, Scope{/* the job's project */}, ttl_caps);
};
hooks.revoke = [&] { identities.revoke(job_id); };

AwsCliControlPlane aws_cli;                      // the reference `aws` binding
AwsProvisioner provisioner{aws_cli, config, hooks};
```

`BurstIdentityManager::mint` is **idempotent + additive** and `revoke(job_id)` is
**bulk**, so this maps cleanly onto the per-node seam: mint for whatever a burst
brought up, and revoke the whole job once the last node is torn down. Because the
burst credentials are **short-lived** (TTL'd capabilities, see
[access-control.md](access-control.md)), a node lost to a Spot interruption —
which never calls `teardown` — has its credential expire on its own; bulk revoke
is the clean-shutdown path, the TTL is the backstop.

## Configuration (`AwsConfig`)

`AwsConfig` is where on AWS to burst, how to size and price the nodes, and how each
joins the cluster. Only the region and the resource scope needed to create the
pool are required; everything else has a sensible default.

| Field | Type | Required | Meaning |
|---|---|:--:|---|
| `region` | `string` | ✓ | the AWS region the nodes burst into (e.g. `us-west-2`) |
| `launch_template` | `string` | ✓ | the EC2 launch template name — defines the AMI, default instance type, **IAM instance profile** (the per-node AWS identity), and the user-data (the cluster-join bootstrap) |
| `subnets` | `string` | ✓ | comma-separated VPC subnet ids (the ASG's `--vpc-zone-identifier`) |
| `pool_name` | `string` | | Auto Scaling Group name; stable → idempotent create/scale/delete (default `zetta-burst`) |
| `instance_type` | `string` | | an instance-type override applied via the ASG's mixed-instances policy; empty → the launch template's type |
| `spot` | `bool` | | burst Spot capacity (cheaper, interruptible) via the mixed-instances policy (default `false`) |
| `max_price` | `number` | | Spot max $/node-hour; **0 = up to On-Demand** (no explicit cap), AWS's convention (default `0`) |
| `seed_peer` | `string` | | cluster seed `host:port` handed to each node's bootstrap so it auto-joins |
| `profile` | `string` | | optional AWS CLI named profile (which credentials the `aws` commands run under) |

> The **per-node AWS identity** is the IAM instance profile carried in the launch
> template (the AWS analog of Azure's user-assigned managed identity) — distinct
> from the zetta burst `Principal`/`AccessKey` the `IdentityHooks` mint. `max_price`
> uses AWS's convention (`0` = capped at On-Demand), where Azure uses `-1`.

### JSON boundary + schema

`AwsConfig` has a **validated JSON boundary** (`to_json` / non-throwing
`from_json`), the same pattern as [`NodeCapability`](provisioner.md),
[`BurstCap`](burst-approval-gate.md), and [`AzureConfig`](azure-provisioner.md):
`from_json` returns `std::nullopt` on anything malformed — not an object, a
missing/empty required field, a wrong-typed field, or a **negative** `max_price` —
so a misconfigured burst fails fast instead of bringing up the wrong (or
unbounded) fleet. The shape is documented by
[`schemas/aws_config.schema.json`](../schemas/aws_config.schema.json), which the
`from_json` checks **mirror** — keep the two in sync. Optional fields are emitted
by `to_json` only when set, so a round-trip is stable and a minimal config reads
cleanly.

## The reference `aws` binding (`AwsCliControlPlane`)

`AwsCliControlPlane` (`include/zetta/aws_cli_control_plane.hpp`) is the reference
`AwsControlPlane`. It borrows the **EC2 Auto Scaling Group + Spot shape** (not the
SDK): a stable, named ASG *is* the burst pool, scaling its desired capacity up is
the burst, terminating instances is teardown — the AWS analog of Azure's VM Scale
Set. It builds `aws` argv, runs them through an injected **`CommandRunner`**, and
parses the JSON the CLI returns.

| `AwsControlPlane` call | `aws` command (shape) |
|---|---|
| `bring_up` | `aws autoscaling create-auto-scaling-group --auto-scaling-group-name <pool> --launch-template LaunchTemplateName=<lt> --min-size 0 --max-size N --desired-capacity N --vpc-zone-identifier <subnets> [--mixed-instances-policy …(Spot / instance-type)… --tags …seed…]`, falling back to `aws autoscaling set-desired-capacity --desired-capacity N` when the group already exists, then `list` |
| `destroy` | `aws autoscaling terminate-instance-in-auto-scaling-group --instance-id <node_id> --should-decrement-desired-capacity` |
| `list` | `aws ec2 describe-instances --filters Name=tag:aws:autoscaling:groupName,Values=<pool> Name=instance-state-name,Values=running --query … --output json` |

`bring_up` creates the ASG at the requested desired capacity, then **lists** what
is up and reports best-effort against the requested count. The stable `pool_name`
makes a re-burst **idempotent**: a second `create-auto-scaling-group` fails because
the group exists, so the binding falls back to `set-desired-capacity` to scale the
existing group (a real failure — bad subnet, quota — surfaces in the result
`detail`). Spot capacity and an instance-type override ride on the ASG's
`--mixed-instances-policy` (the AWS way to ask an ASG for Spot); the seed peer is
attached as a propagating tag the bootstrap reads (a production deployment bakes
the join into the launch template's user-data). The command shapes are grounded in
[research/aws-cli-compute.md](research/aws-cli-compute.md) (the cited `aws` guide).

### The command runner — testable without `aws`

```cpp
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
```

`CommandRunner` is **shared, vendor-neutral** infrastructure
(`include/zetta/command_runner.hpp`) — the same exec seam the reference Azure
binding runs through, and the one GCP (`#378`) will use (see
[cloud-provisioner-pattern.md](cloud-provisioner-pattern.md)). The runner takes
**argv** (program + arguments, no shell — so no shell-injection risk) and returns
`{ exit_code, stdout_text, stderr_text }`. The default
(`default_command_runner()`) `fork`/`execvp`s the program and captures its merged
output; a test injects a fake that asserts on the argv and returns canned JSON.
That makes the **command construction + JSON parsing** — the parts most likely to
drift — fully unit-tested. The actual `aws` exec is the only path CI cannot run; a
production deployment installs and authenticates the CLI (`aws configure` / SSO /
an instance role).

The `list` output is parsed at the **`ProvisionedNode` boundary**
(`ProvisionedNode::from_json`), skipping malformed entries. The reference query
projects each instance's id + private address; a production binding additionally
**joins the capability** the node reports as instance tags into the
`{ node_id, address, capability }` shape (the `node_id` must be the zetta cluster
node id the joined node reports, correlated via a tag — not the raw EC2 instance
id), and the reference binding consumes that shape directly.

## What this slice does **not** do

- **No live-cloud CI.** The acceptance criterion ("provision N nodes that join the
  cluster and are torn down against a configured AWS account") is realized as the
  full *shape* — the seam, config + schema, lifecycle, identity wiring, and the
  `aws` binding — verified against fakes. Running it against a real account is a
  manual / deployment step (install + auth `aws`).
- **No persistence / replication.** `AwsConfig` and the in-flight burst are
  in-memory; the config's JSON form exists for forward use (configuring /
  replicating a backend).
- **No autoscaler wiring.** The autoscaler / job coordinator that *drives*
  `provision`/`teardown` (and the [cost-cap approval gate](burst-approval-gate.md)
  that wraps it) is a separate, later slice. `AwsProvisioner` drops in wherever one
  holds a `Provisioner&`.

## Tests

`tests/test_aws_provisioner.cpp` mirrors `tests/test_azure_provisioner.cpp` —
the config boundary, the lifecycle, and the reference binding:

- **`AwsConfig` JSON boundary** — round-trips a minimal and a full config; the
  defaults (`pool_name`, `spot`, `max_price = 0`); and rejects malformed documents
  (non-object, each missing/empty required field, wrong-typed `spot` / `max_price`,
  and a negative `max_price`).
- **lifecycle (fake control plane)** — provision brings up nodes **and mints
  identities** for exactly what came up; best-effort when the pool is short; a
  zero-count no-op that touches neither the control plane nor the hooks; **no
  mint** when nothing comes up; teardown **bulk-revokes when the pool drains**
  (once, on the last node) and an unknown id is an idempotent no-op that does not
  revoke; `provisioned()` reflects a Spot interruption; and it works with no
  identity hooks wired.
- **`AwsCliControlPlane` (fake command runner)** — `bring_up` builds the
  `create-auto-scaling-group` argv (group name / launch template / sizing /
  subnets / region) then lists; a Spot config adds the `--mixed-instances-policy`
  (with `SpotMaxPrice` + the instance-type override) and the seed tag; it falls
  back to `set-desired-capacity` when the group already exists; best-effort when
  fewer instances come up; a create+scale failure surfaces in the `detail`;
  `destroy` builds `terminate-instance-in-auto-scaling-group`
  (`--should-decrement-desired-capacity`) and maps a non-zero exit to `false`;
  `list` parses the instances and skips malformed entries, and returns empty on a
  command failure.

See also [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) (the recipe
every cloud backend follows), [azure-provisioner.md](azure-provisioner.md) (the
reference backend), [provisioner.md](provisioner.md) (the seam + the Local
backend), [burst-identity.md](burst-identity.md) (the `BurstIdentityManager` the
mint/revoke hooks bind to), and [burst-approval-gate.md](burst-approval-gate.md)
(the cost-cap decorator that can wrap any backend).
