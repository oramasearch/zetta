# In-app test suite (`zetta::testkit`)

A headless-runnable catalog of **canned, parameterized test cases** that exercise
zetta's real primitives end to end — compute / device operators, DDF ops, the ML
pipeline, distributed / cluster behavior over a **real multi-node loopback cluster**,
governance (access + audit + the correctness gate), and the whole UC-1 churn
pipeline — plus the headed **Test suite** panel that runs them interactively (issue
#462). Built on the [UC-1 end-to-end](uc1-end-to-end.md) (#399).

```
include/zetta/testkit/test_suite.hpp   TestCase / TestOption / TestOptions /
                                       TestResult / TestSuite + run_test_case
include/zetta/testkit/harness.hpp      ScratchDir, LoopbackCluster, the in-code
                                       synthetic churn dataset + numeric encoding
src/core/testkit/test_suite.cpp        the engine (options, result, runner, builtin)
src/core/testkit/harness.cpp           the loopback cluster + dataset helpers
src/core/testkit/cases_*.cpp           the canned cases, one file per category
src/gui/app.cpp (draw_test_suite)      the headed renderer
tests/test_testkit.cpp                 runs every representative case headless
```

## The model — catalog in core, GUI as renderer

The catalog lives in **zetta-core**, so it is runnable headless and carries no SDL
dependency; the GUI is a thin renderer over it (the same rule SDL/ImGui already
follow). A `TestCase` is:

- an `id` (stable, dotted — e.g. `compute.cpu.elementwise`), a `TestCategory`, a
  name + description;
- typed **`TestOption`s** — the per-test "options modal" (bool / int / double /
  choice / text, with defaults + bounds);
- a `run(TestOptions, TestContext) -> TestResult` body that calls the real engine.

`TestSuite::builtin()` assembles the catalog once. `run_test_case` times the body and
**converts any thrown exception into a clean `Error` result**, so a misbehaving case
can never take the app down. A `TestResult` carries an outcome (`Pass` / `Fail` /
`Skipped` / `Error`), a one-line summary, key/value **metrics**, a step **log**, and
the elapsed time. A `TestContext` carries an optional **custom dataset path** (a churn
dataset directory the data-driven cases load instead of the built-in synthetic one)
and a progress-log sink.

## The cases

| Category | Cases | What they check |
|----------|-------|-----------------|
| **Compute & device** | `compute.cpu.elementwise`, `compute.cpu.reduce_sort`, `compute.cuda.conformance` | CpuDevice operators (map / elementwise / reduce / sort_by_key) against the host; the CUDA build state (skipped on a CPU-only build) |
| **DDF** | `ddf.scan_filter`, `ddf.group_by` | scan a table into a typed columnar frame, filter via the device gather op, and group-by aggregate — checked against a host computation |
| **Machine learning** | `ml.gbdt_train`, `ml.tree_shap`, `ml.calibration` | train a histogram GBDT (AUC); exact TreeSHAP (the efficiency identity `base + Σφ = raw` + the dominant driver); probability calibration (Platt / isotonic) |
| **Distributed & cluster** | `cluster.membership`, `cluster.allreduce`, `cluster.group_by_consistency` | a real N-node loopback cluster forms; an allreduce returns the identical global result on every node; a distributed group-by equals the single-node computation |
| **Governance** | `governance.access_control`, `governance.audit`, `governance.entity_resolution_gate` | deny-by-default key-scoped access; the immutable audit trail; the entity-resolution gate (FullName passes, Brand conflates the FedEx siblings and is blocked — TC-4 / RULE-1) |
| **UC-1 pipeline** | `pipeline.uc1_end_to_end` | the whole gated + governed churn pipeline (the [#399 end-to-end](uc1-end-to-end.md)) as one runnable case |

Each case declares options (e.g. element count, GBDT trees / depth, node count,
resolver strategy, calibration on/off) so a run is tunable from the panel.

## Real multi-node, not a simulation

The distributed cases run on a `LoopbackCluster`: N real `Node` + `Cluster`
instances on `127.0.0.1`, brought up sequentially (each dials the actually-bound
addresses of the earlier ones, robust to busy ports) and waited until membership
converges — the same substrate the distributed gtest suites use. A SPMD case spawns
a thread per node calling the collective / distributed op concurrently, then asserts
every node agrees (and, for group-by, that the distributed result equals the
single-node ground truth). Cases are **self-contained**: they stand up their own
ephemeral nodes (own temp dirs + ports) and never disturb the app's live node.

## Self-contained data

The ML / governance / pipeline cases use an **in-code synthetic churn dataset**
(`testkit::sample_churn_dataset`) — the FedEx Freight + sibling subsidiaries
resolution case (TC-4), churn correlated to failed payments, and a couple of
deliberately post-`as_of` events (the leakage trap) — so the catalog runs from any
binary with no on-disk fixture. The **Custom dataset dir** field (a churn dataset
directory loaded via `load_churn_dataset`) overrides it for the data-driven cases.

No new JSON / wire format is introduced — custom-data ingestion rides the existing
schema-validated [churn dataset](churn-dataset.md) boundary, and every case calls the
already-validated engine boundaries.

## The headed panel

**Operations → Test suite** (`App::draw_test_suite`) renders the catalog grouped by
category (left), and a detail pane (right) with the selected case's description, its
options rendered as widgets, the custom-data field, a **Run test** button, and the
last result — an outcome badge (semantic dot + word), the summary, a mono metrics
table, and the log. A case runs on a **worker thread** (so the distributed cases'
blocking convergence waits never freeze the UI); the result + live log are written
under a mutex and rendered as they arrive. Following the Orama design system: sentence
case, mono for ids / counts / metrics, semantic green/amber/red for status, a charcoal
primary Run button.

## Run it

```bash
# headless (every representative case, including the real loopback-cluster cases):
ctest --test-dir build -R Testkit --output-on-failure

# interactive: launch the headed app and open Operations -> Test suite.
./build/bin/zetta
```
