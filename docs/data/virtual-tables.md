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
Aggregates and `GROUP BY` fold the composed rows through the same
`GroupAccumulator` every non-columnar path uses (#1218). The declared column list
counts as the table's columns, so a declared column no row carries yet is
`null`, not an error; an undeclared one no row carries is.

## Who may declare and read one (#1392)

A virtual table returns raw replicated values, before any table or column
grant, row-level security policy or the connection-store refusal could apply.
So it is an admin's tool, like `system.nodes` (#766):

- **Declaring** one needs flat `sql:ddl` **and** `cluster:admin`. The writer
  role (`sql:ddl` alone) cannot.
- **Reading** one needs `cluster:admin`. The trusted local session (the library,
  the headed app, tests) counts as admin, as it does for the system tables.
- **Reserved families are never projected**, by anyone, admin and local
  included: `row:`, `tbl:`, `db:`, `cols:`, `part:`, `mfst:` (table data and
  structure), `lo:` (large objects), `user:`, `role:`, `token:`, `user_role:`,
  `key:`, `principal:`, `policy:`, `audit:`, `node_revocation:` (identities,
  credentials and the access-control registry; `node_revocation:` since #1320), `record_collection:`, `object_bucket:`,
  `object_lease:`, `metric:` (families with their own scoped grants) and
  `vtable:`. A prefix reaches a family when either starts with the other, so
  `'r'` and `'row:abc'` are refused as well as `'row:'`. Those families are read
  through their own surfaces (SQL on the table, the system tables, the record and
  object capabilities), which apply their grants and strip secret material.
- A definition over a reserved family that already exists (declared before
  this rule, or replicated from an older node) is refused at `SELECT`.

The list is `kReservedVirtualTableFamilies` in `src/core/query/executor.cpp`.
Tests: `tests/governance/test_virtual_table_access.cpp`.

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
- **Per-row governance** over virtual tables (row or column scoping for a
  non-admin reader). Until then only an admin reads one (#1392, above).

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
  needs-a-CRDT + shadow rejections); `tests/governance/test_virtual_table_access.cpp`
  (admin-only declare and read, reserved families, #1392).
