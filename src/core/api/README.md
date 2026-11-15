# API implementation

Registry declarations generate REST routes, MCP tools and OpenAPI. Pub/sub CRUD
uses the same schemas and handlers on both transports, with coroutine long polls
and no thread per waiter. Successful polling reads share audit sessions and
window totals only when they return data; empty windows add no summary. Writes,
denials and failures stay per-call. The admin-only `purge_audit` action is
offloaded for its segment rewrite and has REST/MCP/OpenAPI parity. See
[CRUD](../../../docs/pubsub/crud.md) and
[REST/OpenAPI](../../../docs/api/rest-openapi.md).

Browser POST registration checks an exact `Origin` before invoking a handler.
Origin refusals close the connection with its body unread. The first middleware
clears cached session/redirect headers on every request, including public GETs.
The auth gate also binds every cookie-authenticated API request to this origin;
GET/HEAD may use same-origin Fetch Metadata or a strictly parsed Referer.
Cookie GET SQL permits only parsed SELECT batches; writes use POST. Explicit
bearer API clients retain their contract. Proxy requirements and failure behavior
are in [registry design](../../../docs/api/registry-design.md#auth-governance-audit--nothing-new)
(#1415).

`node_memory` renders the node's memory-accounting registry through the same
REST/MCP declaration. HTTP sessions, cached parsers, active requests, and
long-poll waiters add their counters to the node's registry while the server
lives. See [memory accounting](../../../docs/ops/memory.md).

The run-metric capabilities share this registry and publish exposed metric
topics. MCP receive and browse waits inspect the first governed result directly;
neither path holds a thread or changes non-object request error handling.
See [run metrics](../../../docs/ml/run-metrics.md).

The ten Study capabilities publish the accepted M-1 search space and validate
and normalize a candidate without mutation. `create_study` uses the same path
for executable objectives, datasets, Pipeline references, and canonical search
controls before admission. They also
include `delete_study`, a separately permissioned action that refuses running
studies, removes non-promoted trial Runs and their metrics, and preserves the
promoted Model and producing Run. REST, MCP and OpenAPI share the registry
declarations (#1205, #1207). Objective metrics use the canonical held-out
names with aliases accepted on input, and `create_study` errors carry `path`
and offer `from_defaults` (#1217). Every input-schema rejection carries the
field at fault in `path`, and a capability's `invalid_input_hint` may add a
way past it. See
[AutoML studies](../../../docs/ml/studies.md).

`create_agent` and `update_agent` take a closed `definition` schema, and an
unknown property is reported at the property itself, so `{path, content}` is
refused at `/definition/files/0/content` in lake's words (#1220).
`agent_run` stores the plan it compiles proposed and answers `not_approved` without
running it; it never approves. Given `project` + `graph_name` naming a stored plan, it
runs it only if approved, checked before the agent's model loads (#913).
`upload_dataset` refuses unsafe table and database names, ragged CSV, and a taken
name (`conflict`) unless `if_exists: "rename"`, with lake's rules and wording;
`get_dataset` accepts `database.table`, and dataset summaries carry
`source_format` (#1221). `load_dataset_object` is the job for a file past
`upload_dataset`'s 16 MiB: it loads an object already in a bucket, checks
`objects:read` on the bucket with `authorize_object_route` (`object_routes.hpp`),
and `objects:write` too when `remove_object` asks it to delete the object, and adopts an `AsyncObjectDatasetLoad` into `BackgroundActions` (#1302). See
[add dataset](../../../docs/data/add-dataset.md).
`get_dataset` answers its `columns` typed, `[{name, type, nullable,
null_fraction}]`, in the source's order (the last three null for a column the
caller may not read), and every summary says whether that order
was recorded (`column_order`); `list_datasets` keeps the names (#1021, see
[column order](../../../docs/data/column-order.md)).

The `list_runs`, `list_models`, and `list_studies` reads expose page-ready
display names, metrics and timestamps through one registry definition, keeping
REST, MCP and OpenAPI output in lockstep (#1184).

Dataset-backed scoring and explanation accept text business identifiers and
return them unchanged; their schemas are generated from the registry alongside
the dataset contract (#957).

`native_http_transport.cpp` checks the egress policy before DNS and before every
connection and redirect hop: outbound is denied by default and each refusal is
reported for the `egress_denied` audit event (#75,
[egress policy](../../../docs/governance/egress-policy.md)). `/cli.md` documents
`--egress-allow` and `ZETTA_EGRESS_ALLOW`.

`mcp_docs.cpp` also renders `/cli.md`. Its pub/sub and audit sections document
the dispatch-pool and class-specific retention flags with their environment
variables; the CLI parity test keeps that page synchronized with `src/main.cpp`.

`sql` is still a read capability on the wire (GET `/v1/sql`), but each statement
is authorized inside the executor and every data-changing one is audited as a
`data_changed` action (#1229). Saved connections report `credential`
(set / none / unreadable / plaintext), never the credential (#1230). See
[access control](../../../docs/governance/access-control.md#per-statement-enforcement).
`sql` also accepts a JSON-body POST, so a statement is not bound by the ~4 KiB
request line; its data-changing statements are audited as `data_changed` by the
executor (#1229, #1231). The record store and time-series capabilities
(`record_capabilities.cpp`) are `blocks` entries with `ON COLLECTION` scoped
authorization. The input check never throws: `contentEncoding: base64` is
checked, and a schema the validator cannot compile is an `invalid_input` refusal
(`registry.cpp`, #1231). See [the record store](../../../docs/data/record-store.md) and
[time series](../../../docs/data/time-series.md).

`network_capabilities.cpp` also owns the app-connection credential lifecycle
(#1280). Issuance requires `api:manage_tokens`, builds a connection-owned user
and least-privilege role through `network/app_credential.cpp`, and returns one
plaintext bearer whose salted hash is persisted. Revocation removes the role
assignment and revokes every token for that connection. The issued
identity reaches only its table/database and topic-prefix grants and their
specific tools. See [scoped app access](../../../docs/network/scoped-app-access.md).

The object capability (`object_capabilities.cpp`, #1005) registers the bucket and
lease operations as `blocks` capabilities scoped `ON BUCKET`, and
`object_route_paths()` documents the byte routes in OpenAPI. `http_server.cpp`
serves `/v1/objects/<bucket>/<key>`: PUT, PATCH and GET stream raw bodies a slice
at a time through `ObjectStore` on the blocking pool, lift the per-request body
limit through the connection's parser, and are authorized and audited by
`authorize_object_route` (`object_routes.hpp`); a refusal drains a bounded unread
body so the connection stays usable. The bucket routes (`GET <bucket>`,
`:usage`, `:barrier`, `:acquire`, `:renew`, `:release`) and `DELETE` call the
registry capabilities through `answer_capability`, the second half of the REST
rendering. A retrying `AwaitableRead` waits on REST and MCP without holding a
thread. Every write, lease and barrier answer carries the bucket's
`incarnation`, and the lease and barrier answers carry
`object_log_prefix_durability` (lake#543). The `Zetta-Incarnation` precondition
travels in `ObjectWrite::incarnation` and the barrier's input, and the store
checks it in the same step as the write. See
[the object capability](../../../docs/data/object-store.md).
