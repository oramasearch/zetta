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
testkit case runs inside a trace (**shipped**, #640: `run_test_case` records a
`testkit:<case.id>` span per case with the result shape; the GUI panel records
into the live node's ring — see
[../testing/test-suite.md](../testing/test-suite.md)). That gives iteration a
memory: per-case timing and result-shape history, spans for every phase, and
failures that arrive pre-annotated with intent + plan. MCP journeys assert on
traces too (`JourneyRunToolThenReadTraceContent` in tests/api/test_registry.cpp
reads a call's trace back through get_trace AND the SQL views). New
functionality lands with its testkit case, and both land observable.

## Build-out (tracked in the product wave; #640)

1. **Shipped** — `zetta::observability` core: trace/span types, `traceparent`
   codec, the bounded recorder, scoped-span helper — wired first into
   `api::Registry::call` (every MCP call = a span with all four captures;
   trace id returned in every result).
2. **Shipped** — `system.traces` / `system.spans` read views + `list_traces` /
   `get_trace` capabilities. The ring lives on the Node
   (`Node::span_recorder()`, the pipeline-jobs precedent), so the SQL views,
   the capabilities, and the GUI all read one source. Columns + examples:
   [../data/query.md](../data/query.md).
3. **Shipped** — crash reports: `install_crash_handler`
   (`observability/crash.hpp`, installed in main for both run modes) writes
   stacktrace (execinfo backtrace) + recent spans (try-lock, never a wait) +
   version/platform to `<data_dir>/crashes/<uuid>.json` on SIGSEGV / SIGABRT /
   SIGBUS / SIGILL / SIGFPE — disposition reset to SIG_DFL first, so the write
   is best-effort and a second fault kills for real. Reports are
   schema-validated on read-back (`schemas/crash_report.schema.json`);
   `system.crashes` surfaces valid AND malformed files (valid=false + reason);
   start-up prints a pointer when prior reports exist. Capturing truly
   in-flight ("active") spans at crash time is a deliberate later slice — the
   report carries the ring's most recent finished spans.
4. **Shipped** — cross-node propagation: the signal envelope carries an
   optional `traceparent` (validated, all kinds); `Run` and `ComputeJob`
   records carry a replicated `trace_id` — the trace's anchor is the
   replicated record, so every node that runs the job joins ONE trace. The
   coordinator records the job root span (`job:<kind>`), each collective
   round and cross-node `map` a child span; steal offers carry the owner's
   traceparent, so a helper's execution of stolen tasks records a
   `steal:<method>` span in ITS ring joined to the OWNER's trace; the
   near-data runners record `near-data:<kind>` spans whose result shape is
   the pipeline counters (train's derived explain/score jobs inherit the
   trace); `GraphExecutor` stamps every run's trace and records `run:` +
   per-`step:` spans. Scope note: SPMD siblings that each create their OWN
   job record trace separately until an orchestrated fan-out shares one job
   record (first-stamper-wins on the CRDT).
5. GUI: trace pane + activity feed integration; feedback bundle affordance.
6. **Shipped** — `submit_feedback` capability: the bundle (description +
   attached traces with spans + recent error spans + crash reports +
   version/platform/node id) is built by
   `observability::build_feedback_bundle`, validated against its own schema
   (`schemas/feedback_bundle.schema.json`), and stored as a large object in
   the `feedback` database (replicated metadata, peer-transferred body). The
   optional `export_path` input writes the bundle to a caller-chosen local
   path — the explicit export; nothing leaves the cluster on its own.

Related: [../api/registry-design.md](../api/registry-design.md) (trace
threading through every call), [../testing/mcp-journeys.md](../testing/mcp-journeys.md)
(journeys assert on traces), the audit trail (governance decisions — WHO was
allowed; traces record WHAT happened and HOW LONG it took; they reference,
never replace, each other).
