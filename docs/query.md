# Query layer

zetta is designed to accept queries in **any database format**. It does this by
separating the dialect (the surface syntax) from execution:

```
SQL text ──▶ Dialect.parse() ──▶ generic AST ──▶ Executor ──▶ QueryResult
             (per-format frontend)  (dialect-neutral)  (runs against a Node)
```

- The **generic AST** (`zetta::query`, `ast.hpp`) is a `std::variant` of
  statement structs. It is dialect-neutral — nothing Postgres-specific lives in
  it.
- A **`Dialect`** (`dialect.hpp`) turns dialect text into that AST. The first
  one is **`PostgresDialect`** (`postgres.hpp`). Supporting another format
  (MySQL, SQLite, a custom syntax) means writing another `Dialect` — the
  executor and everything downstream are reused unchanged.
- The **`Executor`** (`executor.hpp`) runs the AST against a `Node`.
- **`QueryService`** (`service.hpp`) is the parse-then-execute facade the UI and
  tests call.

## Supported Postgres subset

Keywords are case-insensitive. `--` line comments and `;`-separated statements
are allowed. Table names may be qualified as `db.table`; unqualified names use
the **current database** (the UI's selected database; the `current_database`
argument to `QueryService::run`).

### DDL

```sql
CREATE DATABASE shop;
DROP DATABASE shop;

CREATE TABLE shop.items (sku text, qty int);   -- column types parsed, not yet enforced
DROP TABLE shop.items;
```

### DML / query

```sql
INSERT INTO shop.items (sku, qty) VALUES ('A1', 5), ('B2', 3);

SELECT * FROM shop.items;
SELECT sku, qty FROM shop.items WHERE sku = 'A1';
```

Values: single-quoted strings (`''` escapes a quote), integers, decimals,
optional leading `-`, `true` / `false` / `null`.

## Semantics

- `INSERT` requires an explicit column list (the store is schemaless JSON); each
  `VALUES` tuple becomes one JSON row `{col: value, ...}`.
- `SELECT *` returns whole rows; `SELECT a, b` projects to those keys (missing
  keys come back as `null`).
- `WHERE` supports a single `column = value` equality.
- Statements run in order; the first failure stops the batch and is reported.

## The `system` database

System info lives in a virtual database named **`system`** (à la Postgres'
`pg_catalog`): a reserved, **read-only** namespace whose tables are *computed at
query time* — not stored, not replicated (each node's view is its own). `system`
can't be created, dropped, or written to, and no real database may be named
`system`.

Its first table is **`zetta_nodes`** — cluster membership + [replication](replication.md)
status — available whenever the `QueryService` was given a `Cluster` (the headed
UI wires this; without a cluster, selecting it returns a clear error):

```sql
SELECT * FROM system.zetta_nodes;
SELECT node_id, address, in_sync FROM system.zetta_nodes WHERE is_self = false;
SELECT * FROM zetta_nodes;   -- bare name still resolves (back-compat alias)
```

Columns: `node_id`, `address`, `role`, `is_self`, `connected`, `entries`,
`version`, `in_sync`, `last_sync_seconds`. `SELECT *`, projection, and a single
`WHERE` equality all work, exactly as for a regular table — the executor just
sources the rows from `Cluster::nodes()` instead of a storage engine. The headed
explorer also lists `system` (marked *virtual*) so its tables are browsable, not
just queryable.

A second table is **`zetta_deployments`** — the recorded graph→target
[placements](deployments.md) (no cluster needed; it is sourced from
`Node::deployments()`):

```sql
SELECT * FROM system.zetta_deployments;
SELECT graph_name, node_count FROM system.zetta_deployments
  WHERE target_kind = 'local';
```

Columns: `deployment_id`, `graph_name`, `target_kind`, `binding`,
`credentials_ref`, `node_id`, `node_count`, `edge_count`, `created`. (Unlike
`zetta_nodes`, there is **no** bare-name alias — use the qualified
`system.zetta_deployments`.)

Future system tables slot into the same database — notably **`system.zetta_leadership`**
(which exclusive resource is held by which node, with lease/timeout), which is
**planned, not built** (see [replication.md](replication.md) → *Consistency model*).

## Timing

`QueryService::Outcome::elapsed` is the wall-clock time of the whole parse +
execute, measured at nanosecond resolution (`std::chrono::nanoseconds`). The UI
renders it with `zetta::format_duration`, which picks a human unit by
magnitude — ns, µs, ms, s, min, h, d, wk — e.g. `(523 ns)`, `(1.500 ms)`,
`(2.00 wk)`.

## Limitations (today)

Single-equality `WHERE` only; no joins, ordering, aggregation, `UPDATE`,
`DELETE`, or multi-table queries; column types are parsed but not stored or
enforced. These land behind the same AST/executor seam.

## Adding a dialect

1. Implement `Dialect` (`name()` + `parse()`), producing `zetta::query::Statement`s.
2. Construct it in `QueryService` (or make the service take a `Dialect&`).
3. The executor and UI need no changes.
