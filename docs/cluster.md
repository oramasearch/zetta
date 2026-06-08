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
address, the **command to add another node** (with a Copy button), and a
**Nodes & replication** table. Headless logs the same table whenever it changes.

## All nodes + replication status

`Cluster::nodes()` returns this node (first) plus every connected peer, each with
its [replication](replication.md) status: entries, version, whether it's **in
sync** with this node, and how long ago it last synced. Once a `ReplicationManager`
is wired in, peers gossip their documents and status over the connection (see
[replication.md](replication.md)). The view shows up three ways:

- **Headed** — the Cluster panel's Nodes table.
- **Headless** — the heartbeat prints a per-node table when it changes.
- **SQL** — `SELECT * FROM zetta_nodes;` (see [query.md](query.md)).

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
  removes the peer. Peers are de-duplicated by node id.
- The advertised `<listen-addr>` is a **reachable** address, never the bare
  wildcard — a `0.0.0.0` bind advertises (and `peer_command()`/the banner show)
  the host's first non-loopback LAN IP, since a peer can't dial `0.0.0.0`.
- `node-id` is a UUIDv4 generated per process.

## Leadership

zetta is **multi-leader by default** — the CRDT data plane needs no leader, so
the cluster stays equal peers with no global election. Leadership applies only to
**exclusive** structures (locks, etc.) via a planned, replicated `zetta_leadership`
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
- Replication gossips a per-node schema snapshot today; streaming live `Node`
  DDL + row mutations into the replicated document (CRDT as source of truth, disk
  as projection) is the active replication build — see [replication.md](replication.md).
- Plaintext TCP (Corosio supports TLS; not wired yet).
