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
SELECT sku, qty FROM shop.items WHERE qty >= 3;
```

Values: single-quoted strings (`''` escapes a quote), integers, decimals,
optional leading `-`, `true` / `false` / `null`.

### Aggregation (GROUP BY)

A `SELECT … GROUP BY` runs through the **[DDF engine](ddf.md)** (ARC-12 /
API-2): the table is loaded into a typed, columnar `DataFrame`, the `WHERE` is
applied with the DDF `filter` op, and `group_by` computes the per-group
aggregates, materialized back into result rows.

```sql
SELECT segment, SUM(amount), COUNT(*) FROM churn.events GROUP BY segment;
SELECT segment, SUM(amount) AS total, MIN(amount), MAX(amount)
  FROM churn.events WHERE amount >= 10 GROUP BY segment;
SELECT segment, AVG(score) AS avg_score FROM churn.events GROUP BY segment;
SELECT segment FROM churn.events GROUP BY segment;   -- distinct keys (no aggregate)
```

Aggregate functions: `COUNT(*)`, `SUM(col)`, `MIN(col)`, `MAX(col)`,
`AVG(col)` / `MEAN(col)` (mapped to the DDF op set; `AVG` → `mean`). The default
result-column name follows DDF's convention — `count`, `sum_amount`,
`min_amount`, `mean_score`, … — overridable per item with `AS alias`.

## Semantics

- `INSERT` requires an explicit column list (the store is schemaless JSON); each
  `VALUES` tuple becomes one JSON row `{col: value, ...}`.
- `SELECT *` returns whole rows; `SELECT a, b` projects to those keys (missing
  keys come back as `null`).
- `WHERE` supports a single `column <op> value` predicate, where `<op>` is one of
  `=`, `!=` / `<>`, `<`, `<=`, `>`, `>=`. Equality/inequality work over any value;
  the ordering comparisons apply numerically (numbers) or lexicographically
  (strings). A row lacking the column never matches.
- **`GROUP BY`** routes through the DDF engine: it groups by **exactly one
  numeric key column** and aggregates the referenced numeric columns. Because the
  DataFrame is numeric only, the grouping key, every aggregated column, and a
  `WHERE` column must be **dense numeric** fields (present + numeric in every row);
  otherwise the query returns a clear error. Integer columns return whole numbers
  as JSON integers; means and float columns return doubles. Groups come back in
  ascending key order. (Not supported over `system` tables — they are computed,
  non-numeric rows.) Building a new table from a grouped result (`CREATE TABLE AS`)
  is a later slice.
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

A third table is **`zetta_models`** — the [model registry](models.md) (no
cluster needed; sourced from `Node::models()`):

```sql
SELECT * FROM system.zetta_models;
SELECT model_id, kind FROM system.zetta_models WHERE kind = 'classifier';
```

Columns: `model_id`, `name`, `kind`, `project`, `run`, `feature_schema`,
`metrics`, `created`, `has_weights`, `weights_size`, `weights_sha256`,
`weights_present`. The serialized weights body itself isn't in the registry (it's
a large binary on the body seam); its fingerprint and local presence are.
Qualified name only (no bare alias), like `system.zetta_deployments`.

A fourth table is **`zetta_jobs`** — the [compute-job registry](compute-jobs.md)
(no cluster needed; sourced from `Node::jobs()`): one row per unit of native
distributed accelerated compute (ARC-11 / ENT-31) and its lifecycle:

```sql
SELECT * FROM system.zetta_jobs;
SELECT job_id, kind, status FROM system.zetta_jobs WHERE status = 'running';
```

Columns: `job_id`, `kind`, `status`, `project`, `graph`, `run`,
`scaling_policy`, `created`, `updated`, `detail`, `node`. `scaling_policy` is the
optional reward-model reference. `node` is the **observing** node — the node
whose replicated view these rows are read from (jobs replicate, so every node
lists the same set); recording which node actually *runs* a job is a later
slice (the scheduler), so until then `node` is "which node's registry you
queried", not a placement assignment. Qualified name only (no bare alias), like
`system.zetta_deployments`.

Two more tables back the [Cloud Inspector](cloud-inspector.md).
**`system.zetta_cloud_providers`** is the provider catalog as a table — **pure
metadata, it reads nothing from any cloud**:

```sql
SELECT * FROM system.zetta_cloud_providers;
SELECT name, runnable FROM system.zetta_cloud_providers WHERE requires_consent = true;
```

Columns: `provider`, `name`, `category`, `cli`, `requires_consent`, `runnable`,
`config_paths`, `inspect_commands`. **`system.zetta_cloud_status`** is the cached
results of consent-gated inspects (sourced from `Node::cloud_inspections()`):

```sql
SELECT provider, reachable, authenticated, identity FROM system.zetta_cloud_status;
```

Columns: `provider`, `name`, `reachable`, `authenticated`, `identity`, `regions`,
`resources`, `commands_ok`, `commands_total`, `checked_at`, `detail`. **Querying
it never triggers a cloud call** — it only reflects inspects the user already ran
(and consented to) in the Cloud Inspector panel, so it is empty until then; the
cache is ephemeral (in-memory, not replicated). Both are qualified-name only.

Four more tables surface the run + governance state as read views (RULE-7 — the
queryable counterpart of the GUI Runs / Admin panels, so SQL and the GUI never
diverge), each computed at query time from the live `Node` accessors:

- **`system.zetta_runs`** — the run registry (`Node::runs()`): one row per graph
  execution. Columns: `run_id`, `project`, `graph`, `deployment`, `status`,
  `created`, `updated`, `log_count`, `output_count`, `confidence`, `freshness`.
- **`system.zetta_principals`** — the authenticated actors (`Node::principals()`).
  Columns: `principal_id`, `name`, `kind`, `created`, `key_count`.
- **`system.zetta_keys`** — the access keys the principals own. Columns: `key_id`,
  `principal_id`, `principal`, `unrestricted`, `scope`, `capabilities`, `created`.
  `scope` + `capabilities` are readable strings (`project:churn`,
  `orchestration:start_run`, an `@<ttl>` suffix when time-bounded); a key record
  carries no secret material, so this exposes nothing the Admin panel doesn't.
- **`system.zetta_audit`** — the immutable audit trail (`Node::audit_events()`).
  Columns: `audit_id`, `action`, `actor`, `target`, `allowed`, `detail`, `time`.

```sql
SELECT run_id, status FROM system.zetta_runs WHERE status = 'failed';
SELECT action, actor, target FROM system.zetta_audit WHERE allowed = false;
```

All four are qualified-name only.

One more table surfaces the **partition catalog** (see
[dataset-partitions.md](dataset-partitions.md), ENT-36 / TBL-13) — where each
table's data slices live, replicate-to-all so every node sees the same placement:

- **`system.dataset_partitions`** — the partition catalog
  (`Node::dataset_partitions()`): one row per recorded dataset partition. Columns:
  `partition_id`, `table`, `blob_ref`, `row_start`, `row_end`, `row_count`,
  `owner_nodes` (comma-joined), `owner_count`, `stats` (the free-form per-column
  stats object), `created`. Qualified-name only.

```sql
SELECT partition_id, table, owner_count FROM system.dataset_partitions;
SELECT * FROM system.dataset_partitions WHERE table = '<tbl-uuid>';
```

> **Naming.** `system.dataset_partitions` is the **first system table without the
> `zetta_` prefix**: the `system` database is reserved + zetta-controlled, so the
> prefix is redundant. New system tables are named plainly; the legacy `zetta_*`
> tables above are kept until the system-wide rename in
> [#538](https://github.com/oramasearch/zetta/issues/538).

Future system tables slot into the same database — notably **`system.leadership`**
(which exclusive resource is held by which node, with lease/timeout), which is
**planned, not built** (see [replication.md](replication.md) → *Consistency model*).

## Timing

`QueryService::Outcome::elapsed` is the wall-clock time of the whole parse +
execute, measured at nanosecond resolution (`std::chrono::nanoseconds`). The UI
renders it with `zetta::format_duration`, which picks a human unit by
magnitude — ns, µs, ms, s, min, h, d, wk — e.g. `(523 ns)`, `(1.500 ms)`,
`(2.00 wk)`.

## Limitations (today)

`WHERE` is a single-column predicate (no `AND`/`OR`); `GROUP BY` takes exactly
one numeric key (no multi-key grouping, `HAVING`, or aggregating over `system`
tables); no joins, ordering (`ORDER BY`), `UPDATE`, `DELETE`, multi-table queries,
or `CREATE TABLE AS`; column types are parsed but not stored or enforced. These
land behind the same AST/executor seam.

## Adding a dialect

1. Implement `Dialect` (`name()` + `parse()`), producing `zetta::query::Statement`s.
2. Construct it in `QueryService` (or make the service take a `Dialect&`).
3. The executor and UI need no changes.
