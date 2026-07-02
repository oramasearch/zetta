# Add nodes control (the headed Cluster view)

The headed **Cluster** panel's **Add nodes** control (`App::draw_add_nodes_control`,
`src/gui/app_operations.cpp`, **#583** — epic [#575](https://github.com/oramasearch/zetta/issues/575)
area 1) is the GUI front door for **[`zetta::add_nodes`](../cloud/add-nodes.md)** — the
"add N nodes to this cluster" action — driven over an **injected
[`Provisioner`](../cloud/provisioner.md)**. It sits right above the
[spatial node-box canvas](../cluster/cluster.md#spatial-node-box-view-the-headed-canvas), so
the common cluster action is one click away from the membership view it grows.

It is a **thin renderer**: it adds no engine logic of its own. It picks a target +
a count, then calls the `Node`-aware `add_nodes(node, provisioner, count)` overload
(which provisions **and** rebalances sharded partitions onto any new members, #519);
the new members surface in the canvas once they gossip in.

## Targets — the provider catalog

The target selector lists the same
[`CloudProviderCatalog::builtin()`](../cloud/cloud-inspector.md) the Cloud Inspector uses
(local · bare-metal · Kubernetes · AWS · GCP · Azure · OCI). Each entry is pure
metadata — it describes a provisioning target and *exactly what an action there
would read/run*, so the consent gate can name it up front. A green dot marks an
**open** target (Local — no consent); an amber dot marks a **consent-gated** one.

The control drives whatever `Provisioner` was **injected for the selected target's
id** (`App::set_provisioner(target_id, provisioner)`). A target with no injected
provisioner still renders, but reports it isn't wired — the control + consent gate
are real. The cloud targets are wired from
**[local-CLI detection](../cloud/provisioner-detection.md)**: at startup zetta asks each
installed, logged-in CLI (`az`, …) for its deployment context (subscription /
region / resource group — **no credentials**; the CLI keeps its own and the
control plane uses it to act), records it as `system.zetta_provisioners`, and
injects a CLI-backed provisioner per detected target — lighting that target's
**Allow** button up with no UI change. A target whose CLI isn't installed / logged
in stays unwired.

## Local (given hardware) — no consent

`main` wires the **`"local"`** target to a
[`LocalProvisioner`](../cloud/provisioner.md) over the live cluster fleet (it reads
`Cluster::nodes()` on each call, so it tracks self-healing membership). Local
**touches no cloud and brings up no new machines** — it reports/uses the nodes
already in the cluster, best-effort — so it needs no consent and runs on a plain
**Add N nodes** click. On given hardware the result is honestly bounded: adding
more than the cluster has reports `N of M node(s)` with the backend's "given
hardware: cannot add capacity" note (amber). This is the safe default the headed
build ships with.

## Cloud / Kubernetes / on-prem — permission-gated, deny-by-default

Every consent-requiring target follows the **hard rule**
(CLAUDE.md → *Cloud providers — permission-gated*): adding nodes there runs the
provider's CLI to **create billable resources** (a write), so **nothing happens
until the user explicitly allows that one action**, and consent is **never
remembered**. The control:

1. names **what the CLI may read** (the target's `config_paths`, e.g.
   `~/.aws/config`, `~/.aws/credentials`) — verbatim, in mono. The CLI may read
   these; zetta itself never reads or prints them.
2. renders a tinted (amber) **consent callout** that states plainly that the action
   runs the `<cli>` CLI to **create N billable nodes**, that it is a write and
   incurs cost, and that consent applies to this one action only.
3. exposes the action **only** behind an explicit **"Allow adding N nodes via
   `<name>`"** button. That button **is** the per-action consent — there is no
   persisted "granted" state, so a cloud add asks **every time**, deny-by-default.

This mirrors the [Cloud Inspector](../cloud/cloud-inspector.md)'s consent grammar exactly
(named-up-front + tinted callout + explicit allow), applied to a **write** action
rather than a read-only inspect.

## How it runs

`run_add_nodes(target)` is the **single point** that touches the `Provisioner`,
reached only from the Local **Add** button or a cloud target's explicit **Allow**.
The slow provision (the provider CLI — `az vmss create` runs for minutes) runs
**off the UI thread** as a [`runtime::spawn`](../architecture/runtime.md)'d coroutine, so the
window never freezes; `poll_add_nodes` (each frame in the run loop) applies the
result on the UI thread via `apply_added_nodes` (the fast membership + rebalance
half). While it runs the panel shows a **progress bar** (elapsed vs a per-target
ETA baseline) and disables the buttons; a failure renders in full, wrapped +
scrollable, with a one-click **Copy error**. Editable **Region + VM size**
dropdowns (regions detected via `az account list-locations`, sizes fetched
per-region off the UI thread via `az vm list-skus`, applied through
[`Provisioner::set_placement`](../cloud/provisioner.md) before the add) let the operator
steer where a burst lands.

## What this slice does **not** do

- **No teardown from the GUI.** The cloud backends are wired (from
  [local-CLI detection](../cloud/provisioner-detection.md)) and a burst runs off-thread with
  editable placement + SKU fallback, but the control only **grows** the cluster —
  removing a specific burst / node from the GUI is a later slice.
- **No teardown / node removal.** [`add_nodes`](../cloud/add-nodes.md) only grows the
  cluster (it never tears a member down), and the default given-hardware provisioner
  can't release nodes (its `teardown` is a no-op). A `remove_nodes` control — and
  the leadership it needs for cross-node decommission — is a later slice. The epic's
  "node detail/teardown where applicable" is therefore not yet applicable here.
- **No new state or JSON boundary.** The control adds only UI selection
  (`selected_add_target_` / `add_nodes_count_` / a status line). The action rides
  the existing validated [`AddNodesRequest`](../cloud/add-nodes.md) path, and the rebalance
  rides the existing [`DatasetPartition`](../data/dataset-partitions.md) boundary.

## See also

- [add-nodes.md](../cloud/add-nodes.md) — the engine entry point the control drives
  (`zetta::add_nodes`; provision + rebalance, never teardown).
- [provisioner.md](../cloud/provisioner.md) — the `Provisioner` seam + the given-hardware
  `LocalProvisioner` the `"local"` target is wired to.
- [cluster.md](../cluster/cluster.md) — the Cluster panel + the spatial node-box canvas the
  control sits above, and the membership the added nodes join.
- [cloud-inspector.md](../cloud/cloud-inspector.md) — the consent-gate grammar this control
  reuses (named-up-front + tinted callout + explicit, never-remembered allow).
- [partition-rebalance.md](../data/partition-rebalance.md) — the between-jobs rebalance the
  `Node`-aware `add_nodes` triggers onto the new members.
