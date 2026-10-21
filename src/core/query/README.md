# Query implementation

System tables are computed from live sources. Audit rows read segment records;
`system.nodes` exposes the audit protocol and compatibility, while
`system.node_metrics` exposes local per-origin watermarks and estimated audit
memory. The existing SQL capability serves these over REST and MCP (#1164).
`system.runs` and `system.models` include denormalized Project/Pipeline names;
model rows also expose the explicit producing `run_id` (#1184).
See [audit querying](../../../docs/governance/audit-trail.md#querying).

`SELECT` answers the same way on every path (#1218). An aggregate needs no
`GROUP BY` (the whole table is one group, one row even over nothing); a `GROUP BY`
key may be any type. Dense-numeric grouping runs columnar through the DDF engine
(`execute_grouped_ddf`); everything else folds rows through `GroupAccumulator`
(`distributed_select.cpp`), the accumulator a partition owner's scan uses. A
table's columns are the keys its rows carry (`ColumnCatalog` in `executor.cpp`):
naming another is an error, and `SELECT *` reports them as `columns`. Owners
report theirs as `columns_seen`. Parse errors carry the token's position
(`postgres.cpp`). See [the query layer](../../../docs/data/query.md).

`WHERE` is a predicate tree (#912): comparisons under `AND` / `OR` / `NOT`,
parsed with SQL precedence by `parse_where_clause` (`postgres.cpp`) for SELECT,
UPDATE and DELETE, and evaluated with three-valued logic by `row_matches`
(`predicate.cpp`), the one evaluator every path uses (the row scan, UPDATE and
DELETE, and a partition owner's scan). `where_columns` lists every column it
reads, for the unknown-column and column-grant checks. A partition scan request
carries the tree as nested `and` / `or` / `not` objects. The DDF path takes one
comparison or an AND of them; anything else folds through the rows.

`system.datasets` has a `source_format` column: what the rows were loaded from,
recorded on the table at create time (#1221). See
[system.datasets](../../../docs/data/system-datasets.md).

Every data-changing statement checks its right in the executor (#1229): INSERT,
UPDATE, DELETE and TRUNCATE need `sql:write`, CREATE/DROP DATABASE and TABLE
`sql:ddl`, at table or database scope (DELETE, TRUNCATE and table DDL need the
whole table); CREATE VIRTUAL TABLE needs flat `sql:ddl`; CREATE USER and CREATE
TOKEN need `users:manage` / `tokens:manage`. A legacy AccessKey needs the flat
right. Column grants and row-level security apply to UPDATE and DELETE; TRUNCATE
of a policy-protected table is refused for a non-bypass caller. Each such
statement records a `data_changed` audit row. SQL never reaches the reserved
`zetta_connect` database (#1230). See
[access control](../../../docs/governance/access-control.md#per-statement-enforcement).
`GRANT … ON COLLECTION <prefix>` scopes record-store rights to a collection name
prefix (#1231).

`GRANT … ON BUCKET <prefix>` scopes the object capability's rights to a bucket
name prefix (#1005).
