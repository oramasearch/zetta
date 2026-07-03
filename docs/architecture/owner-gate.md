# The owner-thread gate — driving engine runs from a background worker

**Issue:** [`#624`](https://github.com/oramasearch/zetta/issues/624) ·
**Code:** `Node::run_on_owner` / `Node::on_owner_thread` / `Node::drain_owner_work`
(`include/zetta/node.hpp`, `src/core/node.cpp`) · gated write sites in
`src/core/pipeline/executor.cpp`, `src/core/usecases/churn/churn_step_executor.cpp`,
`src/core/compute/job_coordinator.cpp` (`set_owner_gate`), `src/core/ml/gbdt_train_job.cpp` ·
async handles `usecases::churn::AsyncChurnRun` (`churn_solution.hpp`) and
`data::AsyncDatasetAdd` (`add_dataset.hpp`)

## The problem it solves

The `Node` tree is **owned by one thread** — the loop that drives `tick()`
(the headed frame loop, or the headless tick loop). Everything that mutates or
reads the tree must happen there. That rule kept the engine simple, but it also
meant every interactive engine action ran **synchronously on the UI thread**:
a churn run froze the window for its full featurize → train → explain → rank
duration, a large dataset add for its whole decode + materialize.

The API surface already solved the inverse problem with the
`api::MainThreadBridge` (a server coroutine `co_await`s work onto the node
loop). The owner gate is the same decision moved **down to the Node**, so any
engine runner — not just the HTTP adapter — can execute on a worker thread
while its node writes land safely on the owner loop.

## The seam

```cpp
node.run_on_owner([&] { job = node.create_job(...); });
```

- **On the owner thread** the work runs **inline** — zero cost, zero behavior
  change for every existing synchronous caller (the console, testkit, the
  acceptance tests).
- **On any other thread** the work is queued (a mutex-guarded deque — the
  contained cross-thread handoff the coroutines-first convention sanctions)
  and the caller **blocks** until the owner loop has executed it. The wait
  rides the runtime seam: a `runtime::async_event` awaited to completion via
  `block_on` — no caller-facing condition variables.
- **`Node::tick()` drains the queue first thing**, before the `running_`
  check, and adopts the ticking thread as the owner — so whichever loop drives
  the node is the gate's executor, and a gated worker never waits on a loop
  that is ticking. `drain_owner_work()` is public for the one other legitimate
  drain point: a teardown path joining a gated worker (see `~AsyncChurnRun`,
  which would otherwise deadlock — the owner waiting on a worker that is
  waiting on the owner).

`work` must be quick (it executes inside a tick) and must not throw. Calling
from a non-owner thread requires a live owner loop.

## The gated write sites (the churn-run path)

A run driven from a worker touches the node at a **bounded set of
chokepoints**, each now wrapped in the gate:

| Site | Writes |
|---|---|
| `pipeline::GraphExecutor::execute` | `create_run` + every `Run` mutator (status, logs, outputs, lineage); also takes an optional between-steps **cancel** flag |
| `usecases::churn::ChurnStepExecutor` | `create_job` ×4, `create_index`, the index-registry resolve |
| `compute::JobCoordinator::run` | the job's trace stamp + Running/terminal `set_status` — via `set_owner_gate`, wired by `Node::start_coordinator` over `run_on_owner` (unset = inline, for coordinators with no node behind them) |
| `ml::run_train_job` | `create_model` + `set_weights` (the root's Model registration) |

Span recording stays ungated (`SpanRecorder` is internally mutex-guarded), and
the heavy compute between the writes — DDF ops, GBDT rounds, TreeSHAP, the
collectives — runs on the worker.

**The payoff:** because the writes land on the live node *as the run
executes*, the Run's status + logs (`system.runs`) and its ComputeJobs
(`system.jobs`) are a **live progress source** — the GUI panel rendering them
and an MCP caller polling them through `sql` watch the same rows (#614's
job-handle semantics, satisfied without new job infrastructure).

## The async handles

Two core handles package the pattern (headless-testable, GUI stays a thin
caller):

- **`AsyncChurnRun`** (`churn_solution.hpp`) — runs
  `run_churn_solution_on_node` on a dedicated worker; snapshot-by-value
  inputs; cooperative cancel checked **between graph steps** (the in-flight
  step completes; the run ends Failed with a "run cancelled by user" log);
  destructor requests cancel and joins, pumping `drain_owner_work()`.
- **`AsyncDatasetAdd`** (`add_dataset.hpp`) — splits `add_dataset_file` into
  its two natural phases: the **decode** (pure file work) runs on a
  `runtime::spawn` worker with live row counts + cancel, and the
  **materialize** (table create, row adds, partition write, audit event) runs
  on the owner thread in bounded `pump()` slices. A cancel mid-materialize
  drops the partial table. The partition write is a single slice today —
  chunking inside `write_partitioned` is a later slice.

Completed adds and consent-approved cloud inspects now record audit events
(`dataset_added`, `cloud_inspected`), so background completions surface in the
activity feed like every other action.

## Boundaries

- **No new JSON format.** The audit-action enum gained two spellings inside
  the existing validated AuditEvent schema (`schemas/audit_event.schema.json`,
  regenerated); everything else rides existing boundaries.
- **Deliberately later:** a `progress` fraction + `Cancelled` state on the
  ComputeJob record (today `status` / `detail` / the Run's logs carry
  progress); killing an in-flight provider CLI (an inspect has no cancel);
  chunked partition writes; the MCP `kind: job` submit capabilities
  (#616/#617 — they will reuse these same handles + records).

The GUI-side affordances are described in
[../gui/long-running-actions.md](../gui/long-running-actions.md).
