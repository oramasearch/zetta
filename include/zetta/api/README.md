# API interfaces

The capability registry is shared by REST and MCP. Read entries can request a
JSON-body POST alias while retaining GET and read classification. Awaitable reads
use transport-owned async events on both transports.
Run-metric and pub/sub CRUD capabilities coexist in this registry; their shared
long-poll path preserves the same owner-thread authorization and final read.
Study declarations include the shared per-methodology search-space projection
used by discovery and the validation/create registry handlers (#1207).
`AuditPolicy` keeps writes, denials and failures per-call while selected
authorised polling reads use data-bearing window summaries. A blocking marker
offloads the internally synchronized `purge_audit` segment rewrite without
moving authorization off the owner thread (#1169).
See [registry design](../../../docs/api/registry-design.md).
