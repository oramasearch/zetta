# Cloud Inspector (permission-gated)

The **Cloud Inspector** is the headed GUI surface for the deployment-target
backends — local, on-prem bare-metal, Kubernetes, and the managed clouds (AWS,
GCP, Azure, OCI). It is where a user **inspects a cloud** before deploying onto
it. It is the GUI counterpart to the `Provisioner` + CLI-control-plane layer
(see [provisioner.md](provisioner.md), [azure-provisioner.md](azure-provisioner.md),
and the [cloud-provisioner pattern](cloud-provisioner-pattern.md)).

## The hard rule it enforces

Touching a cloud provider is **dangerous to do casually**: inspecting reads
credential/config files (`~/.aws/credentials`, `~/.kube/config`,
`~/.config/gcloud/**`, …) and can call real cloud APIs. So the inspector is
built around a single non-negotiable rule (see CLAUDE.md → *Cloud providers —
permission-gated*):

> **Never read a cloud provider's config/credentials, and never read from or
> write to a cloud provider, without explicit user permission — every time,
> deny by default.**

In the UI this is a **load-bearing consent gate**: nothing is read or called
until the user explicitly allows that one inspection, and consent is **never
remembered** across providers or selections.

## The provider catalog (`zetta::CloudProviderCatalog`)

`include/zetta/cloud/cloud_provider.hpp` + `src/core/cloud/cloud_provider.cpp`.

A `CloudProvider` is **pure metadata** — identity + presentation plus a
*description* of what an inspect would touch:

| field | meaning |
|-------|---------|
| `id` / `name` / `category` / `summary` | identity + presentation |
| `cli` | the CLI an inspect would use (`aws`, `gcloud`, `az`, `kubectl`, `oci`, `ssh`); empty for Local |
| `config_paths` | the credential/config files an inspect would **read** (read-only) |
| `inspect_commands` | the read-only identity/inventory commands an inspect would **run** (never provision/deploy/teardown) |
| `requires_consent()` | true when an inspect would touch anything outside this node |

