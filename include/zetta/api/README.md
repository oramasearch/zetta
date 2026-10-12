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
`ApiError` documents the `conflict` code (HTTP 409), which `upload_dataset`
returns for a taken name (#1221).

`AwaitableRead::retry_within` (#1005) lets an entry keep waiting until `wait_for`
passes, making one governed call per wake; such an entry may be a blocking action
that mutates. `acquire_object_lease` uses it to wait out another holder's lease.
`capabilities.hpp` declares `register_object_capabilities` and
`object_route_paths()`, the OpenAPI rendering of the object byte routes. See
[the object capability](../../../docs/data/object-store.md).
