# Plan: cloud provisioning & the Add-nodes burst path

> **Status:** living plan (started 2026-06-30). Tracks the effort to make the
> headed **Add nodes** control stand up real nodes on a cloud the operator
> controls and have them **join the equal-peer cluster**. The provisioning half is
> built and landing on PR
> [#608](https://github.com/oramasearch/zetta/pull/608) (branch
> `feat/provisioner-detection-from-cli`); the **cluster-join** half is the next
> slice and the focus of "what's next" below.
>
> This is a *plan*, not a spec: the "how" sections weigh options rather than commit
> to one. It complements [true-hybrid.md](../true-hybrid.md) (the equal-peer,
> heterogeneous-hardware vision the `Provisioner` seam serves), the
> [add-nodes-control.md](../add-nodes-control.md) / [provisioner.md](../provisioner.md) /
> [provisioner-detection.md](../provisioner-detection.md) references, and
> [research/azure-cli-compute.md](../research/azure-cli-compute.md).

## The goal

**One button turns "I want more capacity" into a running, correct cluster member on
infrastructure you control.** Click *Add nodes → Microsoft Azure → Allow*, and a
node comes up in *your* subscription, **joins this cluster as an equal peer**, and
starts sharing work — your data never leaves your environment, the action is
consent-gated + audited, and it is correct by construction (a burst that can't place
falls back or fails loudly, never silently half-works).

That decomposes into two halves:

1. **Provision** — bring up a VM on the target cloud from the operator's own CLI.
   **Built** (see below).
