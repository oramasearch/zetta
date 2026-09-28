# Governed action parity — the product loop over MCP

The mutating half of the control surface (#617, the sibling of the #616 read
parity): the whole product loop — project → intent → compile → gate → deploy →
run → results read back — drivable from any MCP client with the same
governance as the GUI. Design rationale:
[registry-design.md](registry-design.md); recipe:
[adding-a-capability.md](adding-a-capability.md).

## The parity rule, realized

Every action is **one registry entry, thin over the SAME engine entry point
the GUI calls**, so RULE-1/RULE-3 cannot be bypassed by construction:

| Tool | Engine entry point | Notes |
|---|---|---|
| `capture_intent` | `Project::capture_intent` | provenance recorded; rejects empty intent |
| `import_n8n` | `pipeline::import_n8n_workflow` | schema-validated at the boundary, stored byte-for-byte |
| `compile_intent` | `Project::compile_intent_to_graph` | one-shot compile + store; audits `graph_created` |
| `compile_n8n` | `pipeline::compile_n8n_workflow` → `Project::create_graph` | engine reasons verbatim (`nodes[2]: unsupported…`) |
| `compile_python` | `input::PythonFrontend::compile` → `Project::create_graph` | lowers sim-Python; never runs Python |
| `approve_plan` | `Project::approve_graph` | review-before-build (#885): flips a proposed plan → approved; audits `graph_approved`; `gate`/`deploy` require it |
| `gate` | `pipeline::default_gate().run` | returns the whole `ValidationReport` (checks + verdict + block_deploy); evaluation only, `mutates=false`; refuses a `proposed` plan with `not_approved` |
| `deploy` | `Node::deploy_graph` | the engine self-gates (RULE-1) + self-audits both outcomes; the handler pre-runs the gate only to surface the failing check's reason verbatim |
| `run` | the selected methodology's registry callback · `Node::run_graph` for structural graphs | see below |
| `add_dataset` | `data::add_dataset_file` | placement over the live `cluster_members()`; audits `dataset_added` |
| `testsuite_run` | `testkit::run_test_case` via `testkit::AsyncCaseRun` | see below |

Governance is uniform (RULE-3): each action requires
`orchestration:<tool-name>` on the calling key; `Registry::call` authorizes
(deny-by-default) and audits every call — allowed or denied — under the acting
key, then `mutates=true` fires the activity hook on success so an embedding
GUI refreshes. Errors are never protocol errors: a gate block, a compile
failure, an unknown entity come back as `isError: true` results carrying the
engine's path-qualified reason verbatim.

**Gate parity.** `gate` reports; `deploy` and `run` enforce. A blocked graph
fails `deploy` with `gate blocked deploy: <check>: <reason>` (the engine's
`deploy_graph` re-runs the gate internally and audits `graph_deployed
allowed=false` — the handler cannot skip it) and fails `run` with `blocked by
gate: …` (the governed-console precedent; the coordinated launch re-checks the
gate on every member).

## Long-running work — `kind: job`, no new infrastructure

`run` (churn-shaped graphs) and `testsuite_run` return **the engine's durable
Run id immediately** and never block the request on engine work
(registry-design.md → "reuse the durable handles"). Progress and outcome are
polled through the read surface: `get_run` / `system.runs` for status + logs,
`system.jobs` for the per-step compute jobs.

- **`run`** — a churn-shaped graph (`is_churn_graph`) routes through THE
  canonical runner (#637) on the node's **live** JobCoordinator, driven
  off-thread by `AsyncChurnRun` (#624): SPMD across the live membership via
  the coordinated launch (#601), the trained Model + ranked at-risk list +
  drivers landing on the node. The Run record is created by the worker
  through the owner gate, so the handler — which runs ON the owner thread
  (the bridge contract) — pumps `Node::drain_owner_work()` until the record
  exists, then returns its id (milliseconds on the demo dataset; a run that
  fails at submit, e.g. no live coordinator, surfaces its engine reason
  instead). The run's outputs carry the deliverables: the `model` output, the
  `index` output (the score/rank at-risk handle), and the `drivers:<job>`
  dataset ref. A non-churn graph runs **structurally** (DefaultStepExecutor)
  within the call and returns its run id + terminal status directly. M-17 reads
  the connect step's own table, fits its grouped posterior, writes the output
  table and Run synchronously, and returns the succeeded Run id; it does not
  enter the structural path.
- **`testsuite_run`** — `testkit::AsyncCaseRun`
  (`include/zetta/testkit/async_case_run.hpp`) creates the Run on the owner
  thread (project label `testkit`, graph label = the case id) so the id is
  known at submit, marks it Running, and executes the case on a worker (case
  bodies never touch the live node). The case's log lines, metrics, and
  verdict land on the Run with one gated completion flush; Pass/Skipped →
  Succeeded, Fail/Error → Failed. Per-case option overrides are validated
  against the case's declared options with path-qualified reasons
  (`/options/<key>`).

**Handle ownership.** The in-process worker objects live in
`api::BackgroundActions` (`include/zetta/api/background.hpp`): a job handler
adopts the handle, and the owning loop prunes finished ones inside
`ApiServer::drain()` — on the owner thread, where a handle's destructor may
join its worker. An `ApiContext` without a `background` owner refuses job
capabilities with a clear reason (bare in-process embeddings opt in by wiring
one).

## Boundaries

No new JSON format: every input rides the registered draft-07 tool schema
(the pboettch boundary in `Registry::call`), and everything the handlers
store rides existing validated boundaries — the n8n workflow schema, the IR's
`ir::from_json`, the Run/ComputeJob/DatasetPartition records. The one new
validation surface is `testsuite_run`'s per-case option typing, checked against
the case's own declared `TestOption`s.

## Journeys

The journeys are DATA (#619): `Journey{JourneyStep{tool, input, expect}}`
tables run by the shared driver `tests/api/journey.hpp`, with the canonical
three in `tests/api/journeys_canonical.hpp`
([../testing/mcp-journeys.md](../testing/mcp-journeys.md)).
`tests/api/test_action_journeys.cpp` runs them at layer 1: the UC-1 happy
path end to end under a capability-scoped acting key (create → intent →
compile → gate → deploy → run on the real churn engine → model/at-risk
index/drivers read back, the near-data trace read-back via the train job's
`get_trace`, every action audited under the key), the gate-blocked negative
(deploy AND run refused with the failing check's reason; read-backs prove
nothing placed, nothing executed), import/compile n8n from the committed
fixture, sim-Python compile, `add_dataset` with placement read-back,
`testsuite_run` submit-then-poll, and the denied-key negative.
`tests/api/test_mcp_server.cpp` replays the dataset-ingest and gate-blocked
tables unchanged over real JSON-RPC (layer 2). Deliberately later slices:
the L2 protocol journey for job polling over real HTTP, the #621 L2 consent
journey over real HTTP with two bearer tokens (the challenge protocol itself
shipped — [consent.md](consent.md)), streamed (rather than
completion-flushed) testkit logs, and an async `add_dataset` for very large
files (the GUI's `AsyncDatasetAdd` pumped from the drain).
