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

`include/zetta/cloud_provider.hpp` + `src/core/cloud_provider.cpp`.

A `CloudProvider` is **pure metadata** — identity + presentation plus a
*description* of what an inspect would touch:

| field | meaning |
|-------|---------|
| `id` / `name` / `category` / `summary` | identity + presentation |
| `cli` | the CLI an inspect would use (`aws`, `gcloud`, `az`, `kubectl`, `oci`, `ssh`); empty for Local |
| `config_paths` | the credential/config files an inspect would **read** (read-only) |
| `inspect_commands` | the read-only identity/inventory commands an inspect would **run** (never provision/deploy/teardown) |
| `requires_consent()` | true when an inspect would touch anything outside this node |

**It performs no cloud access of any kind** — no SDK, no CLI, no file read, no
network call. `config_paths` / `inspect_commands` only *name* what a later,
consent-gated real inspect would touch, so the gate can show it up front. Because
there is no untrusted input, there is no JSON schema / boundary to validate
(unlike the [solution catalog](solution-catalog.md)).

`CloudProviderCatalog::builtin()` is a process-wide, immutable catalog ordered
**local → on-prem → Kubernetes → managed clouds**:

| id | category | reads | runs (read-only) |
|----|----------|-------|------------------|
| `local` | Local | — | — (no consent needed; it is this node) |
| `bare-metal` | On-prem | `~/.ssh/config`, `~/.ssh/known_hosts` | `ssh <host> -- uname -a`, … |
| `kubernetes` | Kubernetes | `~/.kube/config` | `kubectl config current-context`, `kubectl cluster-info`, `kubectl get nodes` |
| `aws` | Managed cloud | `~/.aws/config`, `~/.aws/credentials` | `aws sts get-caller-identity`, `aws configure list`, `aws ec2 describe-regions` |
| `gcp` | Managed cloud | `~/.config/gcloud/**` | `gcloud config list`, `gcloud auth list`, `gcloud compute regions list` |
| `azure` | Managed cloud | `~/.azure/config`, `~/.azure/azureProfile.json` | `az account show`, `az account list-locations` |
| `oci` | Managed cloud | `~/.oci/config` | `oci iam region list`, `oci os ns get` |

## The headed panel (`App::draw_cloud`)

`src/gui/app.cpp`, reached from **Operations → Cloud Inspector**.

A list + detail layout (like the Runs / Admin panels):

- **Left** — every provider, each row tagged `[open]` (Local — reads nothing) or
  `[lock]` (everything else — consent required). Selecting a *different* provider
  **resets any prior consent**, so consent never carries across providers.
- **Right** — the selected provider's detail:
  - **Local**: no consent; shows this node's id / data dir / listen addresses
    (held in memory — no cloud access).
  - **Everything else**: "Inspecting this would read" (the `config_paths`) and
    "and run (read-only)" (the `inspect_commands`), then the **consent gate**:
    - *Locked* (deny-by-default): a button **"Allow inspection of `<name>`"**.
      Until pressed, nothing is read or run.
    - *Allowed*: a clearly-labelled placeholder — **no real read happens in this
      slice** — plus a **"Revoke permission"** button.

## Scope of this slice

This is **scaffold + consent gate only**. The panel renders the catalog metadata
and the gate; it **does not** read any config file or call any cloud, even after
consent. Wiring the gate to the real read-only inspect — actually invoking the
existing CLI control planes (`aws sts get-caller-identity`, `kubectl
cluster-info`, …) behind this same consent step, and surfacing identity / regions
/ clusters — is the **next slice**. Writes (provision / deploy / teardown) stay
out of the inspector entirely.

## Tests

`tests/test_cloud_provider.cpp` — the catalog covers every backend, `find`
returns null for unknown ids, **Local reads nothing and needs no consent**, every
non-Local provider `requires_consent()` and names its files + commands + CLI, AWS
names its credential files (and its commands are read-only), Kubernetes names its
kubeconfig, and provider ids are unique.

## Related

- [provisioner.md](provisioner.md) — the `Provisioner` seam the inspector fronts.
- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the per-vendor
  burst-backend recipe.
- [solution-catalog.md](solution-catalog.md) — the sibling **Deploy panel** (pick
  a blueprint, gate, deploy).
