# Add nodes (permanent cluster expansion)

**`add_nodes`** (`include/zetta/add_nodes.hpp`) is the **permanent-add control
entry point** (`#518`): the plain "add 10 nodes to this cluster" action, on top of
the [`Provisioner`](provisioner.md) seam. It drives a provisioner (the cloud-burst
[`AzureProvisioner`](azure-provisioner.md) per the issue, or any backend) to bring
up N nodes, each of which **auto-joins the cluster** via the seed-peer handshake
and **persists as an ordinary member** — the same membership path a hand-added node
takes, now **automatic with the cloud provisioner**.

```cpp
AddNodesResult add_nodes(Provisioner& provider, const AddNodesRequest& request);

// The issue's add_nodes(azure, N) UX (spec defaults to the no-floor capability):
AddNodesResult add_nodes(Provisioner& provider, std::size_t count,
                         const NodeCapability& spec = {});
```

```cpp
AzCliControlPlane azure_cli;                     // the reference `az` binding
AzureProvisioner  azure{azure_cli, azure_config};

AddNodesResult added = add_nodes(azure, 10);     // "add 10 nodes to this cluster"
// added.members  → the new permanent members that came up (best-effort)
// added.satisfied → were all 10 added?
```

## Permanent, not burst

`add_nodes` is the **permanent sibling** of the
[`JobAutoscaler`](../compute/job-autoscaler.md). Both are consumers of the same
`Provisioner&` seam, but they sit at opposite ends of the lifecycle:

| | `JobAutoscaler` (elastic / burst) | `add_nodes` (permanent) |
|---|---|---|
| Brings nodes up | yes (toward a reward target) | yes (a fixed count) |
| Tears nodes down | yes (on a scale-down / cost-cap shed) | **never** |
| Revokes identities | yes (on full teardown) | **never** |
| Driven by | a per-tick control loop | one control action |
| Membership | elastic (joins/leaves mid-job) | durable members that stay |

`add_nodes` **only** provisions and reports — it never calls `teardown()` or
revokes an identity, so the members it adds **stay**. Elastic / autoscaler /
cost-cap burst is deliberately out of scope here; those drive the *same*
`Provisioner` through the autoscaler's own loop (optionally behind the
[cost-cap approval gate](burst-approval-gate.md)).

## "Join membership and persist as members"

`add_nodes` does **not** maintain a separate roster — it doesn't need to. A node a
provisioner brings up boots with a `ZETTA_SEED_PEER` so it **dials the cluster and
the symmetric handshake records it on both sides** (`Cluster::peers()` /
`Cluster::nodes()`). Its UUID is its [CRDT actor id](../cluster/replication.md), so it owns a
replicated `node:<id>` key in `replication.automerge` — which means the new member
**persists across restart** and shows up in the
[`system.zetta_nodes`](../data/query.md) view, exactly like any other node. This is the
[everything-is-derived rule](../cluster/replication.md): the cluster's existing membership +
replication path *is* "persist as members"; `add_nodes` just makes the cloud
provisioner feed it automatically.

