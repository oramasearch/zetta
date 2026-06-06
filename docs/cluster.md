# Cluster

zetta nodes form a cluster of **equal peers**. Every node listens for
connections and dials any `--peer` addresses it's given; the handshake is
symmetric, so both sides record each other. There is **no configured leader** —
leader election and failover are planned; until then all nodes are equal.

## Running a cluster

Nodes default to the **same port** (`--listen`, default `127.0.0.1:7777`). To run
several nodes on one host (e.g. one per NUMA node), each binds the **first free
port in a range** (`--port-range`, default 16) — so a second local node grabs
`7778` automatically. Point a node at others with `--peer HOST:PORT` (repeatable;
all peers are equal — a seed is just somewhere to start).

```bash
# First node (binds 127.0.0.1:7777):
zetta

# Second node on the same host (auto-binds 7778), joining the first:
zetta --peer 127.0.0.1:7777

# Explicit address / across machines:
zetta --listen 0.0.0.0:7777
zetta --listen 0.0.0.0:7777 --peer <other-host>:7777
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
address, the connected peers, and the **command to add another node** (with a
Copy button). Headless logs `cluster: N peer(s)` each second.

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
- `node-id` is a UUIDv4 generated per process.

## Limits / future

- **Equal peers + membership only.** Leader election and failover are the next
  step; `ClusterRole` exists for that future (currently always `Follower`).
- No data replication or consensus yet.
- Plaintext TCP (Corosio supports TLS; not wired yet).
