# Observability — every operation is observable

A foundation, not a feature: every operation in zetta captures **intent**
(what the caller asked for, verbatim), **plan** (what the engine decided to
do), **performance** (timed spans), and **result shape** (what came back) —
threaded by trace id across surfaces, threads, and nodes. Three audiences,
one system: **developers** iterating on the engine, **agents** driving the
cluster over MCP (a machine consumer of traces is a first-class consumer),
and **users** whose feedback bundles become our improvement loop.

## The model — OpenTelemetry conventions, extended

W3C/OTel where it fits, extended where DX/UX demands more:

- **Ids + propagation are pure OTel/W3C**: 16-byte trace id / 8-byte span id
  (hex), `traceparent` propagation. An inbound MCP/REST call may carry
  `traceparent`; every response returns `trace_id` + `span_id` + duration.
  Cross-node hops (signal-plane RPC, job dispatch) carry the context so a
  distributed run is ONE trace across the cluster.
- **Beyond OTel — the four captures on every span**:
  - `intent` — the caller's stated purpose, verbatim. Every API capability
    accepts a reserved optional `intent` string; the GUI records the acting
    affordance; the console records the statement. Intent is what makes a
    trace legible to a human (and lets us learn what users are *trying* to do,
    not just what they invoked).
  - `plan` — the engine's decision: the compiled graph summary, the cluster
    plan projection (near-data/fan-out placement), the SQL statement kind,
    the job split. Recorded when the operation has one.
  - `performance` — span timings; child spans per meaningful phase (gate,
    deploy, per-step, per-collective-round via the existing instrumentation).
  - `result_shape` — rows/columns counts, produced entity ids
    (run/model/job/dataset), output kinds — never full payloads by default.
- **Errors and crashes**: a failed span carries the engine's path-qualified
  reason (status=error, `error.message`). A **crash** (fatal signal) writes a
  crash report — stacktrace, active spans, version, platform — to
  `<data_dir>/crashes/<uuid>.json` before dying, surfaced on next start.

## Storage + surfaces (RULE-7 all the way down)

- Spans land in a **bounded in-memory ring** on the node (`zetta::observability`,
  ephemeral/observed, NOT replicated — the `system.node_metrics` /
  pipeline-instrumentation precedent), plus the crash files above.
- Read views: **`system.traces`** (one row per trace: root span, intent,
  actor, surface, duration, status) and **`system.spans`** (one row per span)
  — SQL-queryable, GUI-renderable, MCP-readable, one source.
- API capabilities `list_traces` / `get_trace` read the same ring — an agent
  that just ran a tool can immediately pull the full trace of what happened
  (the "MCP agent understands the cluster" loop). A per-entity "what
  happened" pane renders the same rows. The activity feed (#622,
  [../gui/activity-feed.md](../gui/activity-feed.md)) is the durable
  complement: `system.activity` merges the audit trail + runs
  chronologically, so "what was the cluster asked to do, and by whom" reads
  from one view on every surface.

## Feedback bundles — a cluster feature

`submit_feedback` (an API capability + a GUI affordance): the user (or agent)
describes a bug/feature wish; the node snapshots a **feedback bundle** — the
description, the relevant trace(s) with intents + spans, recent errors/crash
reports, versions, platform — as a large object the user can export and send
us. Observability isn't only for us debugging: it's the mechanism by which
users hand us reproducible reality instead of anecdotes. (Bundles are
explicit, user-initiated exports — nothing leaves the cluster on its own; the
no-egress posture is untouched.)

## The test suite is the flywheel

The testkit remains the catalog of ALL cluster functionality — and every
testkit case runs inside a trace. That gives iteration a memory: per-case
timing and result-shape history, spans for every phase, and failures that
arrive pre-annotated with intent + plan. MCP journeys assert on traces too
(did the run actually go near-data? one trace answers it). New functionality
lands with its testkit case, and both land observable.

## Build-out (tracked in the product wave)

1. `zetta::observability` core: trace/span types, `traceparent` codec, the bounded
   recorder, scoped-span helper — wired first into `api::Registry::call` (every
   MCP call = a span with all four captures; trace id returned in every
   result).
2. `system.traces` / `system.spans` read views + `list_traces` / `get_trace`
   capabilities.
3. Crash reports (fatal-signal handler + stacktrace + `system.crashes`).
4. Cross-node propagation (signal-plane envelope carries traceparent; runs +
   compute jobs record their trace id) — one trace per distributed run.
5. GUI: trace pane + activity feed integration; feedback bundle affordance.
6. `submit_feedback` capability + bundle export.

Related: [../api/registry-design.md](../api/registry-design.md) (trace
threading through every call), [../testing/mcp-journeys.md](../testing/mcp-journeys.md)
(journeys assert on traces), the audit trail (governance decisions — WHO was
allowed; traces record WHAT happened and HOW LONG it took; they reference,
never replace, each other).
