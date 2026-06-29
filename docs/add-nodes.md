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
[`JobAutoscaler`](job-autoscaler.md). Both are consumers of the same
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
`Cluster::nodes()`). Its UUID is its [CRDT actor id](replication.md), so it owns a
replicated `node:<id>` key in `replication.automerge` — which means the new member
**persists across restart** and shows up in the
[`system.zetta_nodes`](query.md) view, exactly like any other node. This is the
[everything-is-derived rule](replication.md): the cluster's existing membership +
replication path *is* "persist as members"; `add_nodes` just makes the cloud
provisioner feed it automatically.

Identity is the **provisioner's** concern, wired at construction through its
optional [`IdentityHooks`](azure-provisioner.md#bulk-identity--minted-on-provision-revoked-on-full-teardown)
— and because `add_nodes` never tears a member down, any identity minted on
provision simply persists (it is never revoked). A burst provisioner mints
*short-lived* identities; a provisioner configured for permanent members mints
durable ones. `add_nodes` is identity-agnostic and stays out of that choice.

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
[`AutoscalerConfig`](job-autoscaler.md): `from_json` returns `std::nullopt` on
anything malformed — not an object, a missing / non-integer / negative `count`, or
an invalid `spec` — so a misconfigured "add" command fails fast instead of bringing
up the wrong (or an unbounded) fleet. `spec` is optional (omitted ⇒ the no-floor
capability, satisfied by every node). The shape is documented by
[`schemas/add_nodes_request.schema.json`](../schemas/add_nodes_request.schema.json),
which the `from_json` checks **mirror** — keep the two in sync. The DTO exists so a
later slice can drive `add_nodes` from a control command (a CLI verb, a SQL/control
RPC, a GUI button) over a validated payload.

`AddNodesResult` (`{ members, satisfied, detail }`) is the in-memory outcome — the
new permanent members plus whether the full count came up.

## What this slice does **not** do

- **No elastic / autoscaler / cost-cap burst.** Explicitly deferred (issue #518) —
  that is the [`JobAutoscaler`](job-autoscaler.md) +
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

See also [provisioner.md](provisioner.md) (the seam + the Local backend),
[azure-provisioner.md](azure-provisioner.md) (the reference cloud backend
`add_nodes` drives), [job-autoscaler.md](job-autoscaler.md) (the elastic sibling),
and [cluster.md](cluster.md) (the membership the added nodes join).
