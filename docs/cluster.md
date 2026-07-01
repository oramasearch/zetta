# Cluster

zetta nodes form a cluster of **equal peers**. Every node listens for
connections and dials any `--peer` addresses it's given; the handshake is
symmetric, so both sides record each other. There is **no configured leader** —
leader election and failover are planned; until then all nodes are equal.

## Running a cluster

Nodes listen on **all interfaces** and the **same port** by default (`--listen`,
default `0.0.0.0:7777`) so a plain node is reachable across machines out of the
box. To run several nodes on one host (e.g. one per NUMA node), each binds the
**first free port in a range** (`--port-range`, default 16) — so a second local
node grabs `7778` automatically. Point a node at others with `--peer HOST:PORT`
(repeatable; all peers are equal — a seed is just somewhere to start).

```bash
# First node (binds 0.0.0.0:7777, prints its LAN address):
zetta

# Another machine joins it (use the LAN address it printed):
zetta --peer 192.168.1.10:7777

# Second node on the same host (auto-binds 7778), joining the first:
zetta --peer 127.0.0.1:7777

# Restrict to loopback (not reachable from other machines):
zetta --listen 127.0.0.1:7777
```

## Where am I reachable?

At startup (both modes) a node prints its id, the address(es) it's reachable at,
and the command to add another node:

```
zetta: cluster node 229431f5-275e-4754-acb9-2eea4bac007b
zetta: listening on 192.168.123.140:7777
zetta: listening on 127.0.0.1:7777
zetta: add another node with: zetta --peer 192.168.123.140:7777
```

When bound to a specific host that's the one address. When bound to a **wildcard**
(`0.0.0.0` / `::`), the wildcard itself isn't connectable, so `listen_addresses()`
expands it to the machine's actual interface IPv4s — **non-loopback (LAN) first**,
falling back to `127.0.0.1` — and the join command uses the first (most
reachable) one. Other nodes copy that `--peer` line to join.

Headed mode's **Cluster** panel shows this node's id, every reachable listen
address, the **command to add another node** (with a Copy button), an
**[Add nodes control](add-nodes-control.md)** (below), and a **spatial node-box
canvas** (below). Headless logs the per-node table whenever it changes.

## All nodes + replication status

`Cluster::nodes()` returns this node (first) plus **every other node in the
replicated roster** — the `node:<id>` keys in the CRDT document, identical on
every node (see *Dialing the replicated roster* below), node-id sorted. Each row
carries its [replication](replication.md) status (entries, version, whether it's
**in sync** with this node, how long ago it last synced) and a **`connected`**
flag: `true` when this node currently holds a live link, `false` for a roster
member it can't presently reach (which still appears, so an unreachable node is
visible rather than silently absent). A live peer not yet in the roster is
included too. Once a `ReplicationManager` is wired in, peers gossip their
documents and status over the connection (see [replication.md](replication.md)).
The view shows up three ways:

- **Headed** — the Cluster panel's spatial node-box canvas (below).
- **Headless** — the heartbeat prints a per-node table when it changes.
- **SQL** — `SELECT * FROM system.zetta_nodes;` (see [query.md](query.md)).

### Spatial node-box view (the headed canvas)

