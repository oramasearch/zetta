# API tests

Real loopback HTTP tests cover REST and MCP. Pub/sub CRUD parity compares registry,
OpenAPI and tools/list schemas and calls every operation through each transport.
Long-poll tests exercise both transports while owner work continues and assert
that their reads share an audit session instead of creating one row per return.
The SQL parity path also compares pub/sub dispatch metrics from `system.pubsub_topics`
and `system.node_metrics` over REST and MCP.
Segment-backed `system.audit` rows, the audit protocol metric, admin-only
`purge_audit` REST/MCP parity, empty-window suppression, and the
`system.memory` audit row have REST/MCP SQL parity coverage (#1164, #1169).
The core registry contains 129 capabilities, including the `node_memory` read,
`purge_audit`, and the separately permissioned `delete_study` action.
Its parity test validates both REST and MCP values against one output schema and
compares their subsystem ordering and exact replication byte/item values
(#1167). Both parity suites remain
required, alongside the MCP malformed-request and receive/browse long-poll
regressions.
Page-list parity creates linked Project, Pipeline, Run, Model, and Study records,
then compares `list_runs`, `list_models`, and `list_studies` values across REST
and MCP (#1184).
Solution creation parity confirms that REST and MCP both retain policy predicate
evidence on the solution manifest without adding it to methodology step configs
(#1197).
The server-level Study journey creates M-1 studies through the registry over a
connector-shaped churn table, verifies real dataset and Pipeline trial Runs,
restart recovery, curves, analysis and promotion, and confirms `stop_study`
cancels the active training Run. The same journey checks canonical wall-time and
cost stop reasons without admitting excess trials (#1205).
The S-1 solution journey also runs CL-1 and CL-4 over text customer identifiers,
ISO observation times and boolean labels, checking decision tables, ledger rows
and webhook payloads (#957).
See [CRUD verification](../../docs/pubsub/crud.md#verification).
