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
  audit-on-authorize, long work = Run/ComputeJob handles (no new job infra),
  the error mapping, the consent challenge, and awaitable REST reads (#1076).
- [adding-a-capability.md](adding-a-capability.md) — the recipe + review
  self-check for every new tool/route.
- [../pubsub/crud.md](../pubsub/crud.md) — topic and message CRUD examples,
  non-consuming browse, POST read aliases and bounded REST/MCP long polls (#1136).
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
  pages (#645): plain-GET agent docs generated live from the registry + node
  state, no second prose source.
- [specs-index.md](specs-index.md) — the spec index (#646): `/v1/specs` +
  `list_specs`/`get_spec` serving the OpenAPI document + every capability and
  boundary schema from their C++ accessors, drift-checked through one shared
  table.
- [native-http-transport.md](native-http-transport.md) — the **outbound** HTTP
  client (#827): the coroutine HTTPS client behind `connect::HttpTransport`, why
  it lives in `zetta-api`, the sync/async shapes, and the no-leak contract.
- [../pubsub/exposure.md](../pubsub/exposure.md) — pub/sub on the control surface
  (#1033, #1076): create_topic / drop_topic / list_topics / publish_event / receive_events,
  exposed topics and long polls that suspend without holding threads.
- [serving.md](serving.md) — governed HTTP serving (#648): expose_model /
  expose_index publish per-entity /v1/models/{name}/predict and
  /v1/indexes/{name}/search routes on every node (replicated exposure state,
  data:predict / data:search key scoping, every request a span + audited).

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

The same server serves the registry as **plain REST** (#618,
[rest-openapi.md](rest-openapi.md)): reads are `GET /v1/<name>` (query-string
input), actions/jobs are `POST /v1/<name>` (JSON body), same bearer → same
audit, responses are the same envelope with error codes mapped onto HTTP
statuses; `GET /v1/openapi.json` is the OpenAPI document generated from the
registered schemas (drift-checked into `schemas/openapi.json`).

The same server also answers plain `GET /llms.txt` / `/llms/<topic>.txt` /
`/llms-full.txt` — agent-readable pages generated live from the registry +
node state, so a no-MCP agent orients with one GET
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
| `server_info` | read | api:query | node identity, version, entity counts, membership |
| `sql` | read | api:query | one governed SQL statement — every `system.*` view |
| `search` | read | api:query | ranked fulltext search via [`Node::search`](../indexing/node-search.md) · distributed when partitioned · per-node provenance |
| `list_projects` | read | api:query | project names |
| `get_project` | read | api:query | one project's full detail by name or uuid, including each pipeline's methodology and runnable status |
| `get_run` | read | api:query | one run's validated record (status / logs / outputs / lineage) by uuid |
| `get_model` | read | api:query | one model's validated record (kind / lineage / feature schema / metrics / weights fingerprint) by uuid |
| `get_experiment` | read | api:query | one experiment's sizes, allocation balance, exemption erosion, and uplift-training assignment rows |
| `get_monitor` | read | api:query | one solution's input/score drift, calibration, cohort performance, guardrails, active breaches, and retrain request by solution name or monitor uuid |
| `get_roi_readout` | read | api:query | one solution's versioned correctness certificate, aggregate treated-minus-holdout incrementality with confidence intervals, customer-supplied ROI, guardrails, agent operations, and redacted audit extract |
| `get_job` | read | api:query | one compute job's validated record (kind / status / lineage refs / detail) by uuid |
| `get_index` | read | api:query | one index's validated record (table / kind / backend / fields) by uuid |
| `get_dataset` | read | api:query | one dataset's computed summary (schema / rows / size / replication / placement) by name (+ database when ambiguous) |
| `get_policy` | read | api:query | one decision policy's current config and immutable version history |
| `analyze_dataset` | read | api:query | sample a landed dataset and report semantic types with evidence, column statistics, grain, candidate keys, join candidates, and leakage candidates |
| `plan_features` | read | api:query | return a methodology-specific feature/exclusion plan, or a named `transform_required` correction that is never applied automatically |
| `list_runs` | read | api:query | compact run listing (ids feed `get_run`) |
| `list_models` | read | api:query | compact model listing (ids feed `get_model`) |
| `list_jobs` | read | api:query | compact compute-job listing (ids feed `get_job`) |
| `list_indexes` | read | api:query | compact index listing (ids feed `get_index`) — discover what `search` can target |
| `list_datasets` | read | api:query | every dataset's computed summary, structured (columns/partitions as arrays) |
| `list_methodologies` | read | api:query | all 21 registered methodologies, their data and result contracts, primary solutions, model kinds, implementation status, and tracking issues |
| `list_ledger` | read | api:query | immutable action and outcome entries from the replicated [solutions audit ledger](../solutions/audit-ledger.md) |
| `list_solutions` | read | api:query | all 12 catalogue solutions and eight sketches, with composition, product contract, `levels` objects, missing components, and runnable status |
| `get_solution` | read | api:query | one complete solution or sketch entry by id; `solution.levels[].level` is the integer posted to `create_solution` |
| `list_traces` | read | api:query | recent observability spans |
| `get_trace` | read | api:query | every span of one trace id |
| `get_run_report` | read | api:query | finished-run facts: M-1 quality/risk; M-7 anomaly metrics, ranked findings, output tables, and read/excluded input facts |
| `list_specs` | read | api:query | the spec index (#646): OpenAPI + every capability and boundary schema as names/kinds/links |
| `get_spec` | read | api:query | one spec document by its `list_specs` name (`openapi`, `<capability>.input`/`.output`, or a boundary name) |
| `testsuite_list` | read | api:query | the in-app test-suite catalog — every case id + category + tunable options; ids feed `testsuite_run` (also queryable as `system.test_cases`) |
| `submit_feedback` | action | api:query | snapshot a feedback bundle (description + traces + errors + crashes + versions) as a large object; export is explicit (#640) |
| `record_action` | action | orchestration:record_action | append score, decision, action, actor, versions, experiment cell, cost, and timestamps to the solutions audit ledger |
| `record_outcome` | action | orchestration:record_outcome | append a connector or manual outcome referencing an existing action entry |
| `create_project` | action | orchestration:create_project | create a replicated AI project |
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
| `delete_project` | action | orchestration:delete_project | delete a project with its inputs + graphs on every node; its runs + models are kept and counted (#1009, [projects.md](../pipeline/projects.md)) |
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
| `run` | job | orchestration:run | resolve model graphs through the methodology registry and dispatch their runner, including M-5 survival, M-12 graph ML and M-21 streaming over their connect-step tables; M-16 accepts bounded LP/MIP controls and returns solver status plus plan/constraint table handles; refuse unknown or unavailable model kinds; graphs without a model remain structural |
| `run_solution` | job | orchestration:run_solution | run snapshot and methodology stages, enforce policy/holdout protection, deliver actions and record ledger/monitor state; return a durable Run and decision-table output |
| `add_dataset` | action | orchestration:add_dataset | load a local parquet/jsonl/json/csv file into a partitioned table placed across the cluster |
| `snapshot` | action | orchestration:snapshot | materialize the latest row per entity at or before an inclusive cutoff into a replicated table keyed by `(entity_id, as_of)` (#1103) |
| `upload_dataset` | action | orchestration:add_dataset | load a csv/jsonl file's TEXT sent in the request (≤ 16 MiB) through add_dataset's parsing + placement — for callers that cannot write the node's filesystem (#1008, [add-dataset.md](../data/add-dataset.md)) |
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
tabular reading). Every handler is a thin caller of the same live engine
source its `system.*` view computes from (`Node::…()`), so MCP, SQL, and the
GUI cannot drift (RULE-7). Arbitrary reads beyond these go through `sql`.

**Action parity (#617).** The mutating half of the loop — each a thin caller
of the same engine entry point the GUI drives, `orchestration:<name>`-scoped,
audited under the acting key, `mutates` firing the activity hook; `run` and
`testsuite_run` are `kind: job` and return the engine's durable Run id
immediately (poll `get_run` / `system.runs`). Details + handle-ownership
design: [governed-actions.md](governed-actions.md).