The headed Cluster panel renders membership **spatially** rather than as a table
(`App::draw_cluster`, `src/gui/app_operations.cpp`, #582): **one selectable box per
node** laid out in a wrapping grid (the worktree-manager "boxes" feel), plus a
**detail pane** below it. Each box shows a health dot (green = self / in-sync,
amber = syncing, quiet = no status learned yet), the node's short id, its address,
and a one-word sync state. **Clicking a box selects it**; the detail pane then
shows that node's full detail — id, role, address, membership, replication
(entries / version / in-sync with seconds), and for this node its reachable listen
addresses + live peer count.

It is a **thin renderer over `Cluster::nodes()`**: the snapshot is read live every
frame and the selection (`selected_cluster_node_`, the only added GUI state) is
re-resolved against it, so a peer that **joins, leaves, or heals** simply appears
or drops out of the grid and the selection falls back to self — the view tracks
membership with no separate copy of it. The boxes reuse the
[graph workspace](execution-graph.md)'s `ImDrawList` rect + invisible-hit-test
pattern and the same Orama canvas palette (white cards, hairline borders, a soft
wash on hover, a coral border when selected), and the per-frame layout is one cheap
pass, so it stays within the `render_frame` / `SDL_WaitEventTimeout` cadence and
doesn't starve a live resize. This canvas is the **spatial foundation** the
add-nodes control, per-node gauges, algorithm animation, and the graph-over-cluster
view build on (part of #575).

#### Live per-node resource gauges (#584)

Each node box also carries **live, animated resource gauges + sparklines** — CPU /
memory / GPU + VRAM / storage / network in-out, with % utilization, as moving bars
rather than static numbers (the animated half of #575 area 2). The bars *ease*
toward the live value each frame (the animation), with a sparkline of recent
history over each track and the Orama **semantic** colours (green → amber → red by
load; a neutral flow for network); an unreportable counter degrades to a faint
`n/a`. Selecting a node shows the same gauges with fuller read-outs (core count,
byte totals, in/out rates) in the detail pane.

The gauges are fed by a **cluster-wide telemetry exchange over the
[signal plane](signal-plane.md)** (`zetta::NodeTelemetryExchange`): `system.node_metrics`
sees only the local host, so each node periodically broadcasts its own sample as an
ephemeral `node.metrics` event and every node caches what it receives, so a single
`Node::telemetry_snapshot()` returns the whole cluster's live gauges (self + fresh
peers). It is **not replicated** (a measurement, not durable state) and rides a
schema-validated wire boundary. Full detail in
[cluster-node-gauges.md](cluster-node-gauges.md).

### Add nodes control (the headed control)

Above the canvas sits the **Add nodes** control (`App::draw_add_nodes_control`,
#583) — the GUI front door for [`zetta::add_nodes`](add-nodes.md), driven over an
**injected [`Provisioner`](provisioner.md)**. Pick a target (the
[provider catalog](cloud-inspector.md)) + a count: the given-hardware **Local**
target uses the injected [`LocalProvisioner`](provisioner.md) and runs on a plain
click (no cloud, no consent); every cloud / Kubernetes / on-prem target is
**permission-gated, deny-by-default** — adding nodes there would run the provider's
CLI to create billable resources, so the control names what it would read/run and
nothing happens until the user explicitly allows that one action (never remembered).
The `Node`-aware overload also [rebalances](partition-rebalance.md) sharded
partitions onto any new members; they appear in the canvas once they gossip in. See
**[add-nodes-control.md](add-nodes-control.md)**.

## How it works

- Networking is **Corosio** (coroutine-native TCP), confined to the adapter
  `src/core/cluster.cpp` behind the `zetta::runtime::net` seam — `Cluster`'s
  public API (`include/zetta/cluster.hpp`) exposes no backend types.
- `Cluster` binds the first free port in `[listen_port, listen_port + port_range)`
  synchronously at `start()`, then runs an `io_context` on a background thread:
  an accept loop plus one dial coroutine per `--peer`.
- **Symmetric handshake** (line protocol): each side sends
  `ZETTA/1 HELLO <node-id> <listen-addr>` and records the other; the dialer
  speaks first, the acceptor replies. The connection stays open; dropping it
  removes the peer. Peers are keyed by **listen address** — one entry per
  address — so a node that restarts (new id, same address) **replaces** its
  stale entry instead of leaving a ghost.
- The advertised `<listen-addr>` is a **reachable** address, never the bare
  wildcard — a `0.0.0.0` bind advertises (and `peer_command()`/the banner show)
  the host's first non-loopback LAN IP, since a peer can't dial `0.0.0.0`.
- `node-id` is a UUIDv4 generated per process.
- **Self-healing links.** Each peer sends traffic every ~0.5s — replication
  gossip when a `ReplicationManager` is wired in, otherwise a lightweight `PING`
  keepalive — so silence means the link
  died: a peer that sends nothing for `peer_idle_timeout_seconds` (default 6) is
  dropped — this clears half-open connections that never produce a clean EOF (a
  silent TCP drop across machines). And every `reconnect_interval_seconds`
  (default 3, well under a minute) a node dials any node it isn't currently
  connected to — see *Dialing the replicated roster* — so a dropped link
  re-establishes itself. Between the two, a restarted or
  briefly-partitioned peer reconnects and the stale entry is replaced — no
  stuck `syncing`/`no status` ghost.

## Dialing the replicated roster

Membership is **replicated state**: on startup each node seeds its own
`node:<id>` key — its id plus a reachable address — into the CRDT document, and
peers merge it, so **every node's document lists every node** (id + address),
identical across the cluster. This roster, **not** the static `--peer` seed list,
is what a node dials.

Every `reconnect_interval_seconds` a node dials each roster member (∪ its
configured seeds) that it isn't currently connected to. So a node **reaches peers
it was never handed as a `--peer`** — it learns their addresses from the roster,
which merged in transitively — and it **keeps trying** a member that's
unreachable in one direction, retrying at least once a minute until that member's
`node:<id>` key leaves the roster. Reconnection is therefore **symmetric**: a node
that only *accepted* a connection dials the peer back once that peer seeds itself
into the roster, rather than depending on the original dialer to return.

A per-address **in-flight-dial guard** means re-dialing the whole roster each
interval never piles up concurrent connect attempts on a slow or one-way
peer — at most one dial per address is outstanding at a time. Removing a dead node
from the roster (a leave / tombstone) is a separate, later slice; until then a
departed node stays listed (and dial-retried), and shows `connected = false` in
`nodes()`.

## Signal plane (RPC / events / tasks)

Beyond replication gossip and large-object transfer, the same peer connections
carry a typed, **ephemeral** message layer — the **signal plane**: request/
response RPC, fire-and-forget events (unicast or broadcast), and task envelopes,
each addressed to a peer **by node id**. It's the substrate for distributed
compute (collectives, work-stealing, job dispatch) and is **not** routed through
the replicated document. The wire frame is one more line verb,
`ZETTA/1 SIGNAL <envelope>`, where the envelope is a single-line JSON object
validated against a JSON Schema at the boundary. Full detail in
[signal-plane.md](signal-plane.md).

## Leadership

zetta is **multi-leader by default** — the CRDT data plane needs no leader, so
the cluster stays equal peers with no global election. Leadership applies only to
**exclusive** structures (locks, etc.) via a planned, replicated `system.zetta_leadership`
table (lease + timeout + ascension), with Raft as a future option scoped to those
structures. Leadership is **per-(node, resource)**: a node holds **zero to many**
roles at once (leader for one resource, follower for another), so `ClusterRole`
(currently a single value, always `Follower`) is a placeholder for a future *set*
of roles, not a node-wide title. This is documented design only — see
[replication.md](replication.md) → *Consistency model*.

## Limits / future

- **Equal peers + membership + replication gossip.** No global leader by design
  (multi-leader CRDT); per-resource leadership for exclusive structures is the
  planned next concern (above).
- Replication carries the full database (DDL + rows): the CRDT document is the
  source of truth and every node's disk is a projection of it — see
  [replication.md](replication.md). Cluster **membership** is still a separate
  peer table (not yet replicated); that's the remaining "everything is
  replicated" gap.
- Plaintext TCP (Corosio supports TLS; not wired yet).
