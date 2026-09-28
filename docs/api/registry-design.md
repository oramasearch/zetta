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

## Threading — the main-thread bridge (the key decision)

The `Node` tree is **main-thread-only** (the same invariant the network thread
obeys). The HTTP server runs on its own thread, so **no handler may touch the
`Node` directly**. The bridge:

- `zetta::api::MainThreadBridge` — a mutex-guarded queue of packaged tasks.
  The owning loop **drains it**: `run_headless`'s tick loop and the headed
  app's frame loop both call `bridge.drain()` right where `Node::tick()`
  already runs. An HTTP handler posts a closure and blocks on its future with
  a timeout (default 10 s; a saturated loop returns a clear busy error, never
  a hang).
- Consequence: handler latency is bounded by the loop cadence (≤ ~100 ms
  headless at 10 Hz, one frame headed). That is the correct price for never
  violating the ownership invariant — the alternative (locking the tree) is
  against the architecture and is not on the table.
- Everything routes through the bridge, including reads that could arguably
  go direct — uniformity beats cleverness here; the thread-safety table lives
  in one place.
- **The one exception: an `offloadable` capability** (`chat`, `receive_events`'
  long poll) runs its *handler* on a pool worker so it never pins the loop. The
  call's authorization + audit gate still runs on the owner thread, through
  `Node::run_on_owner`: the gate reads the access keys and appends to the audit
  trail, which the tick reconciles and re-sorts. Running the gate on the worker
  crashed lake's bus Zetta every few minutes (#1063). An offloadable handler may
  touch only thread-safe state (the replication store, `PubSub`), and
  `Registry::add` refuses an offloadable entry that mutates or requires consent,
  because the activity hook and the consent ledger run after the gate.
- **Owner hops wake the loop.** Work a pool worker queues with
  `Node::run_on_owner` (that gate, the pub/sub authorizer, background actions)
  wakes the idle loop through `Node::set_owner_wakeup` (the API server's
  `wake()`, a `MainThreadBridge::poke`), and the loop drains it on that wake,
  not only inside the 10 Hz `tick()`. Without both, each hop waited out a tick:
  `receive_events` took ~205 ms with no wait at all; it now takes under 1 ms.

## Auth, governance, audit — nothing new

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
- Every call runs `Node::authorize(key, required_operation)` — deny-by-default
  capability check **and** the audit record in one motion. Convention: reads
  require `api:query`; each action requires `orchestration:<name>`
  (`orchestration:create_project`). Handlers that reach the gate/deploy/run
  seams inherit those seams' own enforcement too (RULE-1 cannot be bypassed
  by construction, because handlers call the same engine entry points the GUI
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
the MCP response itself tells the agent what happened and where to read more
(`get_trace`). Inbound `traceparent` is honored; without one the call starts a
fresh trace. `intent` is stripped before schema validation — every capability
accepts it.

## Errors — the no-throw boundary, mapped

Handlers are non-throwing and return `ApiResult { ok, value, error{code,
message, path} }`:

- Malformed JSON-RPC / unknown tool → JSON-RPC protocol errors (−32700/−32601/−32602).
- Schema-invalid tool input → −32602 with the validator's path-qualified reason.
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
