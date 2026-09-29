# Query implementation

System tables are computed from live sources. Audit rows read segment records;
`system.nodes` exposes the audit protocol and compatibility, while
`system.node_metrics` exposes local per-origin watermarks and estimated audit
memory. The existing SQL capability serves these over REST and MCP (#1164).
`system.runs` and `system.models` include denormalized Project/Pipeline names;
model rows also expose the explicit producing `run_id` (#1184).
See [audit querying](../../../docs/governance/audit-trail.md#querying).
