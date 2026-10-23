# The capability registry — design

The load-bearing design for the MCP + REST control surface (#614/#615). Every
hard decision is settled here; [adding-a-capability.md](adding-a-capability.md)
is the recipe that follows from it. Realizes
[../product/control-surfaces.md](../product/control-surfaces.md).

## One registry, many renderings

`zetta::api::Registry` holds every capability exactly once:

```
Capability {
  name            // snake_case verb_object: "sql", "server_info",
                  // "list_projects", "create_project", "run_solution"
  title, description   // agent-facing; description states governance +
                       // side effects in one sentence
  input_schema    // draft-07 JSON Schema (schema-as-code: an accessor
                  // returns it; validated with the pboettch validator
                  // at the boundary — same discipline as Run/Model)
  output_schema   // documented shape; asserted in journeys/tests
  kind            // read | action | job
  required_operation   // governance::Operation{domain, action}
  mutates         // fires the activity hook; tells GUIs to refresh
  requires_consent     // cloud-touching: returns a consent challenge
  handler         // std::function<ApiResult(const json&, ApiContext&)>
  awaitable_read  // optional REST/MCP event wait between at most two registry
                  // calls, or (retry_within) one call per wake until wait_for
}
```

Renderings are thin: the MCP server lists/calls entries; the REST layer (#618)
derives `GET /v1/<name>` (reads) and `POST /v1/<name>` (actions) plus the
OpenAPI document from the same schemas; the command palette (#625) is the same
list rendered for humans. **A capability added once appears everywhere.**

**MCP ↔ REST parity is mandatory (Steve, 2026-07-25).** Every capability
surfaces on **both** MCP (`tools/list` / `tools/call`) **and** REST (`/v1/<name>`)
— there is **no** REST-only or MCP-only capability, and no per-capability
"exclude from MCP" flag. This is by construction (both are thin renderings of the
one registry) and it is a **requirement**, not just a consequence: when you add a
capability for one surface (e.g. a UI's REST need), it is *intended* to be an MCP
tool too. Do **not** add a mechanism to hold a capability back from MCP. (The
`system.*` read views are reached through the `sql` capability, which is itself on
both surfaces — same parity.) Per-**principal** scope still applies (a token only
sees the tools it may call, #747); parity is about the *surface*, not authz.

Naming: `snake_case`, verb-first for actions (`create_project`), noun-ish for
reads (`server_info`, `list_runs`, `get_run`). No `zetta_` prefix — the server
provides the namespace; MCP clients that aggregate servers already qualify by
server name.

## JSON embedded in HTML (#1405)

The standalone server-rendered pages use `web_ui_script_json` whenever JSON
enters a script body, including `type="application/json"` data blocks. It
serializes JSON with ASCII escapes and encodes every `<` as `\u003c`, so stored
strings cannot close a script element or change the HTML parser's state.
Parsing the result restores the original values, including nested attributes
and object keys. The graph editor, file panel and assistant rail share this
encoder. HTML text and attributes continue to use `web_ui_escape`.

This changes HTML output only; stored graph data and API JSON formats are
unchanged. With an installed Studio bundle, `/` redirects to `/studio/` and
serves the bundle unchanged; graph data comes through authenticated API reads.

## Threading — the main-thread bridge (the key decision)

The `Node` tree is **main-thread-only** (the same invariant the network thread
obeys). The HTTP server runs on its own thread, so **no handler may touch the
`Node` directly**. The bridge:

- `zetta::api::MainThreadBridge` — a mutex-guarded queue of owner-thread closures.
  The owning loop **drains it**: `run_headless`'s tick loop and the headed
  app's frame loop both call `bridge.drain()` right where `Node::tick()`
  already runs. An HTTP handler posts a closure and suspends on its completion
  event. It resumes only after the closure has run, including during transport
  cancellation, because the closure can borrow the request frame (#1077).
- Enqueueing wakes the owner loop, which drains work before its next timed tick.
  Handlers keep the Node ownership invariant without blocking the I/O thread.
- Everything routes through the bridge, including reads that could arguably
  go direct — uniformity beats cleverness here; the thread-safety table lives
  in one place.
- **The one exception: an `offloadable` capability** (`chat`) runs its *handler*
  on a pool worker so it never pins the loop. The
  call's authorization + audit gate still runs on the owner thread, through
  `Node::run_on_owner`: the gate reads the access keys and appends to the audit
  trail, which the tick reconciles. Running the gate on the worker
  crashed lake's bus Zetta every few minutes (#1063). An offloadable handler may
  touch only thread-safe state (the replication store, `PubSub`), and
  `Registry::add` refuses an offloadable entry that mutates or requires consent,
  because the activity hook and the consent ledger run after the gate.
- **Awaitable reads do not offload.** `receive_events` and `browse_topic` register a topic event
  inside their first owner-loop call, return an immediate read, and let the REST
  or MCP coroutine suspend on that event, bounded by the request's own `wait_ms`
  (at most 60 s, #1174). Publish, edit or close wakes it; the route makes one
  final owner-loop call. A long poll therefore holds no thread and makes at most
  two authorized registry calls. Those reads share an audit session and
  contribute to its five-minute totals (#1076, #1136, #1154). The wait also
  races the client closing its socket (`Impl::await_poll`); a client that left
  gets no final call and no answer, and its session is freed (#1174).
- **A retrying await** (#1005): an `AwaitableRead` with `retry_within` keeps
  waiting until `wait_for` passes — each wake, the event or `retry_within(result)`
  elapsing, makes one more whole governed call with a fresh event. Such an entry
  may be a blocking action that mutates (`Registry::add` still refuses it
  offloadable or consent-gated); its calls run on the blocking pool.
  `acquire_object_lease` waits out another holder's lease this way: the event is
  a lease change, `retry_within` the holder's expiry.
- **Object-scoped operation grants** (#1136): an entry may declare an
  `authorize_scoped` owner-thread fallback when there is no global operation
  grant. The per-tool gate still runs first. Pub/sub resolves concrete topic
  addresses through the object tier; listings accept a topic-prefix grant and
  filter every result through it. A global grant still covers all objects.
- **Owner hops wake the loop.** Work a pool worker queues with
  `Node::run_on_owner` (that gate, the pub/sub authorizer, background actions)
  wakes the idle loop through `Node::set_owner_wakeup` (the API server's
  `wake()`, a `MainThreadBridge::poke`), and the loop drains it on that wake,
  not only inside the 10 Hz `tick()`. Without both, each hop waited out a tick:
  `receive_events` took ~205 ms with no wait at all; it now takes under 1 ms.

## Connection admission

The HTTP adapter owns its accept loop and uses Beast2's HTTP parser/session logic
for each admitted socket. It does not use the fixed TCP worker pool: that pool
waited for an idle worker before accepting, so held long polls could fill the
listen backlog and make new TCP connections time out (#1076).

Sessions are created on demand. `ServerOptions::idle_connection_cache` controls
only reuse of finished parser/serializer objects; `max_connections` is the
separate, explicit admission cap. At that cap the listener still accepts and
answers HTTP 503 / `busy` with `Retry-After: 1`, without entering the registry.
`listen_backlog` requests the kernel's pending-connection queue size. Defaults,
OS resource limits and the 500-held-polls/20-new-connections regression are in
[pub/sub exposure](../pubsub/exposure.md#exposure-on-mcp-and-rest).

Session sockets are closed on completion, including cached sessions. Shutdown
closes the listener and sockets on their IO executor, signals pending poll events,
and keeps draining owner work until all session coroutines finish. This preserves
the lifetime of queued bridge callbacks. No capability schema or new JSON format
is added.

## Ordered shutdown

`ApiServer::stop_accepting()` closes the listener before terminal node shutdown
begins. `main` then calls `PubSub::shutdown()` so every `receive_events` wait is
woken, and calls `ApiServer::stop()` while the `Node` and its owner thread still
exist. `stop()` closes the admitted session sockets on the I/O executor and keeps
`MainThreadBridge` and `Node::run_on_owner` work draining until every REST
capability coroutine has returned, with a five-second upper bound. Only then does
it release the `io_context` work hold, join the I/O thread and allow the server
context to be destroyed (#1077).

The work hold matters for offloaded calls: while a long poll is running on the
blocking pool there may briefly be no completion queued on the I/O executor.
Without the hold, `io_context::run()` can return before the pool posts the
completion, leaving the request coroutine to be destroyed while its stop
callback is still running. No new wire or JSON format is introduced.

With the awaitable long polls from #1076, shutdown cancellation can also resume a
bridge wait before its queued closure has run. That closure borrows the route's
`ApiResult` and input; letting the route finish first leaves dangling references
for the owner drain. `MainThreadBridge::run_on_owner` therefore masks cancellation
only while waiting for queued work to complete. Topic waits remain cancellable,
and `PubSub::shutdown()` signals them while the topics, Node and API context are
still alive. Their final owner-thread reads complete before the routes finish and
the request count reaches zero. The bridge regression covers cancellation both
before queueing and while queued (#1077, stacked on #1100).

`Shutdown.SigtermWithLongPollsInFlightExitsPromptly` starts a real headless
node, opens eleven 30-second REST long polls, and requires a normal exit within
one second of SIGTERM. Its child environment excludes cloud CLIs, and cleanup
reaps the child before joining clients so a regression cannot strand the suite.

## Auth, governance, audit — nothing new

- **Browser origin binding (#1415, LSR-242).** Every browser POST route
  (`/login`, `/session`, both logout routes, `/run`, `/assist`, `/shell`,
  `/graph-save`, `/blob`, `/blob/delete`) requires exactly one `Origin` matching
  the request's scheme and `Host`, including the port. Missing, opaque (`null`),
  duplicate and sibling origins return 403 before dispatch or cookie changes.
  The first router middleware discards cached `Set-Cookie` and `Location` fields
  on every request, including public GETs and preflight responses. Only the
  current handler may set a session or redirect. Origin refusals close the
  connection with the body unread, preventing those bytes from reaching another
  request on the worker.
  Cookie-authenticated REST, MCP and object routes apply the same check on every
  method. For GET/HEAD without `Origin`, `Sec-Fetch-Site: same-origin` or `none`
  is accepted; older clients may send one absolute, same-origin `Referer` without
  userinfo or a fragment. Cross-site and same-site Fetch Metadata are refused,
  even with another matching header. Cookie GET SQL accepts only batches of
  parsed SELECT statements, checked in full before execution; send writes by
  POST. Explicit bearer API requests keep their existing contract.
  TLS ingress must preserve the browser's `Host` and overwrite
  `X-Forwarded-Proto` with `https` (`http` for plain HTTP); forwarded host headers
  are not used. Direct HTTP uses `http://Host`. Headers stripped by a proxy or
  privacy policy can cause a 403; browser callers must retain origin evidence.
  No capability schema or new JSON format is added. Return URL normalization is
  tracked separately under LSR-262.
- **Bearer token = an `AccessKey` id.** Possession of the key id is the
  credential, exactly the strength of the existing model (V1); minting a
  dedicated API-secret rides the existing key entity later, not a parallel
  identity system.
- **The default-deny gate is always on (#799).** Every HTTP surface (`/mcp` +
  `/v1/*`), loopback or not, is closed unless the bearer resolves to a **user**
  through the token store; the capability check then runs against that user's
  effective permissions — see
  [access-control.md](../governance/access-control.md#the-default-deny-gate-is-always-on).
  There is no `--require-auth` flag anymore (#799 removed it and `ZETTA_REQUIRE_AUTH`);
  the gate is unconditional. The in-process headed-GUI governed console still runs as
  a trusted local session because it authorizes **in-process**, not through this HTTP
  gate.
- Every call runs the deny-by-default capability check and the registry applies
  its declared audit policy. Writes, denials, failures, and ordinary reads stay
  per-call. Successful `receive_events`, `browse_topic`, `get_topic`, and
  `list_topics` calls use an actor/capability/scope session plus five-minute
  totals; see [the audit policy](../governance/audit-trail.md#audit-policy).
  Convention: reads
  require `api:query`; each action requires `orchestration:<name>`
  (`orchestration:create_project`). Handlers that reach the gate/deploy/run
  entry points inherit their enforcement too (RULE-1 cannot be bypassed by
  construction, because handlers call the same engine entry points the GUI
  calls).
- **Consent (shipped, #621 — [consent.md](consent.md)):** a `requires_consent`
  capability NEVER executes on first call — it returns a structured consent
  challenge naming exactly what would be read/run (the Cloud Inspector
  grammar) plus a one-time challenge id. Confirmation must come from a
  **different, approval-capable principal** (an approver key via
  `approve_consent`, or a human in the headed app's approval surface) — the
  calling key repeating the call can never satisfy it. Approval executes
  exactly the named (frozen) action once. Per-action, never remembered,
  audited on both sides; the ledger surfaces as `system.consent_requests`.

## Long-running work — reuse the durable handles

**No new job infrastructure.** The engine already gives every long operation a
durable, replicated, queryable handle: a `Run` (`run:<uuid>`) or a
`ComputeJob` (`job:<uuid>`), visible in `system.runs` /
`system.jobs`. A `kind: job` capability submits and returns
`{run_id | job_id}` immediately; progress/outcome is read back through the
read surface. No request-scoped blocking, no bespoke async protocol, and MCP,
REST, SQL, and the GUI all observe the same lifecycle rows.

## Observability — every call is a span

Per [../architecture/observability.md](../architecture/observability.md):
`Registry::call` opens a span (capability, kind, actor, the caller's reserved
optional `intent` field, input digest) and closes it with status + duration +
result shape; every `ApiResult` carries `trace_id`/`span_id`/`duration_ms` so
the MCP response itself tells the agent what happened. Reading more with
`list_traces` / `get_trace` requires `cluster:admin`, because the ring contains
other actors' request content (#1408). Feedback remains available to ordinary
callers, with diagnostic attachments only for administrators. Inbound
`traceparent` is honored; without one the call starts a
fresh trace. `intent` is stripped before schema validation — every capability
accepts it.

## Errors — the no-throw boundary, mapped

Handlers are non-throwing and return `ApiResult { ok, value, error{code,
message, path} }`:

- Malformed JSON-RPC / unknown tool → JSON-RPC protocol errors (−32700/−32601/−32602).
- Schema-invalid tool input → −32602 with the validator's path-qualified reason.
  The check itself never throws (#1231): `contentEncoding: base64` is validated,
  any other encoding is refused at its field, and a schema the validator cannot
  compile answers `invalid_input` naming the problem instead of escaping
  `Registry::call`.
  The `ApiError.path` carries the field at fault as a JSON pointer — for a
  missing required property, the property itself (`/name`), not its parent —
  so a schema rejection has a `path` exactly like a handler's own failure.
  A capability may set `invalid_input_hint` to append its one-step way past
  the error for that input (`create_study` offers `from_defaults: true`,
  [#1217](https://github.com/oramasearch/zetta/issues/1217)).
- **Domain failures (gate blocked, authorize denied, engine status) are NOT
  protocol errors** — they return a successful `tools/call` whose result
  carries `isError: true` + the engine's actual reason verbatim (the MCP
  convention; agents read the reason and adapt). Never launder a
  path-qualified engine reason into "operation failed".

## Placement + build

`api` is a domain like any other: `include/zetta/api/` + `src/core/api/` +
`tests/api/` + `docs/api/`, namespace `zetta::api`. The registry, context, and
bridge live in **zetta-core** (no HTTP dependency). The HTTP/MCP server is
**Beast2** — the C++ Alliance's coroutine-native HTTP server on the SAME
capy/corosio stack zetta already vendors for `zetta::runtime`
([research/beast2-http.md](../research/beast2-http.md)) — vendored like
capy/corosio and confined to **one adapter TU**
(`src/core/api/http_server.cpp`, the API sibling of `cluster.cpp`'s
corosio-adapter rule): no beast2/http/Boost.JSON type may appear outside it,
and Boost.JSON ↔ nlohmann conversion happens once at that boundary. The server
builds into a **`zetta-api`** static library on top of zetta-core — the same
isolation SDL/CUDA get — consumed by `main` (headless + headed) and
`tests/api`. Its `io_context` runs on its own thread(s), separate from the
cluster's, and TLS later rides beast2's `https_server` (#466). The headed app
additionally passes an **activity hook**: any successful `mutates` call fires
it on the owning loop, and the GUI surfaces the action live with a
click-through deep link (#620,
[../gui/external-activity.md](../gui/external-activity.md)); the activity
feed (#622) needs nothing extra — the call's audit event is its record.

## Protocol scope (v1)

JSON-RPC 2.0 over `POST /mcp` (Streamable HTTP, single JSON responses;
SSE/streaming is a later slice): `initialize`, `notifications/initialized`,
`ping`, `tools/list`, `tools/call`. CORS on, loopback-origin default.

**Prompts are the progressive-disclosure layer** (`prompts/list`/`get`, plan
slice 8): a new user (or agent) should never need the full tool table to get
value. The set is layered — `using_this_cluster` (orientation: what this node
is, the three reads to run first), `run_use_case` (UC-1 end to end),
`simulate_user_test` (drive a journey, assert on traces + system.*),
`author_use_case` (the add-use-case flow), `investigate` (pull traces/errors
for a misbehaving operation), `submit_feedback` (bundle a bug/feature report
with its traces). Each prompt teaches the next layer down; expert users
bypass them entirely.

## What the skeleton ships (#615) vs. defers

Ships: registry + bridge + server + auth + audit + activity hook + first
capabilities (`server_info`, `sql` — which alone exposes every `system.*`
view, `list_projects`, `create_project` as the action template) + headless and
headed embedding + real-HTTP tests. Defers: REST rendering (#618 — since
shipped, [rest-openapi.md](rest-openapi.md)), the remaining parity tools
(#616/#617 — since shipped), consent execution (#621), prompts, SSE,
key-secret minting.
