# Virtual tables — `CREATE VIRTUAL TABLE` (#809)

A **virtual table** is a user-declared, read-only SQL table whose rows are
**computed at query time from the replicated CRDT document** — one row per CRDT
entry under a key-family prefix. It is the user-facing generalization of the
built-in [`system.nodes`](query.md) primitive (a virtual table over the
node-list CRDT): the same three-tier, composed-at-query-time model, now
declarable over *any* CRDT key-family.

This is **slice 3** of #809. Slices 1–2 built the internal primitive
(`system.nodes` as a virtual table over `node:<id>`); this slice adds the DDL so
a user can declare their own. The design spec is `docs/plans/virtual-tables.md`
(on the design branch).

## The defining invariant — a virtual table extends a CRDT table

A virtual table **MUST** extend a CRDT key-family: the **durable tier is
mandatory** — it is the convergent row identity. The ephemeral tiers are
enrichment only. `OVER CRDT '<prefix>'` is therefore **required**; there is no
way to declare a virtual table without it.

## Grammar

```sql
CREATE VIRTUAL TABLE <name> OVER CRDT '<prefix>' (
  <col> <type> DURABLE KEY,      -- the CRDT key: the id after the prefix
  <col> <type> DURABLE,          -- a field of the CRDT entry: <prefix><id>.<col>
  <col> <type> EPHEMERAL LOCAL,  -- computed from this node's state (null for now)
  <col> <type> EPHEMERAL SYNCED  -- gossiped/telemetry (null for now)
);
```

Example (the built-in `system.nodes`, expressed as a user DDL):

```sql
CREATE VIRTUAL TABLE my_nodes OVER CRDT 'node:' (
  node_id  TEXT DURABLE KEY,     -- node:<id>            (the id)
  address  TEXT DURABLE,         -- node:<id>.address
  status   TEXT EPHEMERAL LOCAL  -- null until a resolver is bound
);
```

Then `SELECT * FROM my_nodes` returns one row per `node:<id>` entry. Keywords
are case-insensitive; a `type` (e.g. `TEXT`) is captured but not enforced (the
storage is schemaless JSON), exactly like `CREATE TABLE`.

### Tier keywords

| Tier | Meaning | This slice |
|---|---|---|
| `DURABLE` | A field of the CRDT entry (`<prefix><id>.<col>`), replicated. | **projected** from the entry JSON (null when the field is absent) |
| `DURABLE KEY` | The CRDT key — the id after the prefix. Exactly one is required. | the id, e.g. `node:abc` → `abc` |
| `EPHEMERAL LOCAL` | Computed from this node's in-memory state, not synced. | emitted **null** (resolver TODO) |
| `EPHEMERAL SYNCED` | Gossiped/telemetry, TTL'd, not persisted. | emitted **null** (resolver TODO) |

## Read model — read-only

A virtual table is **read-only**: `INSERT` / `UPDATE` / `DELETE` / `TRUNCATE`
targeting one are rejected with a clear error. Durable writes go to the base
entity's own write path (e.g. a node updates its own `node:<id>` entry), not
through the virtual-table view.

`SELECT` reuses the ordinary `WHERE` + projection engine
(`select_over_rows`), so filtering and column projection work as on any table.
`GROUP BY` over a virtual table is not supported (it routes through the numeric
DDF engine, which the composed rows don't feed).

## Naming + resolution

Virtual tables have **global, unqualified names** (`SELECT * FROM my_nodes`, not
`db.my_nodes`). At resolution, a real table in the current database **takes
precedence** — a virtual table never shadows one. `CREATE VIRTUAL TABLE`
validates the name against that precedence up front: it rejects a name that
collides with a system table (or the `zetta_nodes` alias) or with an existing
table in the current database, and rejects a duplicate virtual-table name. The
definition requires **exactly one `DURABLE KEY`** column.

## Persistence — durable, replicated catalog metadata

The definition is stored under the `vtable:<name>` CRDT key
(`src/core/catalog.hpp`), keyed by its (globally unique) name:

```
vtable:<name> -> { name, prefix, columns:[{ name, type, tier, key }] }
```

Because it lives in the CRDT document, it **converges** across the cluster and
**survives restart** (the document is persisted to disk and reloaded), exactly
like `db:` / `tbl:`. There is **no in-memory projection to keep in sync**: the
definitions *and* their rows are read live from the replicated snapshot at query
time (`Node::find_virtual_table` / `Node::virtual_table_rows`), so a peer's
`CREATE VIRTUAL TABLE` is visible with no reconcile pass — the same discipline
`system.nodes` uses to read the node-list.

A virtual table fundamentally requires a CRDT to project over, so it needs a
replicated node: on a standalone (non-replicated) node, `CREATE VIRTUAL TABLE`
is rejected.

## Boundaries

- **No new wire/JSON format beyond the `vtable:` catalog value.** The value is
  parsed at the boundary by the non-throwing `zetta::data::virtual_table_from_json`
  (`include/zetta/data/virtual_table.hpp`); a structurally invalid record is
  skipped, never trusted.

## Deferred (follow-ups)

- **Ephemeral resolvers** — binding an `EPHEMERAL LOCAL` / `EPHEMERAL SYNCED`
  column to a live source (e.g. `... EPHEMERAL LOCAL FROM <source>`). Until then
  ephemeral columns emit `null`. This is the main deferred piece.
- **`DROP VIRTUAL TABLE`** DDL (the engine already has `Node::drop_virtual_table`).
- **A `system.virtual_tables` catalog view** over `Node::virtual_tables()`.
- **A registered draft-07 boundary schema** for the `vtable:` value (currently a
  hand-written non-throwing `from_json`).
- **Governance** (RLS / column scoping) over virtual tables.

## Code

- `include/zetta/data/virtual_table.hpp` · `src/core/data/virtual_table.cpp` —
  the `zetta::data::VirtualTable` value type + tier enum + JSON (de)serialization.
- `src/core/catalog.hpp` — the `vtable:<name>` key family.
- `include/zetta/query/ast.hpp` · `src/core/query/postgres.cpp` — the
  `CreateVirtualTableStmt` AST + the `CREATE VIRTUAL TABLE … OVER CRDT …` parser.
- `src/core/query/executor.cpp` — DDL execution, name validation, SELECT
  projection, and the read-only write rejection.
- `src/core/node.cpp` — `create_virtual_table` / `find_virtual_table` /
  `virtual_tables` / `virtual_table_rows` / `drop_virtual_table`.
- Tests: `tests/data/test_query.cpp` (parse, project, read-only, restart, the
  needs-a-CRDT + shadow rejections).
