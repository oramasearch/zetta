# Virtual tables over CRDT entities

> **Status: design / proposed — not yet built.** A living spec being grilled into
> shape (see "Open questions"). When it ships, its durable parts fold into the
> query + cluster domain docs and this plan is trimmed.
>
> **Provenance:** grew out of the `system.nodes` redesign — the reachability-aware
> dialer ([#803](https://github.com/oramasearch/zetta/issues/803)) surfaced full
> cross-cluster membership, and [#800](https://github.com/oramasearch/zetta/issues/800)
> first modelled it as a *second* table (`system.cluster_nodes`). That split was
> wrong: node state is 2 axes today (**type** × **reachability**) heading to more,
> and those belong as **fields on one `system.nodes`**, not a table per view. This
> plan generalizes the mechanism that makes that possible.

## What it is

A **virtual table** is a read-only SQL table whose columns span **three tiers**,
composed at query time (RULE-7 — a `system.<table>` computed from a live source,
never a stored copy):

| tier | source | replicated? | persisted? | update rate | example |
|---|---|---|---|---|---|
| **durable** | a field of a **CRDT entity** (`node:<id>.{…}`) — the source of truth | yes (CRDT gossip) | yes, **lazily** (coalesced / every few min) | rare (on change) | `address`, `reachability`, `node_type` |
| **ephemeral-synced** | gossiped over the **signal plane**, TTL'd | yes (signal, not CRDT) | **no** | many/sec | live telemetry (`cpu_pct`, `mem_pct`) |
| **ephemeral-local** | **this node's** in-memory state only | **no** | no | many/sec | `last_seen`, `directly_connected`, derived `status` |

The point is the **write-frequency split**: fast-changing columns update many times
a second in memory and **never touch the CRDT**, so the replicated document stays
write-quiet (the engine's anti-flap invariant: *a steady stack produces zero CRDT
writes / zero gossip*). Only the slow, durable columns live in the CRDT, and even
those persist to disk lazily. A **virtual table extends a CRDT entity**: it *is* the
CRDT table's rows, with the two ephemeral tiers overlaid at read time.

## Why (motivation)

- **`system.nodes` must show full membership** — every node on a spanning cluster (N
  k8s clusters + hosts), converging from any node, fixing the "5 not 9" confusion —
  *and* carry rich per-node state (type, reachability, liveness, load). But liveness
  and telemetry change many times/sec; putting them in the CRDT would churn the
  replicated doc continuously. The three-tier virtual table is exactly the shape that
  reconciles "durable, converged membership" with "live, high-frequency state."
- **The node-state space grows.** Today: `node_type ∈ {ephemeral, permanent}` ×
  `reachability ∈ {wan-accessible, dial-out-only}` = 4 states, and more axes are
  coming (lifecycle, zone, capacity, role). Each new axis must be a **column**, not a
  new table.
- **Reuse.** The same primitive gives any control-plane CRDT entity (deployments,
  jobs, datasets) a live view for free — no bespoke per-table code path.

## What it is NOT

### Not a materialized view

A traditional **materialized view** is a *persisted, precomputed snapshot of a query
result*, refreshed eagerly or lazily — it materializes (persists) what a plain view
would recompute, trading storage + staleness for read speed. This primitive is nearly
the **inverse**:

- the durable columns are the CRDT **source of truth** (not a cached query result);
- the fast columns are **deliberately kept live / non-materialized**, recomputed each
  query, precisely to avoid write amplification;
- nothing about the query **result** is persisted — only the durable **source** fields
  (in the CRDT), and those aren't a cache.

So "materialized view" would mislead. It is a **live / stateful view**: a durable CRDT
projection with ephemeral overlays.

### Not DDF (they are neighbors, not twins)

The [DDF](../ddf/README.md) (distributed `DataFrame`) is superficially similar (a
"distributed, in-memory table with columns") but is a **different plane** and should
stay a **separate abstraction**:

| axis | **DDF** (data plane) | **virtual table** (control plane) |
|---|---|---|
| purpose | bulk data-parallel **compute** (sort/join/groupby/featurize → GBDT) | read-only **state view** (identity + liveness) |
| cardinality | millions–billions of rows, **partitioned by shard** | 10s–1000s, **fully converged on every node** (no partitioning) |
| backing | transient columnar chunks in RAM / **VRAM**, recomputed from storage | **durable CRDT** identity + ephemeral overlays |
| column types | typed **numeric** (for math/ML) | heterogeneous metadata (text, enums, bools, timestamps) |
| shape | a **transform algebra** (a compute graph) | a **read-only view** (compose at query time) |
| "in-memory" because | data is too big/hot to persist; wants the accelerator | fast fields would churn the CRDT if persisted |

They are "in-memory" for **opposite reasons**. Unifying would force a 9-row node view
to drag in partitioning, device buffers, and collectives it will never use, and force
DDF to accept CRDT durability it explicitly rejects.

**What they share (so we don't build two of anything):** the **SQL row/column
surface** — both emit rows into the same executor (`select_over_rows`), one schema
representation, one `SELECT` path. **They compose at the edges:** a virtual table can
be a `scan` source **into** a DDF; a DDF result can be materialized **into** a table.

## Row set + liveness

- **Rows = the durable CRDT entities.** Durable identity is the membership record: a
  node is a member iff it has a `node:<id>` entry. An ephemeral signal for a node that
  has no CRDT entry is ignored (a node must register its durable identity to appear).
- **Liveness is a derived column, not row presence.** `status ∈ {live, unreachable,
  gone}` computed from: directly-linked (peer table) **or** telemetry within TTL →
  `live`; in the CRDT but neither, past a short grace → `unreachable`; past a long
  grace → `gone`. Plus `last_seen` and `directly_connected`. A declared member that
  goes silent shows as `unreachable` rather than vanishing — what an operator needs.
- **GC of departed entries.** The CRDT node-list is **add-only today** (nothing removes
  a `node:<id>` — a real latent bug: departed nodes would linger forever). A `gone`
  entry past the long grace is a GC candidate; **who removes it is an open question**
  (see below) — a departed node can't remove its own entry.

## DDL surface (keywords) — strawman

The user's ask: *"build it into the DDL with keywords."* A starting point to grill
(syntax not settled):

```sql
CREATE VIRTUAL TABLE system.nodes
  OVER CRDT 'node:'                         -- durable rows = node:<id> entries
COLUMNS (
  node_id             TEXT   DURABLE KEY,   -- the CRDT key (the <id> after 'node:')
  address             TEXT   DURABLE,       -- node:<id>.address
  reachability        TEXT   DURABLE,       -- node:<id>.reachability
  node_type           TEXT   DURABLE,       -- node:<id>.node_type
  cpu_pct             DOUBLE EPHEMERAL SYNCED FROM telemetry.cpu_pct,   -- signal plane, TTL'd
  mem_pct             DOUBLE EPHEMERAL SYNCED FROM telemetry.mem_pct,
  directly_connected  BOOL   EPHEMERAL LOCAL,                           -- this node's peer table
  last_seen_seconds   DOUBLE EPHEMERAL LOCAL,                           -- this node's observation
  status              TEXT   EPHEMERAL LOCAL                            -- derived: live/unreachable/gone
);
```

Design notes / decisions:
- Column tiers are declared with keywords: `DURABLE`, `EPHEMERAL SYNCED`, `EPHEMERAL
  LOCAL`; `KEY` marks the CRDT key column; `FROM <source>` binds an ephemeral column to
  a registered value-source (a resolver reading the telemetry cache / peer table).
- `system.*` tables are engine-defined through this mechanism (not literally run as
  user SQL), so the same registry backs both engine `system.*` views and (later)
  **user-defined** virtual tables over their own CRDT entities. Whether to expose
  `CREATE VIRTUAL TABLE` to users in V1 is an open question.

## `system.nodes` — the first instance

Replaces both the old direct-peer-only `system.nodes` and the proposed
`system.cluster_nodes` (#800) with **one** table:

| column | tier | meaning |
|---|---|---|
| `node_id` | durable key | node id (CRDT key) |
| `address` | durable | dialable address; empty for dial-out-only |
| `reachability` | durable | `wan-accessible` \| `dial-out-only` |
| `node_type` | durable | `permanent` \| `ephemeral` |
| `is_self` | ephemeral-local | this row is the querying node |
| `directly_connected` | ephemeral-local | a live link is held to it right now |
| `status` | ephemeral-local | `live` \| `unreachable` \| `gone` |
| `last_seen_seconds` | ephemeral-local | since this node last saw it (link or telemetry) |
| `entries`,`version`,`in_sync`,`last_sync_seconds` | ephemeral-synced/local | replication status (as today; null for a node we've no live signal from) |
| `cpu_pct`,`mem_pct`,… | ephemeral-synced | live telemetry (as `system.node_metrics` today) |

`node_type` and `reachability` are **orthogonal** (4 states, decoupled) — a departure
from the operator's current "ephemeral forces dial-out-only." Whether the engine keeps
them fully independent is an open question below.

## Relationship to the in-flight cluster work

- **#803 (reachability-aware dialer) is orthogonal and stands** — it's the dialer /
  advertise / HELLO fix (the actual "7 not 9" repair) and is correct regardless of how
  the view is shaped. It already introduced `node:<id> -> { address, reachability }`.
- **This supersedes `system.cluster_nodes` (#800).** When this lands, the separate
  table is folded into the single virtual `system.nodes`. PR
  [#807](https://github.com/oramasearch/zetta/pull/807) currently ships both the #803
  dialer and the #800 second table; the second table is the part reconsidered here.

## Open questions (still grilling)

1. **GC ownership** — who removes a `gone` `node:<id>` from the add-only CRDT list: the
   operator (owns pod lifecycle; but bare/dev clusters have none) or an engine reaper
   (any node removes an entry it has observed `gone` past a long grace — simple, but a
   merely-partitioned node must re-register on return)?
2. **`node_type` source** — self-declared via a `--node-type ephemeral|permanent` flag
   (engine self-sufficient, like `--dial-out-only`), or written by the operator into
   the node-list (operator [#105](https://github.com/oramasearch/orama-cloud-v1-operator/issues/105))?
3. **Orthogonality** — keep `node_type ⊥ reachability` (drop the operator's "ephemeral
   ⇒ dial-out-only" coupling), or keep some coupling?
4. **DDL exposure** — is `CREATE VIRTUAL TABLE` user-facing in V1, or only the internal
   registration for `system.*`?
5. **Ephemeral resolver seam** — how a `FROM <source>` binds to the telemetry cache /
   peer table (a registry of named resolvers?), and the exact liveness thresholds
   (short grace → `unreachable`, long grace → `gone`, telemetry TTL).
6. **Column-tier nullability** — replication/telemetry columns are null for a node we
   have no live signal from; confirm null (not zero) and that `directly_connected` /
   `status` gate their interpretation.
7. **Persistence cadence** — is the "persist the CRDT every 1–5 min" an explicit knob,
   or just the existing coalesced-on-change persistence (which already batches)?