2. **Join** — the VM boots zetta and gossips into this cluster as a `node:<id>`
   member. **Not built** — the critical next slice (see *What's next*).

## Where we are (built, on PR #608)

The provisioning half is done end-to-end and verified against a real Azure
subscription (a `Standard_E2s_v7` placed in `zetta-burst-01` in ~44 s, off the UI
thread). Landed as seven commits (`bcaa8c9`…`19d3523`):

| Piece | What | Where |
|---|---|---|
| **CLI detection** | At startup, ask each installed, logged-in CLI (`az`) for its deployment context (subscription / region / group) — **no credentials stored** — and wire a provisioner per detected target. | `src/core/provisioner_detect.cpp`, `system.zetta_provisioners` |
| **Async provisioning** | The provider CLI (`az vmss create`, minutes) runs **off the UI thread** as a `runtime::spawn`'d coroutine; a progress bar + per-target ETA baseline animates; `poll_add_nodes` applies the result on the UI thread. Failures render in full, **copyable**. | `runtime::spawn` (`src/core/runtime.cpp`), `App::run_add_nodes` / `poll_add_nodes` |
| **Correct Flexible create** | Microsoft-Learn-verified `az vmss create` shape (fault-domain-count, single-placement-group, image + admin + SSH keys). | `src/core/azure_cli_control_plane.cpp`, [research doc §2.7](../research/azure-cli-compute.md) |
| **Fresh resource group** | Each burst picks the first free `zetta-node-group-NN` (a deprovisioning group reads as in-use), so delete + re-add never races. | `pick_resource_group` |
| **SKU fallback** | Azure exposes **no capacity API** — `az vm list-skus` shows only policy restrictions — so on `SkuNotAvailable` the create deletes the partial set and rolls to the next candidate SKU. Retry-with-fallback is the only reliable placement (Microsoft's own guidance). | `sku_candidates` / `bring_up` |
| **Editable placement** | Region + VM-size **dropdowns** of what the subscription can deploy (regions from `az account list-locations`; sizes from `az vm list-skus`, fetched off-thread + cached per region), applied via `Provisioner::set_placement`. | `App::draw_add_nodes_control`, `set_placement` on each cloud backend |

Rules we locked in along the way: **async is coroutines through the `zetta::runtime`
seam, never a hand-rolled thread** (`thread_pool` is for bounded concurrency,
`parallel_for` for CPU fan-out); **CLI-driven + credential-free**; **research-grounded**
(cite Microsoft Learn / the repo research doc, don't guess).

## What's next (the join half — the slice that matters most)

**Today a provisioned Azure VM does not join the cluster**, for two independent
reasons. This is the next real work.

### Gap A — the VM doesn't actually run zetta yet

`bring_up` passes the cluster seed to the VM as `--custom-data
ZETTA_SEED_PEER=<addr>`, but there is **no cloud-init that installs and starts the
zetta binary**. The VM boots a bare Ubuntu image and sits there.

*How we'd like to try it (options, roughly in order of preference):*

- **A pre-baked image / shared-gallery image** that already contains the zetta
  binary + a systemd unit reading `ZETTA_SEED_PEER` from instance metadata. Fastest
  boot, most reproducible; needs an image-build pipeline (a later sub-slice can bake
  it from CI).
- **cloud-init that pulls the binary** (from a release URL / blob) and runs it —
  slower boot, no image pipeline, good for a first working join. This is the likely
  **first** step: prove the join with cloud-init, then optimize with an image.
- Either way, the binary must match the host arch (Azure VMs are x86-64; our dev
  build is arm64 macOS) — so this needs a **Linux x86-64 release artifact**, which
  is its own small piece (CI cross-build / container).

### Gap B — the seed address isn't routable from the cloud

The seed handed to the VM is this node's **listen address** (`cluster.listen_addresses().front()`),
which on a laptop is a private `192.168.x`/`127.0.0.1` — an Azure VM can't route to
it, so even a zetta-running VM couldn't gossip in.

*How we'd like to try it (options):*

- **An operator-provided advertise address** — a `--advertise HOST:PORT` (or a GUI
  field) that is the publicly reachable address peers should dial, defaulting to the
  detected public IP. Simplest; puts the reachability burden on the operator (open a
  port / have a public IP). Good enough for a first end-to-end join.
- **A secure overlay mesh** (WireGuard / Tailscale-style) so every node — laptop,
  cloud VM, on-prem box — joins a private network and dials each other by overlay
  address regardless of NAT. This is almost certainly the **right long-term answer**:
  it matches the product's "your environment, compute-near-data, your data never
  leaves" framing and sidesteps public-IP/port-opening entirely. Bigger lift;
  a candidate for the slice after a basic advertise-address join works.
- **Reverse dial** — since peers are equal and dial each other, the cloud VM could
  advertise *its* public IP and this node dials *it*. Works when the cloud side is
  reachable (usually true) and the laptop isn't — a useful complement to the overlay.

**Sequencing:** the cheapest path to a *first real join* is cloud-init-pulls-binary
(Gap A) + operator advertise-address (Gap B), on a Linux x86-64 release artifact.
Harden afterwards with a baked image + an overlay mesh.

## Other follow-ups (smaller, independent)

- **Status surfacing.** A successful `az vmss create` can still leave 0 instances
  (async allocation failure); today we report placement, not the leaf reason. Parse
  real `az vmss list-instances`, and on a 0-instance outcome surface the
  activity-log reason (`SkuNotAvailable`, quota, …) in-product instead of a bare
  count. Also fix `AzCliControlPlane::list()` to parse Azure's real instance JSON.
- **Other-provider detectors.** Only Azure detects today; AWS / GCP / OCI /
  Kubernetes / bare-metal have the same shape (`whoami`/`config` → their `XConfig`)
  and slot into `detect_provisioner_configs` independently. `set_placement` is
  already wired on all of them.
- **Teardown from the GUI.** The control only grows the cluster; removing a specific
  burst / node (and its resource group) is unbuilt.
- **Persistence.** The ETA baseline and the per-region VM-size cache are
  session-only; persisting them to the data dir would make restarts feel instant.
- **Capacity, better.** Availability zones (`--zones 1 2 3`) broaden placement; the
  Spot Placement Score API gives statistical signal for spot. Both are grounded
  in [research/azure-cli-compute.md] follow-ups and reduce fallback churn.

## Principles (how we build this)

1. **Coroutines through the seam.** Any off-the-UI-thread work is a
   `zetta::runtime::task` via `spawn` — never a raw `std::thread`. (See CLAUDE.md
   "async is coroutines, through the seam".)
2. **CLI-driven, credential-free.** The operator's own CLI holds the credentials;
   zetta reads only non-secret deployment context and drives the CLI to act.
3. **Research-grounded.** Cloud behavior is verified against Microsoft Learn / the
   repo research doc before it ships — no guessing (e.g. "Azure has no capacity API
   → retry-with-fallback").
4. **Correct by construction, consent-gated, audited.** A burst places or fails
   loudly; every cloud write is per-action consent (never remembered) and recorded
   in the audit trail.
5. **Vendor-neutral seam.** New behavior lives behind `Provisioner` /
   `XControlPlane` so a new cloud is a new backend, nothing else.

## Acceptance for "the goal"

A single click on a laptop brings up an Azure VM that **appears in the Nodes panel
as an equal peer within a few minutes**, shares a partition rebalance, and can be
torn down again — with the whole round trip consent-gated, audited, and its data
never leaving the operator's environment.
