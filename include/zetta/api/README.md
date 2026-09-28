# API interfaces

The capability registry is shared by REST and MCP. Read entries can request a
JSON-body POST alias while retaining GET and read classification. Awaitable reads
use transport-owned async events on both transports.
Run-metric and pub/sub CRUD capabilities coexist in this registry; their shared
long-poll path preserves the same owner-thread authorization and final read.
See [registry design](../../../docs/api/registry-design.md).
