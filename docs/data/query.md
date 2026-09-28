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

### Ordering and paging (ORDER BY, LIMIT, OFFSET)

```sql
SELECT account, risk FROM churn.accounts ORDER BY risk DESC, account LIMIT 10;
SELECT * FROM shop.items ORDER BY sku OFFSET 20 LIMIT 10;   -- either order; LIMIT ALL = none
SELECT segment, COUNT(*) AS n FROM churn.events GROUP BY segment ORDER BY n DESC LIMIT 1;
```

They apply to the finished result of every path (a plain scan, a `GROUP BY`, a `system`
table, a partition-only table scattered to its owners), in that order: sort, skip `OFFSET`,
keep `LIMIT`. An `ORDER BY` key names a column of the result — a selected column, a
`GROUP BY` key or an aggregate's alias (any column with `SELECT *`); another name is an
error that says so. Numbers sort numerically, then booleans, then strings; `NULL` sorts
last ascending and first descending, as in Postgres; ties keep the next key's order, then
scan order. The whole result is still read before it is cut: `LIMIT` bounds what is
returned, not what is scanned.

### Aggregation (GROUP BY)

A `SELECT … GROUP BY` runs through the **[DDF engine](../ddf/ddf.md)** (ARC-12 /
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
- A `SELECT` over a **[partition-only table](partition-only-tables.md)** is not
  answered from the local store at all: it is
  **[scattered to the partition owners and merged](scatter-gather-select.md)**,
  with `WHERE` + projection pushed near the data, aggregates merged rather than
  concatenated, and an unreachable owner failing the query. The SQL is identical
  and the result columns are named identically — only the path differs.
- Statements run in order; the first failure stops the batch and is reported.

## The `system` database

System info lives in a virtual database named **`system`** (à la Postgres'
`pg_catalog`): a reserved, **read-only** namespace whose tables are *computed at
query time* — not stored, not replicated (each node's view is its own). `system`
can't be created, dropped, or written to, and no real database may be named
`system`.

**`system.tables`** is the catalog of the system tables themselves (#684): one
row per table with its `table` name and one-line `description`, alphabetical,
itself included. "Which system tables exist" is observable state, so it is a
read view like everything else (RULE-7):

```sql
SELECT * FROM system.tables;
SELECT description FROM system.tables WHERE table = 'nodes';
```

The list is the executor's own dispatch catalog (`kSystemTableCatalog` in
`src/core/query/executor.cpp`, exposed as
`zetta::query::system_table_names()`), so it can't drift from what actually
serves: the headed explorer's `system` table list, the unknown-system-table
error's suggestions, and this view all render from the one array. A test
asserts every cataloged name dispatches.

Its first table is **`nodes`** — the cluster's **FULL membership** as a **virtual
table over the replicated node-list CRDT** (#809): one row per `node:<id>` the
document knows, **including nodes this node holds no direct link to**, because the
node-list CRDT converges on all N members from any node (the whole cluster, not
just direct peers — no "5 not 9" undercount on a spanning mesh of many k8s
clusters + hosts). Its rows carry the durable node-list entry overlaid with this
node's live [replication](../cluster/replication.md) status, and it is available
whenever the `QueryService` was given a `Cluster` (the headed UI wires this;
without a cluster, selecting it returns a clear error):

```sql
SELECT * FROM system.nodes;
SELECT node_id, address, in_sync FROM system.nodes WHERE is_self = false;
SELECT node_id, reachability FROM system.nodes WHERE directly_connected = false;
SELECT * FROM zetta_nodes;   -- the historical bare spelling still resolves
```

There is **no bare `nodes` alias** (it would shadow a user table named `nodes`);
only the pre-rename `zetta_nodes` spelling resolves unqualified, and every
legacy `system.zetta_<table>` name resolves to its plain `system.<table>`:

```sql
SELECT * FROM system.zetta_nodes;   -- back-compat alias for system.nodes
```

The rows are **composed at query time from tiers** — the durable node-list entry
overlaid with ephemeral, observed state that is **never written back to the CRDT**:

- **durable** (from the replicated `node:<id>` entry): `node_id`, `address` (its
  dialable address; **empty** for a dial-out-only node), `reachability`
  (`wan-accessible` | `dial-out-only`), and `node_type` (`permanent` | `ephemeral`
  — the node's self-declared durability class, set with `--node-type`; see
  [../cluster/cluster.md](../cluster/cluster.md)).
- **ephemeral, local to this node**: `is_self`, `directly_connected` (whether a
  live link to it is held right now), `status` (`live` | `unreachable` — a `gone`
  state + GC of departed node-list entries is a later slice), and `last_seen_seconds`
  (`null` when this node has never seen that member).
- **ephemeral, synced/observed**: replication status — `entries`, `version`,
  `in_sync` (its version equals ours), `last_sync_seconds` — known for **self +
  direct peers**, plus liveness overlaid from the TTL'd telemetry plane.

`SELECT *`, projection, and a single `WHERE` equality all work, exactly as for a
regular table — the executor composes the rows from the node-list CRDT (the durable
tier) plus the live `Cluster` (the ephemeral tiers) instead of a storage engine, and
the ephemeral overlays are recomputed per query, never captured back into the
document. The headed explorer also lists `system` (marked *virtual*) with every
table from `system_table_names()`, so all of them are browsable, not just queryable
([gui/explorer-system-tables.md](../gui/explorer-system-tables.md), #684).

A second table is **`deployments`** — the recorded graph→target
[placements](../pipeline/deployments.md) (no cluster needed; it is sourced from
`Node::deployments()`):

```sql
SELECT * FROM system.deployments;
SELECT graph_name, node_count FROM system.deployments
  WHERE target_kind = 'local';
```

Columns: `deployment_id`, `graph_name`, `target_kind`, `binding`,
`credentials_ref`, `node_id`, `node_count`, `edge_count`, `created`. (There is
no bare-name alias — use the qualified `system.deployments`.)

A third table is **`models`** — the [model registry](../ml/models.md) (no
cluster needed; sourced from `Node::models()`):

```sql
SELECT * FROM system.models;
SELECT model_id, kind FROM system.models WHERE kind = 'classifier';
```

Columns: `model_id`, `name`, `kind`, `project`, `run`, `feature_schema`,
`metrics`, `created`, `has_weights`, `weights_size`, `weights_sha256`,
`weights_present`, `exposed`, `endpoint`. The serialized weights body itself
isn't in the registry (it's a large binary on the body seam); its fingerprint
and local presence are. `exposed` / `endpoint` are the governed serving state
([../api/serving.md](../api/serving.md), #648). Qualified name only (no bare
alias), like `system.deployments`.

A fourth table is **`jobs`** — the [compute-job registry](../compute/compute-jobs.md)
(no cluster needed; sourced from `Node::jobs()`): one row per unit of native
distributed accelerated compute (ARC-11 / ENT-31) and its lifecycle:

```sql
SELECT * FROM system.jobs;
SELECT job_id, kind, status FROM system.jobs WHERE status = 'running';
```

Columns: `job_id`, `kind`, `status`, `project`, `graph`, `run`,
`scaling_policy`, `created`, `updated`, `detail`, `node`. `scaling_policy` is the
optional reward-model reference. `node` is the **observing** node — the node
whose replicated view these rows are read from (jobs replicate, so every node
lists the same set); recording which node actually *runs* a job is a later
slice (the scheduler), so until then `node` is "which node's registry you
queried", not a placement assignment. Qualified name only (no bare alias), like
`system.deployments`.

Two more tables back the [Cloud Inspector](../cloud/cloud-inspector.md).
**`system.cloud_providers`** is the provider catalog as a table — **pure
metadata, it reads nothing from any cloud**:

```sql
SELECT * FROM system.cloud_providers;
SELECT name, runnable FROM system.cloud_providers WHERE requires_consent = true;
```

Columns: `provider`, `name`, `category`, `cli`, `requires_consent`, `runnable`,
`config_paths`, `inspect_commands`. **`system.cloud_status`** is the cached
results of consent-gated inspects (sourced from `Node::cloud_inspections()`):

```sql
SELECT provider, reachable, authenticated, identity FROM system.cloud_status;
```

Columns: `provider`, `name`, `reachable`, `authenticated`, `identity`, `regions`,
`resources`, `commands_ok`, `commands_total`, `checked_at`, `detail`. **Querying
it never triggers a cloud call** — it only reflects inspects the user already ran
(and consented to) in the Cloud Inspector panel, so it is empty until then; the
cache is ephemeral (in-memory, not replicated). Both are qualified-name only.

Five more tables surface run, governance, and solution state as read views (RULE-7 — the
queryable counterpart of the GUI Runs / Admin panels, so SQL and the GUI never
diverge), each computed at query time from the live `Node` accessors:

- **`system.runs`** — the run registry (`Node::runs()`): one row per graph
  execution. Columns: `run_id`, `project`, `graph`, `deployment`, `status`,
  `created`, `updated`, `log_count`, `output_count`, `confidence`, `freshness`.
- **`system.principals`** — the authenticated actors (`Node::principals()`).
  Columns: `principal_id`, `name`, `kind`, `created`, `key_count`.
- **`system.keys`** — the access keys the principals own. Columns: `key_id`,
  `principal_id`, `principal`, `unrestricted`, `scope`, `capabilities`, `created`.
  `scope` + `capabilities` are readable strings (`project:churn`,
  `orchestration:start_run`, an `@<ttl>` suffix when time-bounded); a key record
  carries no secret material, so this exposes nothing the Admin panel doesn't.
- **`system.audit`** — the immutable audit trail (`Node::audit_events()`).
  Columns: `audit_id`, `action`, `actor`, `target`, `allowed`, `detail`, `time`.
- **`system.ledger`** — the immutable, replicated solutions reward ledger
  (`Node::ledger_entries()`). Action rows carry score, decision, action, actor,
  versions, experiment cell, cost, currency, and timestamps. Outcome rows carry
  their referenced `action_id`, outcome value, source, currency, and timestamps.
  Every row also carries `ledger_id` and `kind`. See
  [audit-ledger.md](../solutions/audit-ledger.md).

```sql
SELECT run_id, status FROM system.runs WHERE status = 'failed';
SELECT action, actor, target FROM system.audit WHERE allowed = false;
SELECT * FROM system.ledger WHERE subject = 'account-42';
```

All five are qualified-name only.

Layered over those sources is **`system.activity`** — the merged, newest-first
activity feed (#622; [../gui/activity-feed.md](../gui/activity-feed.md)): the
audit trail + runs composed chronologically at query time by
`zetta::observability::activity_entries`, each row with who asked (actor +
surface: `local` / `console` / `api`), what for (action + target), and how it
went (outcome: `ok` / `denied` / `failed` / `info`). The exact feed the headed
Activity view renders (RULE-7), named plainly (no `zetta_` prefix):

```sql
SELECT * FROM system.activity;
SELECT time, surface, actor, action, target FROM system.activity WHERE surface = 'api';
```

Columns: `time`, `surface`, `actor`, `action`, `target`, `outcome`, `detail`,
`ref_kind`, `ref_id` (the owning entity for deep links: `run` / `job` / `graph`
/ `deployment` / `audit`). Qualified-name only.

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

- **`system.datasets`** — datasets as a first-class primitive
  ([system-datasets.md](system-datasets.md), #585), sourced from `Node::datasets()`.
  One row per dataset (= a table + its data), computed live from the tree + the
  partition catalog. Columns: `dataset` (= the table name), `database`, `table_id`
  (joins to `system.dataset_partitions.table`), `format` (the on-disk storage format
  — `jsonl` today), `replication` (the replication strategy), `rows`, `size_bytes`,
  `columns` (inferred schema, comma-joined), `column_count`, `partitioned`,
  `partition_count`, `owner_nodes`, and `placement` (per-partition
  `[start,end)@owner1|owner2`, joined with `, ` — which node owns which slice).
  Qualified-name only.

```sql
SELECT dataset, rows, size_bytes, format FROM system.datasets;
SELECT dataset, owner_nodes, placement FROM system.datasets WHERE partitioned = true;
```

There is also **`system.checkpoints`** — burst-job eviction tolerance
([job-checkpoint.md](../compute/job-checkpoint.md), RULE-5 / #468), sourced from
`Node::checkpoints()` (the latest checkpoint per job in this node's **local** store).
Columns: `job_id`, `kind`, `sequence` (units completed = the resume point), `total`,
`progress` (`sequence / total`), `created`. Unlike the catalog above, checkpoints are
**ephemeral + local + not replicated** (like `system.cloud_status`): the view
has no CRDT backing and reflects only what is durably resumable on this node.
Qualified-name only.

```sql
SELECT job_id, kind, sequence, progress FROM system.checkpoints;
```

There is also **`system.node_metrics`** — this node's **live host resource
utilization** ([node-metrics.md](../cluster/node-metrics.md), #575 area 2), sourced from
`Node::sample_node_metrics()` (CPU / memory / GPU + VRAM / storage / network,
sampled from OS counters at query time). A node measures only its own host, so it
returns exactly **one row** — the node the query hits. Columns: `node_id`,
`cpu_cores`, `cpu_percent`, `memory_used_bytes`, `memory_total_bytes`,
`memory_percent`, `gpu_present`, `gpu_percent`, `vram_used_bytes`,
`vram_total_bytes`, `storage_used_bytes`, `storage_total_bytes`,
`storage_percent`, `net_rx_bytes_per_sec`, `net_tx_bytes_per_sec`, `net_percent`,
`sampled_at`. Counters a host can't report are `null` (graceful degradation — the
GPU group is absent in a CPU-only core build). Like `system.checkpoints` /
`system.cloud_status` it is **ephemeral, observed, NOT replicated** (no CRDT
backing). Qualified-name only.

```sql
SELECT node_id, cpu_percent, memory_percent FROM system.node_metrics;
```

There are also two **live pipeline instrumentation** views
([pipeline-instrumentation.md](../compute/pipeline-instrumentation.md), #589 / #575 area 5 —
the engine source the GUI's algorithm-at-work animation reads from), both
**ephemeral, observed, NOT replicated** (no CRDT backing) like `system.node_metrics`:

- **`system.pipeline_jobs`** — one row per recorded **near-data** compute job
  (sourced from `Node::pipeline_jobs()`, a bounded in-memory ring the near-data
  runners record into). Columns: `job_id`, `kind` (`group_by` / `featurize` /
  `train`), `node`, `partitions_read`, `rows_read`, `local_bytes_read` (bytes read
  near data, not moved), `wire_bytes_moved` (the collective-plane partials),
  `status`, `recorded_at`. Empty until a near-data job runs. Qualified-name only.
- **`system.collective_rounds`** — exactly **one row** (this node), the live
  [job coordinator](../compute/job-coordinator.md)'s collective-plane progress. Columns:
  `node`, `attached` (a coordinator is attached), `participants`,
  `rounds_completed` (allreduce/broadcast/barrier/gather that succeeded),
  `wire_bytes_moved` (cumulative). Ticks up during any distributed run; a
  standalone node reports `attached=false` with zeroed meters. Qualified-name only.

```sql
SELECT job_id, kind, local_bytes_read, wire_bytes_moved FROM system.pipeline_jobs;
SELECT rounds_completed, wire_bytes_moved FROM system.collective_rounds;
```

There are also three **observability** views
([observability.md](../architecture/observability.md), #640), all
**ephemeral, observed, NOT replicated** and qualified-name only:

- **`system.spans`** — one row per span in this node's span ring
  (`Node::span_recorder()`), oldest first: every traced operation's four
  captures. Columns: `trace_id`, `span_id`, `parent_span_id`, `name`,
  `started_at`, `duration_ms`, `ok`, `error`, plus `intent` / `plan` /
  `result_shape` surfaced as first-class columns and the full `attributes`
  object. The same ring the `list_traces` / `get_trace` capabilities read.
- **`system.traces`** — one row per trace aggregated from the same ring: the
  root span's `name` + `intent`, `root_span_id`, earliest `started_at`, the
  root's `duration_ms`, `span_count`, `ok` (every span succeeded), `error`
  (the first failing span's reason). The "what happened" summary the GUI
  renders.
- **`system.crashes`** — one row per crash report under `<data_dir>/crashes/`
  ([crash reports](../architecture/observability.md), #640): what a dying node
  wrote on a fatal signal. Columns: `id`, `signal_number`, `signal_name`,
  `crashed_at`, `version`, `platform`, `frame_count`, `stacktrace`,
  `recent_spans`, plus `path` / `valid` / `error` — a report file that fails
  schema validation still gets a row (`valid=false` + the reason), never a
  silent skip. Local files read at query time (the checkpoints precedent).

```sql
SELECT name, intent, duration_ms FROM system.traces WHERE ok = false;
SELECT name, duration_ms FROM system.spans WHERE trace_id = '<id>';
SELECT signal_name, crashed_at, version FROM system.crashes;
```

> **Naming.** System tables are named **plainly, without a `zetta_` prefix**: the
> `system` database is reserved + zetta-controlled, so the prefix is redundant.
> The legacy tables that originally shipped as `system.zetta_<table>` were renamed
> in [#538](https://github.com/oramasearch/zetta/issues/538); the old spellings
> (`system.zetta_nodes`, `system.zetta_jobs`, …) still resolve as **back-compat
> aliases** for at least one cycle, as does the historical bare `zetta_nodes`.

Future system tables slot into the same database — notably **`system.leadership`**
(which exclusive resource is held by which node, with lease/timeout), which is
**planned, not built** (see [replication.md](../cluster/replication.md) → *Consistency model*).

## Timing

`QueryService::Outcome::elapsed` is the wall-clock time of the whole parse +
execute, measured at nanosecond resolution (`std::chrono::nanoseconds`). The UI
renders it with `zetta::util::format_duration`, which picks a human unit by
magnitude — ns, µs, ms, s, min, h, d, wk — e.g. `(523 ns)`, `(1.500 ms)`,
`(2.00 wk)`.

## Limitations (today)

`WHERE` is a single-column predicate (no `AND`/`OR`); `GROUP BY` takes exactly
one numeric key (no multi-key grouping, `HAVING`, or aggregating over `system`
tables); `ORDER BY` takes result columns only (no expressions); no joins,
`UPDATE`, `DELETE`, multi-table queries, or `CREATE TABLE AS`; column types are
parsed but not stored or enforced. These land in the same AST and executor.

## Adding a dialect

1. Implement `Dialect` (`name()` + `parse()`), producing `zetta::query::Statement`s.
2. Construct it in `QueryService` (or make the service take a `Dialect&`).
3. The executor and UI need no changes.
