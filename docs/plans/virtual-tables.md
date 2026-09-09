# Virtual tables over CRDT entities

> **Status: design / proposed — not yet built.** A living spec being grilled into
> shape (see "Open questions"). When it ships, its durable parts fold into the
> query + cluster domain docs and this plan is trimmed. Tracking issue:
> [#809](https://github.com/oramasearch/zetta/issues/809).
>
> **Provenance:** grew out of the `system.nodes` redesign — the reachability-aware
> dialer ([#803](https://github.com/oramasearch/zetta/issues/803)) surfaced full
> cross-cluster membership, and [#800](https://github.com/oramasearch/zetta/issues/800)
> first modelled it as a *second* table (`system.cluster_nodes`). That split was
> wrong: node state is 2 axes today (**type** × **reachability**) heading to more,
> and those belong as **fields on one `system.nodes`**, not a table per view. This
> plan generalizes the mechanism that makes that possible.

## What it is

A **virtual table** is a SQL table whose columns span **three tiers**, composed at
query time (RULE-7 — computed from a live source, never a stored copy).

> **Defining invariant: a virtual table MUST extend a CRDT table.** The durable tier is
> mandatory and CRDT-backed — it is the table's convergent, replicated **row identity**;
> the two ephemeral tiers are only *enrichment* laid over those rows. A virtual table
> cannot be purely ephemeral (that is a stream, not a table) and cannot sit on a
> non-CRDT base (nothing durable to anchor the rows / overlays to). It applies to **any**
> CRDT table — a system entity family (`node:`, `deployment:`, …) *or* a user data table
> — and is user-facing via DDL (the `OVER CRDT <table>` clause is **required**).

The three tiers:

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
  entry past the long grace is a GC candidate. A departed node can't remove its own
  entry, so removal is done by the **operator** (it owns pod lifecycle) and, for
  operator-less clusters, an **engine reaper** (any node removes an entry it has
  observed `gone` past a long grace). This is safe on a multi-writer CRDT (see below);
  the one chosen semantic is **re-register-wins** — a briefly-partitioned node that
  returns re-asserts its entry with a causally-later write, superseding a stale
  removal, so only genuinely-departed nodes stay gone.

## Writer model — multi-leader (writes from anywhere)

This is the engine's **multi-leader** architecture (the core rule: *multi-leader by
default; equal peers, no global leader or election*). **Writes are allowed from
anywhere and rely on robust reconciliation by design**: every change to the node-list
carries its author's actor id (a node's replication actor id *is* its node_id — one
UUID for both), so concurrent writes from **any node or the operator** merge
conflict-free and converge everywhere, like Google Docs. There is no single-writer
constraint, no locking, no coordination.

Division of labor is therefore a **soft semantic convention**, not a mechanism:

- **the node self-declares its own identity** — `address`, `reachability`, `node_type`
  — from its own flags (`node_type` via `--node-type ephemeral|permanent`, like
  `--dial-out-only`). The node is the authority on *what it is*; the engine stays
  self-sufficient (bare hosts / dev / the macOS node need no operator).
- **the operator enriches + drives lifecycle** — labels, desired-state, and **GC**
  (removing `node:<id>` for pods it deleted). It composes with, not replaces,
  self-registration (operator [#105](https://github.com/oramasearch/orama-operator/issues/105)
  is enrichment, not the sole writer).

Because merges are conflict-free, bending the convention (both write a field) simply
converges deterministically — no corruption. The only semantic that matters is
GC's **re-register-wins** (above).

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
- `OVER CRDT <table>` is **required** (the defining invariant) — every virtual table
  names its durable CRDT base. The base may be a system entity family (`node:`) or a
  user data table.
- **`CREATE VIRTUAL TABLE` is user-facing** (any CRDT table), and the same declaration
  mechanism also backs the engine's `system.*` views (one registry, not a bespoke code
  path — this is what kills `cluster_node_rows`). Governance: you may define a virtual
  table over a CRDT table you can access; the ephemeral resolvers are drawn from a
  registered set (telemetry, peer table, …).
- **Read model:** the virtual table is a **read** projection; durable columns are
  mutated by writing the **CRDT base table** through its normal path (the node
  self-registers, or an `INSERT` into the base), and ephemeral columns are pushed by
  the runtime, never by SQL. Whether to allow **write-through** on durable columns (an
  `UPDATE` on the virtual table rewriting the CRDT base) is a later refinement — not
  needed for `system.nodes`.

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

`node_type` and `reachability` are **fully orthogonal** — any of the 4 combinations is
legal, and they can be mixed in one cluster (permanent+wan, permanent+dial-out,
ephemeral+wan, ephemeral+dial-out). The operator's "ephemeral ⇒ dial-out-only" is a
*policy* the operator applies, not an engine law.

### `node_type` is load-bearing beyond the view — it is a durability concept

`node_type` is not just metadata; it drives the replication/placement model, and this
is *why* it must be first-class:

- **Replication factor is maintained over the `permanent` set only.** Durability = how
  many **permanent** nodes hold a copy. Only a **permanent** node leaving moves RF and
  triggers re-replication to restore durability.
- **`ephemeral` nodes never count toward RF.** They may pull a **locality** replica
  (fast local access / compute-near-data), but it carries **no durability weight** — so
  when an ephemeral node vanishes at any moment, RF is untouched and the system does not
  scramble to re-replicate. Ephemeral churn must not wreak havoc on the replication
  factor.

This makes the engine's placement / `ReplicationPolicy` **node-type-aware** (RF over
permanent replicas; ephemeral = optional, uncounted locality copies) — a **separate,
larger engine concern** that this view merely *surfaces*. It should get its own design
+ issue (see open questions); the virtual-table work only needs `node_type` to exist as
a durable, self-declared field.

## Relationship to the in-flight cluster work

- **#803 (reachability-aware dialer) is orthogonal and stands** — it's the dialer /
  advertise / HELLO fix (the actual "7 not 9" repair) and is correct regardless of how
  the view is shaped. It already introduced `node:<id> -> { address, reachability }`.
- **This supersedes `system.cluster_nodes` (#800).** When this lands, the separate
  table is folded into the single virtual `system.nodes`. PR
  [#807](https://github.com/oramasearch/zetta/pull/807) currently ships both the #803
  dialer and the #800 second table; the second table is the part reconsidered here.

## Resolved (grilled)

- **Writer model** — multi-leader; writes from anywhere converge conflict-free (above).
- **GC ownership** — operator (owns pod lifecycle) + an engine reaper fallback for
  operator-less clusters; **re-register-wins** by causality so a returning node isn't
  wrongly reaped.
- **`node_type` source** — **self-declared** via `--node-type ephemeral|permanent`
  (like `--dial-out-only`); the operator passes the flag but the node writes the field.
- **Orthogonality** — `node_type ⊥ reachability`, fully independent; all 4 combinations
  legal and mixable in one cluster. The "ephemeral ⇒ dial-out" coupling is an operator
  policy, not an engine law.
- **Must extend a CRDT table** — the defining invariant (above). Durable CRDT base is
  required; ephemeral tiers are enrichment only.
- **DDL exposure** — **user-facing** `CREATE VIRTUAL TABLE OVER CRDT <table>` for any
  CRDT table, and the *same* declaration mechanism backs the engine `system.*` views.
  Read projection; write-through on durable columns is a later refinement.

## Spawned concern (own design + issue)

**Node-type-aware replication factor + placement.** Larger than this view (which only
*surfaces* `node_type`); needs its own design doc + GitHub issue. Two facets, both
resting on the fact that **permanent nodes have inherently more reliable uptime**:

- **Durability** — RF is maintained over the `permanent` set; a `permanent` node
  leaving moves RF and triggers re-replication, an `ephemeral` node leaving does not.
  `ephemeral` nodes hold only uncounted **locality** replicas.
- **Placement stability** — the existing **rendezvous placement** (`hash(partition,
  member)` keeping the top-`replication_factor` members, `rendezvous_placement.hpp`)
  reshuffles owners whenever the member set changes. Restricting the placement
  **universe to permanent members** means `ephemeral` join/leave causes **zero**
  re-placement — no cascading data movement across the cluster on ephemeral churn.
  Ephemeral nodes sit *outside* the RF/placement universe entirely.

Grounding: today `ReplicateToAllNodes` is the only `ReplicationPolicy` (every node
holds all state); `replication_factor` + rendezvous placement exist for datasets but
place over *all* members. The change = make the placement universe the permanent set.
Boundary: control-plane + small state stays `ReplicateToAllNodes` on every node
(incl. ephemeral); only the **data plane** (partitioned datasets, large-object bodies)
is RF/placement over permanent.

## Open questions (implementation detail — settle while building)

- **Ephemeral resolver seam** — how a `FROM <source>` binds to the telemetry cache /
  peer table (a registry of named resolvers?), and the exact liveness thresholds
  (short grace → `unreachable`, long grace → `gone`, telemetry TTL).
- **Column-tier nullability** — replication/telemetry columns are null for a node we
  have no live signal from; confirm null (not zero) and that `directly_connected` /
  `status` gate their interpretation.
- **Persistence cadence** — is the "persist the CRDT every 1–5 min" an explicit knob,
  or just the existing coalesced-on-change persistence (which already batches)?
- **Write-through** — allow `UPDATE` on durable columns to rewrite the CRDT base, or
  keep the virtual table read-only (durable writes go to the base table directly)?
