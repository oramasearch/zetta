# Cluster

zetta nodes form a cluster of **equal peers**. Every node listens for
connections and dials any `--peer` addresses it's given; the handshake is
symmetric, so both sides record each other. There is **no configured leader** —
leader election and failover are planned; until then all nodes are equal.

## Running a cluster

Nodes listen on **all interfaces** and the **same port** by default (`--listen`,
default `0.0.0.0:7777`) so a plain node is reachable across machines out of the
box. `--listen` is honored **exactly**: the node binds that port or **exits
non-zero** — it never silently drifts to another port, because its peer address
is load-bearing (siblings dial it and it's gossiped as membership; a silent move
fragments the mesh — [#738](https://github.com/oramasearch/zetta/issues/738)).
To run several nodes on one host (e.g. one per NUMA node), opt into a range with
`--port-range N`: each then binds the **first free port** in `[PORT, PORT+N)` and
advertises the one it actually bound — so a second local node grabs `7778`. Point
a node at others with `--peer HOST:PORT` (repeatable; all peers are equal — a
seed is just somewhere to start).

All inter-node traffic runs **mutual TLS**, and joining needs the cluster's
**join token** (the first node generates it; the printed join command carries
it) — see [security.md](security.md).

```bash
# First node (binds 0.0.0.0:7777, prints its LAN address + join command):
zetta

# Another machine joins it (copy the exact command the first node printed):
zetta --peer 192.168.1.10:7777 --join-token <64-hex>

# Second node on the same host (--port-range binds the next free port, 7778):
zetta --port-range 8 --peer 127.0.0.1:7777 --join-token <64-hex>

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
zetta: inter-node mTLS on (join token in <data_dir>/security/cluster-secret.json)
zetta: add another node with: zetta --peer 192.168.123.140:7777 --join-token <64-hex>
```

When bound to a specific host that's the one address. When bound to a **wildcard**
(`0.0.0.0` / `::`), the wildcard itself isn't connectable, so `listen_addresses()`
expands it to the machine's actual interface IPv4s — **non-loopback (LAN) first**,
falling back to `127.0.0.1` — and the join command uses the first (most
reachable) one. Other nodes copy that `--peer` line to join.

Headed mode's **Cluster** panel shows this node's id, every reachable listen
address, the **command to add another node** (with a Copy button), an
**[Add nodes control](../gui/add-nodes-control.md)** (below), and a **spatial node-box
canvas** (below). Headless logs the per-node table whenever it changes.

## All nodes + replication status

`Cluster::nodes()` returns this node (first) plus every connected peer, each with
its [replication](replication.md) status: entries, version, whether it's **in
sync** with this node, and how long ago it last synced. Once a `ReplicationManager`
is wired in, peers gossip their documents and status over the connection (see
[replication.md](replication.md)). The view shows up three ways:

- **Headed** — the Cluster panel's spatial node-box canvas (below).
- **Headless** — a one-line status heartbeat prints every **~30s**, and the full
  per-node table prints **immediately** whenever membership or a sync state
  changes (a node joins/leaves or flips in/out of sync). The per-second heartbeat
  was throttled to ~30s so a long-running node doesn't flood its log
  ([#788](https://github.com/oramasearch/zetta/issues/788)); membership changes
  are still surfaced within ~1s. The one-time startup banner (node id, listen
  addresses, join command) is unchanged.
- **SQL** — `SELECT * FROM system.nodes;` (see [query.md](../data/query.md)).

### Spatial node-box view (the headed canvas)

The headed Cluster panel renders membership **spatially** rather than as a table
(`App::draw_cluster`, `src/gui/app_cluster.cpp`, #582): **one selectable box per
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
[graph workspace](../pipeline/execution-graph.md)'s `ImDrawList` rect + invisible-hit-test
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
[signal plane](signal-plane.md)** (`zetta::cluster::NodeTelemetryExchange`): `system.node_metrics`
sees only the local host, so each node periodically broadcasts its own sample as an
ephemeral `node.metrics` event and every node caches what it receives, so a single
`Node::telemetry_snapshot()` returns the whole cluster's live gauges (self + fresh
peers). It is **not replicated** (a measurement, not durable state) and rides a
schema-validated wire boundary. Full detail in
[cluster-node-gauges.md](../gui/cluster-node-gauges.md).

### Add nodes control (the headed control)

Above the canvas sits the **Add nodes** control (`App::draw_add_nodes_control`,
#583) — the GUI front door for [`zetta::cloud::add_nodes`](../cloud/add-nodes.md), driven over an
**injected [`Provisioner`](../cloud/provisioner.md)**. Pick a target (the
[provider catalog](../cloud/cloud-inspector.md)) + a count: the given-hardware **Local**
target uses the injected [`LocalProvisioner`](../cloud/provisioner.md) and runs on a plain
click (no cloud, no consent); every cloud / Kubernetes / on-prem target is
**permission-gated, deny-by-default** — adding nodes there would run the provider's
CLI to create billable resources, so the control names what it would read/run and
nothing happens until the user explicitly allows that one action (never remembered).
The `Node`-aware overload also [rebalances](../data/partition-rebalance.md) sharded
partitions onto any new members; they appear in the canvas once they gossip in. See
**[add-nodes-control.md](../gui/add-nodes-control.md)**.

## How it works

- Networking is **Corosio** (coroutine-native TCP), confined to the adapter
  `src/core/cluster/cluster.cpp` behind the `zetta::runtime::net` seam — `Cluster`'s
  public API (`include/zetta/cluster/cluster.hpp`) exposes no backend types.
- **Every connection runs mutual TLS 1.3 first** (Corosio's OpenSSL provider)
  when `ClusterConfig::security` is enabled — the handshake precedes any
  `ZETTA/1` line, so an unenrolled peer never reaches `HELLO`. The zetta binary
  enables it by default; `--cluster-plaintext` opts out for debugging. See
  [security.md](security.md).
- `Cluster` binds the first free port in `[listen_port, listen_port + port_range)`
  synchronously at `start()`, then runs an `io_context` on a background thread:
  an accept loop plus one dial coroutine per `--peer`. `port_range` defaults to
  **1** (strict — exactly `listen_port`); the acceptor sets **`SO_REUSEADDR`**
  before bind so a restarted node reclaims its exact port even while the previous
  process's sockets linger in `TIME_WAIT`. If nothing binds, `is_listening()` is
  false and the zetta binary **exits non-zero** rather than run without its
  advertised address — never a silent drift
  ([#738](https://github.com/oramasearch/zetta/issues/738)).
- **Symmetric handshake** (line protocol): each side sends
  `ZETTA/1 HELLO <node-id> <endpoint[,endpoint,…]>` and records the other; the
  dialer speaks first, the acceptor replies. The connection stays open; dropping
  it drops the live link. Peers are keyed by **reached address** — one entry per
  address — so a node that restarts (new id, same address) **replaces** its stale
  entry instead of leaving a ghost.
- **A node advertises a SET of candidate endpoints, not one address** (#739). The
  HELLO carries a comma-separated set — the reachable listen address(es) plus any
  `--advertise HOST:PORT[,…]` extras (a node behind NAT / a load balancer lists its
  public/LB address there, so peers learn the endpoint they can actually dial, not
  just the unreachable bound pod IP — the root cause of the membership flap). A
  legacy single-address HELLO parses as a one-element set (backward compatible).
  `Cluster::advertised_endpoints()` is the set a node sends.
- **The reached endpoint is per-observer.** Each side records the endpoint it
  actually reached the peer on (the address it dialed; for the acceptor, the peer's
  advertised front) — the reachable path differs by vantage point (a home node
  reaches an AKS pod via the LB, an in-cluster node via the pod IP). A seed may
  itself be a comma-separated candidate set for one peer; the dialer tries the
  candidates in order and keeps the first that connects.
- **A seed/candidate host may be a DNS name, not just a numeric IPv4** — e.g. a
  Kubernetes StatefulSet per-pod address
  (`foo-0.foo-headless.ns.svc.cluster.local:7777`). A non-numeric host is
  **resolved at dial time** and the dialer tries the resolved address(es) in
  order; resolution is **never cached** (a pod restart gets a fresh IP, so every
  redial re-resolves), and the *recorded* reached endpoint stays the stable
  seed/DNS label rather than the ephemeral IP
  ([#785](https://github.com/oramasearch/zetta/issues/785)).
- The advertised endpoints are **reachable** addresses, never the bare wildcard —
  a `0.0.0.0` bind advertises (and `peer_command()`/the banner show) the host's
  first non-loopback LAN IP, since a peer can't dial `0.0.0.0`.
- **Dial-out-only nodes advertise NO dialable address; peers must never dial them**
  ([#803](https://github.com/oramasearch/zetta/issues/803)). A node behind NAT / a private VNet has no
  routable address — it advertises an empty dialable set (node-id + a dial-out-only marker) and is a pure
  **initiator**: the one outbound link it opens carries CRDT sync **both directions**. Advertising a
  private/bound pod IP is actively harmful when two k8s clusters **share a pod CIDR** (both `10.244.0.x`) —
  a peer dials the colliding address, reaches the WRONG node, and membership undercounts. Reachability is
  gossiped so every peer makes the same don't-dial decision.
- `node-id` is a UUIDv4 generated per process.
- **Direct peers vs full membership.** `system.nodes` lists a node's DIRECT peers + self, so on a
  cross-cluster (spanning) mesh no single node shows all N members (the "5 not 9" surprise) — full
  gossiped membership rides the CRDT node-list and is surfaced separately
  ([#800](https://github.com/oramasearch/zetta/issues/800)). For aggregate cross-cluster bandwidth, each
  PERMANENT wan-accessible node should advertise its OWN address (per-node LB, orama-cloud-v1-operator#104)
  so sync spreads across every node's uplink instead of funnelling through one shared rendezvous.
- **Self-healing links.** Each peer sends traffic every ~0.5s — replication
  gossip when a `ReplicationManager` is wired in, otherwise a lightweight `PING`
  keepalive — so silence means the link
  died: a peer that sends nothing for `peer_idle_timeout_seconds` (default 6) is
  dropped — this clears half-open connections that never produce a clean EOF (a
  silent TCP drop across machines). And every `reconnect_interval_seconds`
  (default 3) a node re-dials any `--peer` it isn't currently connected to, so a
  dropped link re-establishes itself. Between the two, a restarted or
  briefly-partitioned peer reconnects and the stale entry is replaced — no
  stuck `syncing`/`no status` ghost. This self-healing **depends on a node
  keeping its advertised address across a restart**: siblings re-dial the exact
  `--peer` they were given, so a node that drifted to a different port would
  never be found again. Strict `--listen` + `SO_REUSEADDR` (above) guarantee the
  reclaimed address matches ([#738](https://github.com/oramasearch/zetta/issues/738)).
  stuck `syncing`/`no status` ghost.
  (default 3) a node re-dials any `--peer` **whose node it doesn't currently hold a
  live link to**, so a dropped link re-establishes itself. The reconnect is deduped
  **by node id**, not by matching the seed string against advertised addresses: a
  seed that never matches (a `127.0.0.1` loopback or a load-balancer VIP vs. the
  peer's advertised address) used to redial every interval forever — connect +
  handshake churn (bounded by #741's reap, but wasteful). Now, once a seed has
  resolved to a node that's linked, it's skipped (#739). Between the two, a
  restarted or briefly-partitioned peer reconnects and the stale entry is replaced —
  no stuck `syncing`/`no status` ghost.
- **Membership hysteresis (#739).** When a peer's live link drops, the live views
  (`peers()`/`peer_count()` and the signal plane) reflect it at once, but the
  **membership** view (`nodes()` / `system.nodes` / the UI) keeps the node for
  `membership_grace_seconds` (default 10) before dropping it, so a brief blip or a
  reconnect doesn't flap it out — it ages out only after a sustained absence. This
  debounces the down edge; the up edge is immediate (a node appears the moment a
  live link is held). Mirrors the operator's status anti-flap.
- **One connection per peer (no leak).** A handshake is keyed by the peer's
  **node id**: the moment both ends learn each other's id, a per-node-id registry
  keeps exactly one link and closes any duplicate. This covers a mutual dial
  (both sides seed each other, so each also accepts the other — the link
  *initiated by the smaller node id* wins on both ends, so they converge on the
  same physical connection) and a redial race where a seed's address never
  matches the peer's advertised one (e.g. a `127.0.0.1` or load-balancer seed vs.
  the peer's real LAN IP), which would otherwise pile up held sockets and burn CPU
  servicing them. `Cluster::connection_count()` reports the links actually served —
  it equals `peer_count()` in steady state; a higher value means links are leaking
  (#739).

## Node reachability + the route table

Not every node can accept inbound connections, so the mesh forms by having the
unreachable nodes dial **outbound** to the reachable ones. Nodes are classified by
reachability (a property of the node, added to the nodes table):

- **wan-accessible** — the node has a routable address (a public/WAN IP, or a LAN IP
  its peers can reach). It's advertised in the nodes table at that address and is a
  **rendezvous**: other nodes dial **outbound** to it. It accepts inbound because it
  genuinely can.
- **ephemeral** — a transient node (burst / autoscaled capacity). It's expected to come
  and go; its departure is normal, not a fault, so membership shouldn't flap it out on a
  brief absence (hysteresis — #739).
- otherwise **dial-out-only** — behind NAT or private cloud networking (a home node; a
  cloud pod with only a private VNet IP). It opens **outbound** connections to
  wan-accessible nodes and accepts **no inbound** — no load balancer, no exposed port.

**The route table self-forms.** Add a wan-accessible node's address to the nodes table (a
`--peer` / seed) and existing nodes reach out to it; gossip then relays transitively so
every node reaches every other **through** the wan-accessible rendezvous(es), each node
keeping a per-peer reachability/route table (#739). A dial-out-only node behind NAT never
needs to be dialed — it is always the initiator.

> **Anti-pattern:** exposing a dial-out-only node via an inbound LoadBalancer (worse,
> source-locked to one peer's IP) to "make it reachable" **inverts the model** — it puts
> an inbound surface on a node that should only dial out. Designate the genuinely
> wan-accessible nodes instead, and let the private ones dial them.

### What the route table holds today (#739)

Each `PeerRecord` carries the peer's advertised **candidate-endpoint set** plus the
one endpoint **we reached it on** (per-observer). The pieces in place:

- **Multi-endpoint advertise** — the HELLO carries the whole candidate set; a node
  behind NAT/LB adds its public/LB address with `--advertise`, so the reachable path
  is advertised, not just the bound pod IP.
- **Per-vantage path selection** — a seed may list several candidate endpoints for one
  peer; the dialer tries them in order and caches the one that connected, so the
  reachable endpoint is chosen from *this* node's vantage point.
- **Prefer the established connection** — the per-node-id link registry (#741) keeps a
  single bidirectional session and never re-dials a candidate a live link already
  covers (reconnect deduped by node id).
- **Membership hysteresis** — a node stays in `system.nodes` through the grace window
  after its link drops, so a blip on one endpoint doesn't flap it out.
- **Dial learned members ([#789](https://github.com/oramasearch/zetta/issues/789))** —
  the reconnect loop dials not just configured seeds but every member it has learned:
  a direct peer whose link dropped, plus a node known only **transitively** from the
  replicated `node:<id>` membership entries gossiped through a common peer. Deduped by
  node id (skip a node we already hold a live link to; one dial in flight per node),
  with the registry reaping any duplicate a mutual dial makes. This fills a hub-seeded
  **star** (every node `--peer`'d at one seed, as k8s does) into a full equal-peer
  **mesh** — learned members were recorded but never dialed before.

**Division of responsibility.** The **operator** supplies candidate endpoints (each
slice's LB/public + LAN addresses, via `--advertise` / peer-LB wiring); **Zetta** owns
the routing — reachability selection, connection lifecycle, and membership hysteresis.

**Still future.** Reachability *classification* (`wan-accessible` / `ephemeral` /
`dial-out-only` as a field on the nodes table) and RTT-ranked candidate ordering are
not yet built. A node **does** now dial members it learns second-hand — via the
replicated `node:<id>` membership entries — so a hub-seeded star fills into a full
mesh (#789); what remains future is **relaying a peer's full advertised endpoint
set** (its LB / public candidates), so a transitively-learned member is currently
dialed by its single `node:` address rather than by every path it advertises. The
registry + hysteresis keep the mesh stable.

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
**exclusive** structures (locks, etc.) via a planned, replicated `system.leadership`
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