Identity is the **provisioner's** concern, wired at construction through its
optional [`IdentityHooks`](azure-provisioner.md#bulk-identity--minted-on-provision-revoked-on-full-teardown)
— and because `add_nodes` never tears a member down, any identity minted on
provision simply persists (it is never revoked). A burst provisioner mints
*short-lived* identities; a provisioner configured for permanent members mints
durable ones. `add_nodes` is identity-agnostic and stays out of that choice.

## Rebalance partitions onto the new members (#519)

Adding nodes changes the cluster membership — but the
[partition catalog](../data/dataset-partitions.md) still records the **old** owners, so the
existing data stays on the original nodes until something re-places it. The
`Node`-aware overloads **wire the node-management action to the
[between-jobs rebalance pass](../data/partition-rebalance.md)** so the data spreads onto the
new nodes as a direct, synchronous consequence of the add:

```cpp
AddNodesResult add_nodes(Node& node, Provisioner& provider,
                         const AddNodesRequest& request);
AddNodesResult add_nodes(Node& node, Provisioner& provider, std::size_t count,
                         const NodeCapability& spec = {});

AddNodesResult added = add_nodes(node, azure, 10);  // add 10 nodes AND rebalance
```

After the provision, the overload recomputes **sharded** partition placement
([rendezvous / HRW](../data/partitioner.md)) against the grown membership via
[`rebalance_partitions`](../data/partition-rebalance.md):

- **previous membership** = `node`'s current replicated `node:<id>` set
  ([`Node::cluster_members()`](../architecture/architecture.md)), which is also what leaves
  replicate-to-all partitions untouched;
- **grown membership** = the previous set **plus** the just-provisioned members
  (each `ProvisionedNode::node_id` is the cluster / CRDT id), de-duplicated.

So **≈ rf/N of the partitions move onto each new node** (whole partitions, no full
rehash), and the re-placement is visible **immediately** in
`node.dataset_partitions()` / [`system.dataset_partitions`](../data/query.md) — the
acceptance. The catalog moves are captured into the CRDT (so they replicate), and
the partition **bodies follow for free** over the existing
[sharded-blob-placement](../data/sharded-blob-placement.md) seam (the new owner `LOREQ`s
the body; a non-owner refuses it). When nothing new came up — a zero / best-effort-
short add, or an idempotent re-add of already-present members — there is no
membership delta and the rebalance is **skipped**.

The existing [`Node::tick()` membership auto-trigger](../data/partition-rebalance.md#the-auto-trigger-between-jobs)
(#517) stays as the **safety net**: once the new nodes actually boot, join, and
gossip their `node:<id>` keys in, the next `tick()` re-confirms the placement against
the **real** replicated membership — idempotent, moving nothing the direct pass
already placed (and correcting it should a reported member never actually join). The
Provisioner-only overloads above are unchanged (no `Node`, no rebalance) for callers
that only want to bring nodes up.

## Behaviour

| | |
|---|---|
| `add_nodes(provider, request)` | Calls `provider.provision(ProvisionRequest{count, spec})`, then returns the brought-up nodes as the new permanent `members`, with `satisfied` / `detail` passed through. **Best-effort** — a backend may return fewer than `count` (cloud quota / Spot capacity, or a given-hardware fleet that is already full). A `count` of 0 is a satisfied no-op. |
| `add_nodes(provider, count, spec)` | The convenience overload — builds an `AddNodesRequest{count, spec}` (spec defaults to the no-floor capability) and forwards. |

It is **vendor-neutral**: it programs against the abstract `Provisioner&` (like the
autoscaler), so the *same* entry point drives the cloud backends, the
given-hardware [`LocalProvisioner`](provisioner.md), or a cost-cap-decorated
provisioner unchanged.

## `AddNodesRequest` — the control-command JSON boundary

`AddNodesRequest` (`{ count, spec }`) carries a **validated JSON boundary** (`to_json`
/ non-throwing `from_json`), the same hand-written pattern as
[`NodeCapability`](provisioner.md) / [`AzureConfig`](azure-provisioner.md) /
[`AutoscalerConfig`](../compute/job-autoscaler.md): `from_json` returns `std::nullopt` on
anything malformed — not an object, a missing / non-integer / negative `count`, or
an invalid `spec` — so a misconfigured "add" command fails fast instead of bringing
up the wrong (or an unbounded) fleet. `spec` is optional (omitted ⇒ the no-floor
capability, satisfied by every node). The shape is documented by
[`schemas/add_nodes_request.schema.json`](../../schemas/add_nodes_request.schema.json),
which the `from_json` checks **mirror** — keep the two in sync. The DTO exists so a
later slice can drive `add_nodes` from a control command (a CLI verb, a SQL/control
RPC, a GUI button) over a validated payload.

`AddNodesResult` (`{ members, satisfied, detail }`) is the in-memory outcome — the
new permanent members plus whether the full count came up.

## What this slice does **not** do

- **No elastic / autoscaler / cost-cap burst.** Explicitly deferred (issue #518) —
  that is the [`JobAutoscaler`](../compute/job-autoscaler.md) +
  [`BurstApprovalGate`](burst-approval-gate.md), which act through the same seam.
- **No node removal.** `add_nodes` only grows the cluster; a permanent
  `remove_nodes` control action (and the leadership it needs for cross-node
  decommission) is a later slice.
- **No live-cloud CI.** Like every provisioner backend, the acceptance is the
  *shape* — the control action + the result/JSON boundary, verified against a fake
  `Provisioner`. The actual cloud bring-up is the provisioner's own concern
  (install + auth the vendor CLI), unchanged by this entry point.
- **No new persistence of its own.** "Persist as members" is the existing cluster
  membership + replication path; `add_nodes` adds no parallel roster.

## Tests

`tests/test_add_nodes.cpp` covers the control action and the JSON boundary against a
fake `Provisioner`:

- **control action** — adds N permanent members carrying the requested capability
  floor; best-effort when the backend is short (`satisfied == false` + a `detail`);
  a zero count is a satisfied no-op; it **never tears a member down** (the members
  stay up and `teardown` is called zero times, even across a re-add); the request
  and convenience overloads agree; and it drives a concrete non-cloud backend (the
  `LocalProvisioner`) unchanged, proving vendor-neutrality.
- **`AddNodesRequest` JSON boundary** — round-trips a minimal and a capability-floor
  request; `spec` is optional and defaults to the no-floor capability; and it
  rejects malformed documents (non-object, missing / negative / wrong-typed `count`,
  an invalid embedded `spec`).

The **rebalance wiring** (#519) is covered in
`tests/test_partition_rebalance.cpp` (alongside the rebalance pass it drives):
`add_nodes(node, …)` over a partitioned table spreads ≈ rf/N of the partitions onto
the new member (verified through `node.dataset_partitions()`), keeps every row
covered exactly once, and never tears a member down; a zero / best-effort-short add
leaves placement untouched.

See also [provisioner.md](provisioner.md) (the seam + the Local backend),
[azure-provisioner.md](azure-provisioner.md) (the reference cloud backend
`add_nodes` drives), [partition-rebalance.md](../data/partition-rebalance.md) (the rebalance
pass `add_nodes(node, …)` triggers), [job-autoscaler.md](../compute/job-autoscaler.md) (the
elastic sibling), and [cluster.md](../cluster/cluster.md) (the membership the added nodes join).
