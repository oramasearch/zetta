# Plan: the MCP + REST control surface

Status: proposed · 2026-07-02 · tracking: epic [#614](https://github.com/oramasearch/zetta/issues/614)
(slices #615–#621; UX companions #622–#626) · realizes [product/control-surfaces.md](../product/control-surfaces.md)
(API-6 / API-8; sets up FEAT-13). Reference implementation for the pattern:
worktree-manager's `wtm-mcp` (Streamable HTTP JSON-RPC on `POST /mcp`,
cpp-httplib, loopback + bearer token, full dashboard parity, prompts as the
methodology layer, server embedded in the GUI with a live-refresh activity
hook).

## Current state (what we already have)

- **Engine entry points for nearly every product action** — project/input/graph
  CRUD, `compile_intent` / `compile_n8n_workflow` / `PythonFrontend`,
  `default_gate`, `deploy_graph`, `run_graph` / `run_churn_solution_on_node`,
  `QueryService` (SQL incl. every `system.*` view), `add_dataset_file`,
  `add_nodes` (consent-gated), `telemetry_snapshot`, the whole
  `testkit::TestSuite` catalog — all headless, all no-throw, all
  boundary-validated. The MCP surface is mostly *wiring*, not new engine work.
- **Governance done once**: capability-scoped keys (RULE-3), the gate (RULE-1),
  per-action cloud consent, one audit trail. The governed-console pattern
  (`repl::GovernedConsole`) is the template for a governed remote statement.
- **Schema-as-code** everywhere — tool input/output schemas can be generated
  from the same embedded schemas the boundaries already validate.
- **No HTTP server in the node yet** (the cluster speaks its own TCP protocol).
  **Beast2** (C++ Alliance, coroutine-native, on the capy/corosio stack we
  already vendor) is the chosen HTTP layer — see
  [../research/beast2-http.md](../research/beast2-http.md); settled design in
  [../api/registry-design.md](../api/registry-design.md).

## Architecture (mirrors zetta's existing seams)

- **`zetta-api` as a domain**: `include/zetta/api/` + `src/core/api/` +
  `tests/api/` + `docs/api/` — namespace `zetta::api`. Holds the HTTP server,
  the MCP protocol layer, and the tool/route registry. zetta-core stays
  SDL-free; the API layer links zetta-core like the GUI does.
- **One registry, three renderings**: each capability is registered once
  (name, input schema, handler over the engine entry point, `mutates` flag,
  required capability) and exposed as (a) an MCP tool, (b) a REST route, and
  (c) later, CLI verbs (API-7). Tool inputs validate against the registered
  schema at the boundary (the pboettch pattern — same validator, same
  discipline).
- **Auth = the existing identity model**: a bearer token maps to an
  `AccessKey`; every call is `Node::authorize`d (deny-by-default) and audited
  (`data_access` / the action's own audit event). An unauthenticated loopback
  session mirrors the console's "unauthenticated local" (still audited).
  Cloud-touching tools carry the consent gate — deny-by-default, per-action,
  named commands — exactly like the GUI.
- **Embedded + headless**: the server runs inside the headed app (a background
  thread over the same `Node`) and in `--headless` nodes. A mutating call fires
  an activity hook the GUI subscribes to, so externally-driven actions render
  immediately (the "watch the agent make it dance" loop).
- **Prompts as the methodology layer**: `operate_this_cluster` (orientation),
  `run_use_case` (UC-1 end to end), `simulate_user_test` (drive a journey,
  assert on `system.*`), `author_use_case` (the add-use-case flow). Prompts map
  to blueprint artifacts and to testkit journeys, not to ad-hoc behavior.

## Slices

1. **HTTP server + MCP skeleton** — Beast2 (vendored, one adapter TU behind
   the seam); `POST /mcp` JSON-RPC (initialize/list/call), loopback + bearer
   token, CORS; embedded in headed + headless. Acceptance: `tools/list` from
   Claude Code against a running node.
2. **Read parity** — tools/routes over every `system.*` view + entity detail
   reads (project/graph/run/model/job/dataset/index/audit). Read-only, cheap,
   safe. Acceptance: an agent answers "what is this cluster doing?" without the
   GUI.
3. **Governed action parity** — create_project / capture_intent / import_n8n /
   compile (intent · n8n · sim-Python) / gate / deploy / run / sql / add_dataset
   / testkit_run; each capability-scoped + audited; `mutates=true` fires the
   activity hook. Acceptance: UC-1 driven end to end from Claude Code over MCP
   on a live multi-node cluster.
4. **REST rendering** — the same registry served as plain HTTP routes
   (`/v1/...`), OpenAPI document generated from the registered schemas.
5. **Simulated user tests** — MCP-driven journeys recorded as replayable
   testkit cases (the example-scripts pattern, driven remotely); wired into the
   suite so product ergonomics regress loudly.
6. **GUI activity surfacing** — the activity hook renders externally-initiated
   actions in the headed app (feed + the existing cluster/graph animations
   picking up MCP-triggered runs).
7. **Cluster ops (consent-gated)** — add_nodes / provisioner detection /
   cloud inspect behind the same per-action consent grammar as the GUI; a
   remote caller receives the consent question, never a silent yes.
8. **Prompts** — the methodology layer above, versioned in-repo.

Out of scope here: FEAT-13 personalization (layers on top), the CLI rendering
(API-7, falls out of the registry), Postgres wire protocol.

## Risks / open questions

- **Consent over a remote surface**: the GUI's ask-every-time gate needs a
  remote equivalent that cannot be auto-approved by the calling agent —
  likely: the tool returns a consent challenge that must be confirmed by a
  distinctly-privileged key (or in the GUI), never by repeating the call.
- **Long-running actions**: runs/bursts need job-handle semantics (submit →
  poll `system.jobs` / subscribe) rather than request-scoped blocking.
- **Multi-node**: a tool call lands on one node; SPMD co-launch across peers
  is the same open slice the interactive GUI run has (#601) — the MCP surface
  should not invent a second answer.
