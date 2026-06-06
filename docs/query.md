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

## Limitations (today)

Single-equality `WHERE` only; no joins, ordering, aggregation, `UPDATE`,
`DELETE`, or multi-table queries; column types are parsed but not stored or
enforced. These land behind the same AST/executor seam.

## Adding a dialect

1. Implement `Dialect` (`name()` + `parse()`), producing `zetta::query::Statement`s.
2. Construct it in `QueryService` (or make the service take a `Dialect&`).
3. The executor and UI need no changes.
