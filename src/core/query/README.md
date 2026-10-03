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

`system.datasets` has a `source_format` column: what the rows were loaded from,
recorded on the table at create time (#1221). See
[system.datasets](../../../docs/data/system-datasets.md).