**The catalog itself performs no cloud access of any kind** — no SDK, no CLI, no
file read, no network call. `config_paths` / `inspect_commands` only *name* what a
consent-gated real inspect would touch, so the gate can show it up front. The
actual reads are done by the [`CloudInspector`](#the-inspector-zettacloudinspector)
below, and only behind explicit consent. Because the catalog has no untrusted
input, there is no JSON schema / boundary to validate on it (unlike the
[solution catalog](../pipeline/solution-catalog.md)) — the boundary lives on the inspector's
*output* instead.

Each `inspect_commands` entry is an **`InspectCommand`** — a `display` string
(shown verbatim in the gate) plus the `argv` that actually runs it (program +
arguments, **no shell**). `argv` is empty when the command can't run as-is
(bare-metal's `ssh <host> …` needs a concrete host), so `requires_consent()` is
true but `runnable()` is false; the CLI providers carry real argv and are
runnable.

`CloudProviderCatalog::builtin()` is a process-wide, immutable catalog ordered
**local → on-prem → Kubernetes → managed clouds**:

| id | category | the CLI may read | zetta runs (read-only) | runnable |
|----|----------|------------------|------------------------|----------|
| `local` | Local | — | — (no consent needed; it is this node) | n/a |
| `bare-metal` | On-prem | `~/.ssh/config`, `~/.ssh/known_hosts` | `ssh <host> -- uname -a`, … | no (needs a host) |
| `kubernetes` | Kubernetes | `~/.kube/config` | `kubectl config view --minify -o json`, `kubectl get nodes -o json` | yes |
| `aws` | Managed cloud | `~/.aws/config`, `~/.aws/credentials` | `aws sts get-caller-identity --output json`, `aws ec2 describe-regions --output json` | yes |
| `gcp` | Managed cloud | `~/.config/gcloud/**` | `gcloud config list --format=json`, `gcloud compute regions list --format=json` | yes |
| `azure` | Managed cloud | `~/.azure/config`, `~/.azure/azureProfile.json` | `az account show --output json`, `az account list-locations --output json` | yes |
| `oci` | Managed cloud | `~/.oci/config` | `oci iam region list --output json`, `oci os ns get --output json` | yes |

The commands are JSON-emitting on purpose: the inspector parses each one's output
at a [validated boundary](#the-inspector-zettacloudinspector). zetta runs these
commands but **never reads or prints the `config_paths` files** — those are named
in the gate only so you see what the CLI itself may touch.

## The headed panel (`App::draw_cloud`)

`src/gui/app.cpp`, reached from **Operations → Cloud inspector**. Built in the
[design-system](../gui/design-system.md) grammar.

A list + detail layout (like the Runs / Admin panels):

- **Left** — providers grouped under mono category eyebrows (`LOCAL` / `ON-PREM`
  / `KUBERNETES` / `MANAGED CLOUD`), each row led by a status dot: **green** =
  open (Local, reads nothing), **amber** = locked behind consent. Selecting a
  *different* provider **resets any prior consent**, so consent never carries
  across providers.
- **Right** — the selected provider's detail:
  - **Local**: no consent; shows this node's id / data dir / listen addresses
    (held in memory, no cloud access).
  - **Everything else**: the `config_paths` and read-only `inspect_commands`
    rendered in mono (they are code), then the **consent gate** as a tinted,
    bordered callout box:
    - *Locked* (amber wash, deny-by-default): a **"Allow inspection of `<name>`"**
      button. Until pressed, nothing is read or run.
    - *Granted* (green wash): the **real inspect result** —
      connected/reachable/not-reachable with the identity, the regions, any nodes,
      and a green/red dot per command (with the error when one failed, e.g. a
      missing CLI) — plus **"Re-inspect"** and **"Revoke permission"** buttons.
      The inspect runs only on Allow / Re-inspect, never on a passive render.

`App::run_cloud_inspect()` is the single place the GUI touches a cloud. It runs
synchronously on the UI thread (which owns the node, so caching the result races
with nothing) and records the report via `Node::record_cloud_inspection`.

## The inspector (`zetta::CloudInspector`)

`include/zetta/cloud/cloud_inspector.hpp` + `src/core/cloud/cloud_inspector.cpp`.

`CloudInspector::inspect(provider)` is **the real cloud read** — but it performs
**no consent itself**: the caller (the GUI gate) must obtain explicit, per-action,
deny-by-default permission *before* calling it. It runs every command through the
shared `CommandRunner` seam (`include/zetta/cloud/command_runner.hpp` — argv, no shell;
the same exec seam the [CLI control planes](cloud-provisioner-pattern.md) use), so
it is testable with a fake runner (no cloud CLI installed) and free of
shell-injection risk. It returns a `CloudInspectReport`:

| field | meaning |
|-------|---------|
| `ran` | an inspect actually executed (a `runnable()` provider) |
| `reachable` | at least one command exited 0 |
| `authenticated` | the identity command succeeded and yielded an identity |
| `identity` | account / arn / user / project / namespace (provider-specific) |
| `locations` | regions / locations |
| `resources` | k8s nodes, … |
| `commands[]` | per-command `display` / `exit_code` / `ok` / `output` / `error` |
| `detail` / `checked_at` | an overall note + an ISO-8601 stamp |

It carries **no credentials** — it runs the read-only commands but never reads or
stores the config/credential files.

**Boundary validation.** Each vendor command's JSON output is parsed
non-throwing and validated against an embedded draft-07 schema (`$defs` in
`schemas/cloud_inspect_report.schema.json`) **before any field is read**, so a
missing CLI, a non-zero exit, or malformed/unexpected JSON degrades gracefully
(the failure is recorded on that command; the inspect never throws). The
`CloudInspectReport` is zetta's own contract and is itself boundary-validated
(`validate_cloud_inspect_report` / `parse_cloud_inspect_report`) — it is the value
cached on the node and surfaced as SQL.

## SQL surface (`system` tables)

Per the [house rule](../../CLAUDE.md) *surface observable state as a `system.zetta_*`
table by default*, the inspector is queryable through the
[query layer](../data/query.md):

- **`system.zetta_cloud_providers`** — the provider catalog as a table
  (`provider` / `name` / `category` / `cli` / `requires_consent` / `runnable` /
  `config_paths` / `inspect_commands`). **Pure metadata — reads nothing from any
  cloud.**
- **`system.zetta_cloud_status`** — the cached results of consent-gated inspects
  (`Node::cloud_inspections()`: `provider` / `name` / `reachable` /
  `authenticated` / `identity` / `regions` / `resources` / `commands_ok` /
  `commands_total` / `checked_at` / `detail`). **Querying it never triggers a
  cloud call** — it only reflects inspects you already ran (and consented to) in
  the panel, so it is empty until then. The cache is **ephemeral**: in-memory,
  not replicated, not persisted (a live inspect is session-local).

## Scope of this slice

The inspector now does the **real read-only inspect** behind the consent gate (the
follow-up to the original scaffold). Out of scope, by design: **writes** (provision
/ deploy / teardown) stay out of the inspector entirely; bare-metal/on-prem is
described but **not runnable** until a host is configured; and the inspect runs on
the UI thread (a brief block on a user-initiated click) rather than off-thread.

## Tests

- `tests/cloud/test_cloud_provider.cpp` — the catalog covers every backend, `find`
  returns null for unknown ids, **Local reads nothing and needs no consent**,
  every non-Local provider `requires_consent()` and names its files + commands +
  CLI, the CLI providers are `runnable()` while **bare-metal is not**, AWS names
  its credential files (and its commands + argv are read-only), Kubernetes names
  its kubeconfig, and provider ids are unique.
- `tests/cloud/test_cloud_inspector.cpp` — with a **fake `CommandRunner`** (no cloud
  touched): AWS/GCP/Azure/OCI/Kubernetes success paths extract identity +
  regions/nodes; a missing CLI is reported not-reachable; malformed / wrong-shape
  JSON degrades gracefully; bare-metal runs nothing (`ran = false`); and the
  report round-trips through its JSON boundary (a malformed report fails
  validation).
- `tests/data/test_query.cpp` — `system.zetta_cloud_providers` covers every backend
  with WHERE/projection; `system.zetta_cloud_status` is empty until an inspect is
  recorded, then returns it.

## Related

- [provisioner.md](provisioner.md) — the `Provisioner` seam the inspector fronts.
- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the per-vendor
  burst-backend recipe.
- [solution-catalog.md](../pipeline/solution-catalog.md) — the sibling **Deploy panel** (pick
  a blueprint, gate, deploy).
