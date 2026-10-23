# API docs

The programmatic control surface: the capability registry rendered as **MCP
tools** and **REST routes** — drive the cluster without the GUI, with
the same governance as every other surface. Product framing:
[../product/control-surfaces.md](../product/control-surfaces.md); execution
plan: [../plans/mcp-service.md](../plans/mcp-service.md) (epic
[#614](https://github.com/oramasearch/zetta/issues/614)). Read here before any
MCP/HTTP work.

- [registry-design.md](registry-design.md) — **read first**: the settled design
  — one registry/many renderings, the main-thread bridge, auth = AccessKey +
  per-capability audit policy, long work = Run/ComputeJob handles (no new job infra),
  the error mapping (schema rejections carry `path`, #1217), browser origin binding (#1415),
  the consent challenge, awaitable reads, independent connection admission (#1076), and
  script-safe JSON in fallback pages (#1405).
- [../architecture/observability.md](../architecture/observability.md) — trace
  tools require `cluster:admin`; feedback omits diagnostics for other callers (#1408).
- [adding-a-capability.md](adding-a-capability.md) — the recipe + review
  self-check for every new tool/route.
- [../pubsub/crud.md](../pubsub/crud.md) — topic and message CRUD examples,
  non-consuming browse, POST read aliases and bounded REST/MCP long polls (#1136).
- [../governance/audit-trail.md](../governance/audit-trail.md) — per-call audit
  rules, polling-read sessions and summaries, segment storage, and retention.
- [governed-actions.md](governed-actions.md) — action parity (#617): the
  product loop's mutating tools, the kind:job handle semantics
  (BackgroundActions, run-id-at-submit), gate parity, boundaries.
- [consent.md](consent.md) — remote consent for cloud-touching tools (#621):
  the challenge protocol (never executes on first call), the
  distinctly-privileged approval rule (approver key / headed surface), the
  consent ledger + `system.consent_requests`, audit + spans on both sides —
  and why enforcement is **off by default** until a reachable approval surface
  ships (#995).
- [rest-openapi.md](rest-openapi.md) — the REST rendering (#618): reads →
  GET /v1/<name>, actions → POST /v1/<name>, output schemas, and the
  drift-checked OpenAPI document generated from the registry.
- [llms-endpoints.md](llms-endpoints.md) — the llms.txt progressive-disclosure
  pages (#645, #1402): public agent docs from the registry, with no workspace
  inventory.
- [specs-index.md](specs-index.md) — the spec index (#646): `/v1/specs` +
  `list_specs`/`get_spec` serving the OpenAPI document + every capability and
  boundary schema from their C++ accessors, drift-checked through one shared
  table.
- [native-http-transport.md](native-http-transport.md) — the **outbound** HTTP
  client (#827): the coroutine HTTPS client behind `connect::HttpTransport`, why
  it lives in `zetta-api`, the sync/async shapes, the no-leak contract, and confined model downloads (#1412).
- [../pubsub/exposure.md](../pubsub/exposure.md) — pub/sub on the control surface
  (#1033, #1076): create_topic / drop_topic / list_topics / publish_event / receive_events,
  exposed topics and long polls that suspend without holding threads.
- [../ml/run-metrics.md](../ml/run-metrics.md) — the five run-metric capabilities,
  their REST and MCP request/response examples, and live topic reads (#1137).
- [../ml/studies.md](../ml/studies.md) — the ten study capabilities, published search spaces, read-only validation, Run-backed
  trials, dedicated promotion/deletion permissions, analysis, lineage-preserving
  cleanup, and bounded live progress reads (#1138, #1158).
- [deletes.md](deletes.md) — `delete_dataset`, `delete_model`, `delete_run`,
  `delete_index` (lake#645): what each removes and keeps, `dry_run`, refusals.
- [serving.md](serving.md) — governed HTTP serving (#648): expose_model /
  expose_index publish per-entity /v1/models/{name}/predict and
  /v1/indexes/{name}/search routes on every node (replicated exposure state,
  data:predict / data:search key scoping, reversible text entity ids, every
  request a span + audited; #957).

## The endpoints (shipped with #615, REST with #618)

`POST /mcp` — JSON-RPC 2.0 (`initialize`, `notifications/initialized`, `ping`,
`tools/list`, `tools/call`), served by Beast2 on the vendored capy/corosio
stack, embedded in BOTH run modes. Default bind: `127.0.0.1:4800`. The default-deny
auth gate is **always on** (#799): every `/mcp` + `/v1/*` request needs
`Authorization: Bearer <secret>` resolving to a **user** via the token store, else
**401** (see [access-control.md](../governance/access-control.md#the-default-deny-gate-is-always-on)).
A fresh node with an empty user store seeds an owner token — from `--admin-token` /
`ZETTA_ADMIN_TOKEN`, or auto-generated and printed **once** if none is supplied —
so it is never locked out. Flags: `--mcp-listen HOST:PORT`, `--no-mcp`. There is
**no** auth flag or toggle: `--require-auth` and `ZETTA_REQUIRE_AUTH` were removed
(#799) and the legacy `--mcp-token` shared secret was removed from the CLI — the
gate is unconditional. Every `tools/call` result carries `structuredContent` with
`ok/value|error`, `trace_id`, `span_id`, `duration_ms` (`value` follows the
tool's `outputSchema`); inbound `traceparent` is honored. The handlers marshal
onto the owning loop (headless tick / headed frame) via the bridge — the loops
drain it beside `Node::tick()`.

With `--clerk-issuer` configured, the same gate also verifies Clerk JWTs from
bearers or the session cookie (#889, #1416). Key retrieval suspends the request;
concurrent misses share one fetch, failed fetches are cached for five seconds,
and each fetch/wait has a two-second deadline. Native tokens remain usable during
an issuer outage. See [Clerk key caching](../governance/access-control.md#clerk-key-caching-1416)
for rotation and stale-key limits. No issuer means no Clerk network requests.

The same server serves the registry as **plain REST** (#618,
[rest-openapi.md](rest-openapi.md)): reads are `GET /v1/<name>` (query-string
input), actions/jobs are `POST /v1/<name>` (JSON body), same bearer → same
audit, responses are the same envelope with error codes mapped onto HTTP
statuses (`forbidden` is 403 and `runner_required` 409, #1261); `GET /v1/openapi.json` is the OpenAPI document generated from the
registered schemas (drift-checked into `schemas/openapi.json`).

The same server also answers plain `GET /llms.txt` / `/llms/<topic>.txt` /
`/llms-full.txt` — public agent-readable pages generated from the registry,
with no workspace inventory, so a no-MCP agent orients with one GET
([llms-endpoints.md](llms-endpoints.md), #645) — and `GET /v1/specs` /
`/v1/specs/<name>`, the spec index over every schema in the system
([specs-index.md](specs-index.md), #646).

The same server serves the **exposed-entity endpoints** ([serving.md](serving.md),
#648): `POST /v1/models/{name}/predict` and `GET /v1/indexes/{name}/search`
answer only while the entity's replicated `exposed` flag is set
(expose_model / expose_index below); access requires a key holding
`data:predict` / `data:search`.

## Connect a client

`POST /mcp` is the **Streamable HTTP** MCP transport, so any MCP client points
straight at it — no adapter, no stdio shim. With a running node (default
`127.0.0.1:4800`; the auth gate is always on, so every request carries a bearer
token):

**Claude Code** — register the server for the current project:

```sh
claude mcp add --transport http zetta http://127.0.0.1:4800/mcp \
  --header "Authorization: Bearer <token>"   # bearer required — the gate is always on
# remove:  claude mcp remove zetta
```

**A JSON-config client** (Claude Desktop and similar) — an `mcpServers` entry:

```json
{
  "mcpServers": {
    "zetta": { "type": "http", "url": "http://127.0.0.1:4800/mcp" }
  }
}
```

**Any HTTP client** — the three calls the protocol needs (add
`-H 'Authorization: Bearer <token>'` — the node always requires one, the gate is
always on):

```sh
curl -sX POST http://127.0.0.1:4800/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
curl -sX POST http://127.0.0.1:4800/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
curl -sX POST http://127.0.0.1:4800/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"server_info","arguments":{}}}'
```

Prefer no MCP at all? A plain `GET /llms.txt` orients an agent in one request
([llms-endpoints.md](llms-endpoints.md)), and the whole surface is plain REST
under `/v1/<name>` ([rest-openapi.md](rest-openapi.md)).

## Capabilities (v1)

| Name | Kind | Requires | Does |
|---|---|---|---|
| `server_info` | read | api:query | node identity, version, entity counts, membership, failed object logs, the object logs' durability format (`object_log_prefix_durability`) and the node's `incarnation` |
| `node_memory` | read | api:query | process RSS, allocator truth, and subsystem ownership |
| `sql` | read | api:query | one governed SQL statement — every `system.*` view; answers `{messages, columns, rows}` with `columns` always filled, and an error for an unknown column ([query.md](../data/query.md)). Each data-changing statement is authorized on its own (`sql:write` / `sql:ddl` / `users:manage` / `tokens:manage`) and audited as `data_changed` ([per-statement enforcement](../governance/access-control.md#per-statement-enforcement), #1229); also a JSON-body POST, so a statement is not bound by the ~4 KiB request line (#1231) |
| `get_record` | read | data:records_read (`records:read`, scopable `ON COLLECTION`) | one keyed record by `(collection, key)`; also POST ([record-store.md](../data/record-store.md)) |
| `list_records` | read | data:records_read | one index value's records ordered by `order` then key, paged by `next_after`; also POST |
| `list_network_records` | read | data:network_records_read (`network:read`) | decoded, retained connection/grant/session/activity pages from this workspace's Zetta; indexed filters, newest-first by default; also POST ([network records](../network/network-records.md)) |
| `query_metric` | read | data:metrics_read (`metrics:read`) | sum a counter or aggregate a gauge over a range, filtered, grouped and stepped; also POST ([time-series.md](../data/time-series.md)) |
| `list_objects` | read | data:objects_read (`objects:read`, scopable `ON BUCKET`) | a bucket's keys and sizes by key, paged; also POST and `GET /v1/objects/<bucket>` ([object-store.md](../data/object-store.md)) |
| `object_usage` | read | data:objects_read | a bucket's objects, bytes and quota; also `GET /v1/objects/<bucket>:usage` |
| `storage_usage` | read | api:query | the bytes this node holds on its disk, by kind (datasets, files, models, indexes, runs, buckets and each bucket, streams, records, catalogue, system); walks the data directory on the blocking pool; `scope: "cluster"` sums every member's census from the telemetry exchange and names the members missing ([storage-census.md](../data/storage-census.md)) |
| `search` | read | api:query | ranked fulltext search via [`Node::search`](../indexing/node-search.md) · distributed when partitioned · per-node provenance |
| `list_projects` | read | api:query | project names (`projects`), plus `summaries` (#1185): each project's id and name and each pipeline's id, name and approval, in one call; present only when the caller may also call `get_project` ([projects.md](../pipeline/projects.md#listing-projects--list_projects)) |
| `get_project` | read | api:query | one project's full detail by name or uuid, including each pipeline's methodology and runnable status |
| `get_run` | read | api:query | one run's validated record (status / logs / outputs / lineage) by uuid |
| `get_model` | read | api:query | one model's validated record (kind / lineage / feature schema / metrics / weights fingerprint) by uuid |
| `get_experiment` | read | api:query | one experiment's sizes, allocation balance, exemption erosion, and uplift-training assignment rows |
| `get_monitor` | read | api:query | one solution's input/score drift, calibration, cohort performance, guardrails, active breaches, and retrain request by solution name or monitor uuid |
| `get_roi_readout` | read | api:query | one solution's versioned correctness certificate, aggregate treated-minus-holdout incrementality with confidence intervals, customer-supplied ROI, guardrails, agent operations, and redacted audit extract |
| `get_job` | read | api:query | one compute job's validated record (kind / status / lineage refs / detail) by uuid |
| `get_index` | read | api:query | one index's validated record (table / kind / backend / fields) by uuid |
| `get_dataset` | read | api:query | one dataset's computed summary (typed columns `[{name, type, nullable, null_fraction}]` in source order + `column_order` / rows / size / storage + source format / replication / placement) by name or `database.table` (+ database when ambiguous) |
| `get_policy` | read | api:query | one decision policy's current config and immutable version history |
| `analyze_dataset` | read | api:query | sample a landed dataset and report semantic types with evidence, column statistics, grain, candidate keys, join candidates, and leakage candidates |
| `plan_features` | read | api:query | return a methodology-specific feature/exclusion plan, or a named `transform_required` correction that is never applied automatically |
| `list_runs` | read | api:query | run rows with Project/Pipeline display names, status, stored metric summary and timestamps |
| `list_models` | read | api:query | model-version rows with Project/Pipeline display names, producing Run, metrics, feature schema, serving state and creation time |
| `list_jobs` | read | api:query | compact compute-job listing (ids feed `get_job`) |
| `list_indexes` | read | api:query | compact index listing (ids feed `get_index`) — discover what `search` can target |
| `list_datasets` | read | api:query | every dataset's computed summary, structured (columns/partitions as arrays); `detail: "summary"` (#1195) answers only the list fields (rows, size_bytes, format, partition_count, degraded, …) from counters, reading no row and no file ([system-datasets.md](../data/system-datasets.md#the-lean-list-list_datasets-detail-summary-1195)) |
| `list_methodologies` | read | api:query | all 21 catalogue methodologies, their data and result contracts, study support and published search spaces, primary solutions, model kinds, implementation status, and tracking issues, then the workspace's own; every entry says `source` (`orama` or `workspace`) and `editable` ([workspace methodologies](../ml/workspace-methodologies.md), #1261) |
| `get_methodology` | read | api:query | one methodology by id: a catalogue entry (M-n, read-only) or a workspace methodology (W-<8 hex>) with its interface, code location, image and validation (#1261) |
| `register_methodology` | action | orchestration:register_methodology | register a workspace methodology whose Python code Lake's runner runs (never the engine); returns its W- id, not validated (#1261) |
| `update_methodology` | action | orchestration:update_methodology | change a workspace methodology, its image or its validation; a new code revision resets validation; Orama's answer `forbidden` (#1261) |
| `delete_methodology` | action | orchestration:delete_methodology | remove a workspace methodology (idempotent); Orama's answer `forbidden` (#1261) |
| `record_methodology_run` | action | orchestration:record_methodology_run | record what Lake's runner produced: a training run registers a Model (kind `python:<id>`, the artifact as its weights: ≤ 24 MiB inline, or up to 1 GiB staged with `artifact_staged`, in-sample and held-out metrics) and a Run, on the pipeline for a step (#1261, lake#690) |
| `append_methodology_artifact` | action | orchestration:append_methodology_artifact | stage a run's artifact over 24 MiB in ordered chunks of ≤ 8 MiB, up to 1 GiB; answers the bytes staged (lake#690) |
| `read_model_weights` | read | api:query | a model's weights, ≤ 8 MiB from `offset`, with `next_offset`, `total_bytes` and `sha256` (lake#690) |
| `list_ledger` | read | api:query | immutable action and outcome entries from the replicated [solutions audit ledger](../solutions/audit-ledger.md) |
| `list_solutions` | read | api:query | all 20 runnable catalogue solutions, with composition, product contract, `levels` objects, missing components, and runnable status |
| `get_solution` | read | api:query | one runnable catalogue solution by id; `solution.levels[].level` is the integer posted to `create_solution` |
| `list_traces` | read | api:cluster_admin | recent node-wide observability spans; requires `cluster:admin` (#1408) |
| `get_trace` | read | api:cluster_admin | every span of one trace id; requires `cluster:admin` (#1408) |
| `get_run_report` | read | api:query | finished-run facts: M-1 quality/risk; M-7 anomaly metrics, ranked findings, output tables, and read/excluded input facts |
| `get_study` | read | api:query | one study's objective, search space, sampler, pruner, budgets, usage, state, and Run trials |
| `validate_study` | read | api:query | validate and normalize a create-study candidate without storing it, returning a field path and repair reason when invalid |
| `list_studies` | read | api:query | all studies with flattened Project/Pipeline names, lifecycle state, timestamps and objective summary |
| `list_trials` | read | api:query | one study's Run trials, optionally filtered by running/pruned/failed/complete state |
| `get_study_analysis` | read | api:query | best trial, optimization history, parameter importances, parallel-coordinate rows, and confidence leaderboard |
| `list_specs` | read | api:query | the spec index (#646): OpenAPI + every capability and boundary schema as names/kinds/links |
| `get_spec` | read | api:query | one spec document by its `list_specs` name (`openapi`, `<capability>.input`/`.output`, or a boundary name) |
| `testsuite_list` | read | api:query | the in-app test-suite catalog — every case id + category + tunable options; ids feed `testsuite_run` (also queryable as `system.test_cases`) |
| `issue_app_credential` | action | api:manage_tokens (`tokens:manage`) | create one connection-owned user, least-privilege role, and hashed token; return its plaintext once; refuse a second live token for the same connection ([scoped app access](../network/scoped-app-access.md), #1280) |
| `revoke_app_credential` | action | api:manage_tokens (`tokens:manage`) | revoke every token for one connection and remove its role assignment; idempotent (#1280) |
| `create_collection` | action | orchestration:create_collection (`records:manage`) | declare a record collection, its indexes and retention; idempotent for an identical definition |
| `put_record` | action | data:records_write (`records:write`) | conditional (`if_absent` / `if_revision`, 409 otherwise) keyed write, durable and on every live node before it returns |
| `write_network_record` | action | data:network_records_write (`network:write`) | validate and durably store a Lake connection/grant history object or zetta-tunnel session/gateway decision in this workspace's Zetta ([network records](../network/network-records.md)) |
| `delete_record` | action | data:records_write | conditional delete |
| `create_metric` | action | orchestration:create_metric (`metrics:write`) | declare a labelled counter or gauge |
| `record_metric` | action | data:metrics_write (`metrics:write`) | append up to 1000 labelled points, all validated first |
| `create_bucket` | action | orchestration:objects_manage (`objects:manage`, scopable `ON BUCKET`) | declare an object bucket, its quota and replication factor; idempotent for an identical definition |
| `object_barrier` | action | data:objects_write (`objects:write`) | fsync every write to the bucket acknowledged before the call, on every live node; also `POST /v1/objects/<bucket>:barrier` |
| `delete_object` | action | data:objects_write | remove one object, fenced once the bucket has a lease holder; also `DELETE /v1/objects/<bucket>/<key>` |
| `acquire_object_lease` / `renew_object_lease` / `release_object_lease` | action | data:objects_write | the bucket's owner lease: `acquire` waits up to `wait_ms` without holding a thread and returns the fence, the bucket's `incarnation` and `object_log_prefix_durability` (409 `held` otherwise); also `POST /v1/objects/<bucket>:acquire` / `:renew` / `:release` |
| `submit_feedback` | action | api:query | store feedback as a large object; only `cluster:admin` attaches traces, errors and crashes; export is explicit (#640, #1408) |
| `record_action` | action | orchestration:record_action | append score, decision, action, actor, versions, experiment cell, cost, and timestamps to the solutions audit ledger |
| `record_outcome` | action | orchestration:record_outcome | append a connector or manual outcome referencing an existing action entry |
| `create_project` | action | orchestration:create_project | create a replicated AI project |
| `create_study` | action | orchestration:create_study | create a gated Run-backed AutoML study with typed search space, sampler, pruner, and budgets |
| `stop_study` | action | orchestration:stop_study | stop a study and its active trial Runs |
| `stop_trial` | action | orchestration:stop_trial | stop one active trial Run without stopping its study |
| `promote_trial` | action | orchestration:promote_trial (`studies:promote`) | register a complete trial's existing Model as the Study's promoted project version |
| `delete_study` | action | orchestration:delete_study (`studies:delete`) | delete a stopped or complete study and non-promoted trial Runs with their metrics while preserving promoted Model lineage |
| `create_policy` | action | orchestration:create_policy | create version 1 of a replicated decision policy |
| `update_policy` | action | orchestration:update_policy | append a replacement version or apply bounded numeric tuning |
| `decide` | action | orchestration:decide | apply a policy to one scored entity or a scored table and persist versioned decisions |
| `approve_decision` | action | orchestration:approve_decision | approve a pending A1 decision without changing its policy version |
| `create_experiment` | action | orchestration:create_experiment | create a replicated seeded holdout, treatment, exploration, and below-the-line allocation definition |
| `assign` | action | orchestration:assign | assign an entity before a proposed action, suppress protected model-driven actions, and log explicit exemptions |
| `publish_report` | action | orchestration:publish_report | publish a driver report and decision log to a forum webhook with retry, idempotency, acknowledgement, and ledger evidence |
| `enqueue_action_task` | action | orchestration:enqueue_action_task | write an owner- and SLA-bound task to the replicated outbox after holdout and policy checks |
| `deliver_action_task` | action | orchestration:deliver_action_task | deliver an outbox task through webhook, Salesforce, or HubSpot with bounded retries and dead-letter handling |
| `execute_closed_loop` | action | orchestration:execute_closed_loop | enqueue and deliver one task while rechecking holdout and the current policy kill-switch before HTTP |
| `purge_audit` | action | orchestration:purge_audit | admin-only segment rewrite before a UTC cutoff; advances replicated low-water marks and records `audit_purged` (#1169) |
| `delete_project` | action | orchestration:delete_project | delete a project with its inputs + graphs on every node; its runs + models are kept and counted (#1009, [projects.md](../pipeline/projects.md)) |
| `delete_dataset` | action | orchestration:delete_dataset | delete a dataset with its partitions, their bodies and every index over it; refused while a pending or running run or a running study reads it (zetta#1258); `dry_run` lists what goes ([deletes.md](deletes.md)) |
| `delete_model` | action | orchestration:delete_model | delete a model (unexposed first); its run is kept; a standalone model's last version takes its internal project with it; `dry_run` lists what goes ([deletes.md](deletes.md)) |
| `train_model` | job | orchestration:train_model | train a standalone model from a dataset, a target and a methodology; its internal project is never listed ([standalone-models.md](../ml/standalone-models.md)) |
| `retrain_model` | job | orchestration:retrain_model | train a new version of a model, on what it trained on or new settings ([standalone-models.md](../ml/standalone-models.md)) |
| `register_model` | action | orchestration:register_model | keep a brought model file (ONNX tree ensemble, XGBoost JSON/UBJSON, LightGBM text, or the engine's own weights, converted exactly) as a standalone model, or a new version of one ([model-import.md](../ml/model-import.md)) |
| `export_model` | read | api:query | one model version's weights as a file: as kept (`zetta`), or a tree model as ONNX (`onnx`) ([model-import.md](../ml/model-import.md)) |
| `move_model_to_project` | action | orchestration:move_model_to_project | reveal a standalone model's project under a name, with every version ([standalone-models.md](../ml/standalone-models.md)) |
| `delete_run` | action | orchestration:delete_run | delete a finished run; refused while pending or running; its models are kept and listed ([deletes.md](deletes.md)) |
| `delete_index` | action | orchestration:delete_index | delete an index (unexposed first); the dataset is kept ([deletes.md](deletes.md)) |
| `expose_model` | action | orchestration:expose_model | publish a model's /v1/models/{name}/predict endpoint on every node (replicated flag); endpoint access needs data:predict |
| `unexpose_model` | action | orchestration:unexpose_model | withdraw the model's predict endpoint everywhere in one action |
| `expose_index` | action | orchestration:expose_index | publish an index's /v1/indexes/{name}/search endpoint (distributed scatter-gather underneath); endpoint access needs data:search |
| `unexpose_index` | action | orchestration:unexpose_index | withdraw the index's search endpoint everywhere in one action |
| `capture_intent` | action | orchestration:capture_intent | capture NL intent as a replicated project input with provenance |
| `import_n8n` | action | orchestration:import_n8n | schema-validate + store an n8n workflow byte-for-byte as a project artifact |
| `compile_intent` | action | orchestration:compile_intent | resolve an optional methodology or solution (otherwise registered keywords), refuse unavailable work with its issue, and store the runnable methodology's editable graph |
| `create_solution` | action | orchestration:create_solution | compose a selected level and create its versioned policy, experiment, monitor and ROI definition from typed defaults and explicit snapshot/delivery/economics inputs |
| `compile_n8n` | action | orchestration:compile_n8n | compile a stored n8n workflow into a stored, editable graph |
| `compile_python` | action | orchestration:compile_python | lower sim-Python source to a stored, editable graph (never runs Python) |
| `gate` | action | orchestration:gate | run the correctness gate; returns the full ValidationReport (no state change) |
| `deploy` | action | orchestration:deploy | gate (RULE-1, failing check's reason verbatim) then place a graph; replicated + audited |
| `run` | job | orchestration:run | resolve model graphs through the methodology registry and dispatch their runner, including M-5 survival, M-12 graph ML and M-21 streaming over their connect-step tables; a workspace methodology's graph answers `runner_required` (409) with `error.details` naming what Lake's runner runs ([workspace methodologies](../ml/workspace-methodologies.md#a-pipeline-step), #1261); M-16 accepts bounded LP/MIP controls and returns solver status plus plan/constraint table handles; refuse unknown or unavailable model kinds; graphs without a model remain structural |
| `run_solution` | job | orchestration:run_solution | run snapshot and methodology stages, enforce policy/holdout protection, deliver actions and record ledger/monitor state; return a durable Run and decision-table output |
| `add_dataset` | action | orchestration:add_dataset | load a local parquet/jsonl/json/csv file into a partitioned table placed across the cluster |
| `snapshot` | action | orchestration:snapshot | materialize the latest row per entity at or before an inclusive cutoff into a replicated table keyed by `(entity_id, as_of)` (#1103) |
| `upload_dataset` | action | orchestration:add_dataset | load a csv/jsonl file's TEXT sent in the request (≤ 16 MiB) through add_dataset's parsing + placement — for callers that cannot write the node's filesystem (#1008); refuses a taken name (`conflict`) unless `if_exists: "rename"`, unsafe names and ragged CSV, in lake's words (#1221, [add-dataset.md](../data/add-dataset.md)) |
| `load_dataset_object` | job | orchestration:add_dataset | load a csv/jsonl object already in a bucket (staged a chunk at a time with `PATCH /v1/objects/<bucket>/<key>`, up to 1 GiB) into a partition-only table in the background; returns a run id to poll with `get_run`; also needs `objects:read` on the bucket; `remove_object` removes the object afterwards and also needs `objects:write` (#1302, [add-dataset.md](../data/add-dataset.md#load-from-a-stored-object-1302)) |
| `create_index` | action | orchestration:create_index | build a Search index over a loaded table (records the entity + builds the per-partition BM25F sub-indexes) so `search` returns ranked hits — the public create path [`Node::create_index`](../indexing/node-search.md) names |
| `testsuite_run` | job | orchestration:testsuite_run | start one in-app test case in the background; run id returned immediately, logs land with the terminal status |
| `cloud_inspect` | action | orchestration:cloud_inspect | **consent-gated with `--governance`** read-only provider inspect (identity/regions via the provider CLI); result cached as `system.cloud_status` |
| `detect_provisioners` | action | orchestration:detect_provisioners | **consent-gated with `--governance`** re-detect of the local CLIs' deployment contexts → `system.provisioners` |
| `add_nodes` | action | orchestration:add_nodes | add permanent cluster nodes via a wired provisioner; cloud targets **consent-gated with `--governance`** (billable), `local` executes directly |
| `list_consent_requests` | read | api:query | the node's consent ledger — pending challenges, decisions, and executed results |
| `approve_consent` | action | orchestration:approve_consent | approve one challenge and execute exactly the named action once — must be a **different principal** than the requester |
| `deny_consent` | action | orchestration:deny_consent | refuse (or withdraw) a pending challenge; the action never runs |

All accept the reserved optional `intent` field (recorded to the span,
stripped before schema validation).

**Read parity (#616).** The typed `get_*` reads return the entity's
**already-validated value JSON** — exactly the `metadata()` shape the boundary
schema validated (the CRDT value / on-disk projection) — with the `id` added;
`get_dataset` / `list_datasets` return the computed `DatasetSummary` with the
schema and partition placement kept structured (the SQL view flattens them for
tabular reading). `list_datasets` gives each dataset's `columns` as names;
`get_dataset` gives them typed, `[{name, type, nullable, null_fraction}]`, with
the last three null for a column the caller may not read (`sql:read`). Both
list them in the source's order and say whether one was recorded
(`column_order`: `source` / `first_seen`) — [column-order.md](../data/column-order.md), #1021. Every handler is a thin caller of the same live engine
source its `system.*` view computes from (`Node::…()`), so MCP, SQL, and the
GUI cannot drift (RULE-7). Arbitrary reads beyond these go through `sql`.

**Action parity (#617).** The mutating half of the loop — each a thin caller
of the same engine entry point the GUI drives, `orchestration:<name>`-scoped,
audited under the acting key, `mutates` firing the activity hook; `run` and
`testsuite_run` are `kind: job` and return the engine's durable Run id
immediately (poll `get_run` / `system.runs`). Details + handle-ownership
design: [governed-actions.md](governed-actions.md).
