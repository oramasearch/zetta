# Sim-Python example-script harness (`zetta::testkit`)

A **headless test harness that runs sim-Python example scripts end to end as
integration tests** (issue #523). Each example program is run through the whole
frontend pipeline — **parse → IR → gate → run** — and its declared expectations are
asserted on the outputs, so *the example programs ARE the integration tests* ("run
all of these simulated python commands as examples in the test suite"). A green
script doubles as a canonical usage example; a failing script proves the harness
surfaces the **offending statement**.

```
include/zetta/testkit/example_script.hpp   ExampleScript / ExampleExpectations /
                                           ExampleRunReport + run_example_script +
                                           example_scripts()
src/core/testkit/example_script.cpp        the harness: parse→IR→gate→run + assert
                                           + offending-statement reconstruction
src/core/testkit/cases_examples.cpp        the built-in catalog + register_example_cases
tests/test_example_scripts.cpp             the harness, headless
tests/test_testkit.cpp                     the example cases in the catalog, headless
```

It builds directly on the [simulated-Python frontend](python-frontend.md) (#520),
the [correctness gate](gate.md) (RULE-1), and [graph execution](execution.md)
(`Node::run_graph`), and slots into the [in-app test suite](test-suite.md)
(`zetta::testkit`, #462) as a new `TestCategory::Examples`.

## The pipeline

`run_example_script(script)` drives one script through four phases and checks the
script's `ExampleExpectations` at each:

1. **Compile** — `zetta::input::PythonFrontend::compile(source)` lowers the
   sim-Python to the [ExecutionGraph IR](execution-graph.md). Optional structural
   assertions on the lowered graph (node/edge counts, that a given step lowered to
   the expected `StepKind`, that an expected data-flow edge exists) are checked
   here.
2. **Gate** — `default_gate().run(graph)` must pass (no run without a passing gate,
   RULE-1).
3. **Run** — `Node::run_graph(graph)` executes the graph on an **ephemeral scratch
   node** (its own temp dir, preflight off — never the app's live node) to a
   `Succeeded` Run. Optional assertions on the produced **outputs** (every listed
   kind — `dataset` / `model` / `index` — must appear) are checked here.
4. **Done** — every expectation held.

The harness is **non-throwing**: any exception from an engine path is converted to a
failing `ExampleRunReport` (the same contract the testkit `run_test_case` honors), so
a misbehaving script can never take the suite — or the app — down.

## Asserting on outputs

`ExampleExpectations` is a small in-code struct (the same shape as `TestOption` —
**no new JSON format**; the only JSON the harness touches is the already
schema-validated IR JSON form and the Run record). Every field is opt-in; a
default-constructed `ExampleExpectations` is the happy path (`compiles` +
`gate_passes` + `runs`, all true):

```cpp
ExampleScript script;
script.source =
    "ds = load_dataset('saas-churn')\n"
    "model = train(ds, target='outcome')\n";
script.expect.node_count = 2;
script.expect.edge_count = 1;
script.expect.steps = {{"load_dataset", StepKind::Connect},
                       {"train", StepKind::Model}};
script.expect.edges = {{"load_dataset", "train"}};
script.expect.output_kinds = {"dataset", "model"};

const ExampleRunReport report = run_example_script(script);
// report.ok == true, report.reached == ExamplePhase::Done
```

A **negative example** declares the phase that must fail (`compiles` / `gate_passes`
/ `runs` set to `false`) and a distinctive substring of the expected reason
(`failure_contains`). It "passes as a test" when it fails exactly as declared:

```cpp
script.source = "ds = load('unterminated\n";   // unterminated string literal
script.expect.compiles = false;
script.expect.failure_contains = "unterminated string";
// report.ok == true (it failed as expected; the offending statement is surfaced)
```

## The offending statement

When any phase fails, `ExampleRunReport::offending_statement` names the source
statement at fault:

- **Compile** — the parser's own non-throwing locator (e.g. `unterminated string`,
  `unexpected ')' after statement`, `no pipeline steps`).
- **Structural / gate / run** — the failing step's statement, **reconstructed from
  its `python` provenance bag** (the frontend records `call` / `assigned_to` /
  `args` / `kwargs` on every node). A mismatch on `train` surfaces
  `m = train(ds, target='outcome')` — verb, assignment, and arguments — so the
  failure reads back to the line a script author wrote. (The frontend keeps no
  source-line table, so the harness rebuilds a canonical, deterministic form rather
  than echoing raw text.)

When an example is registered as a `TestCase`, the offending statement is folded
into the `TestResult::error` (`… [at: m = train(ds, target='outcome')]`) and shown as
a metric, so a failure in the suite names the statement directly.

## The built-in catalog

`example_scripts()` is the built-in set, each registered as a `TestCase` under
`TestCategory::Examples` (so they run both headless and in the headed **Operations →
Test suite** panel):

| id | what it is |
|----|-----------|
| `examples.churn_pipeline` | the full `connect→clean→model→index→serve→act` taxonomy (load two sources → featurize → train → build index → serve → notify); compiles, gates, runs green |
| `examples.minimal_train` | the smallest green sample (load → train): 2 steps, 1 edge — the canonical "runs green in the suite" |
| `examples.method_chain` | `read_csv(...).dropna().fillna(0)` → fit; a chain lowers to a linear data-flow path |
| `examples.uc1_at_scale` | the UC-1-at-scale program (add nodes → load the saas parquet → train on `outcome` → explain → rank); asserts the lowered shape + a green single-node run. Its real distributed near-data run is the [`cluster.uc1_at_scale`](uc1-at-scale-e2e.md) case |
| `examples.unterminated_string` | negative: a syntax error fails at compile; the parser's locator is surfaced |
| `examples.no_pipeline_steps` | negative: a script with no calls is rejected (nothing to lower / run) |

## Growth path

The catalog grows as the bindings the larger test cases need land. The flagship
verification cases referenced by the issue —

- **TC-9** ([#499](https://github.com/oramasearch/zetta/issues/499)) parquet-reader
  parity, **TC-10** ([#500](https://github.com/oramasearch/zetta/issues/500))
  partition placement / rebalance, **TC-11**
  ([#501](https://github.com/oramasearch/zetta/issues/501)) compute-near-data,
  **TC-13** ([#503](https://github.com/oramasearch/zetta/issues/503)) fulltext index
  build + query, and **UC-1-at-scale** ([#531](https://github.com/oramasearch/zetta/issues/531)
  / TC-12) —

each become a green example script once their engine bindings exist: the
**cluster + ML sim-Python bindings** (`add_nodes` / `train` / `explain` / `rank` →
real IR, [#522](https://github.com/oramasearch/zetta/issues/522)), the parquet→DDF
wiring, the partitioned write path, and the fulltext backend. Today those verbs lower
to generic steps, so the harness ships with the green-pipeline samples + the
failure-surfacing negatives; authoring a new case is a `struct ExampleScript`
appended to `example_scripts()` (and, for a real-engine pipeline, a custom
`StepExecutor` is the natural next extension to `run_example_script`, the way
[`ChurnStepExecutor`](churn-step-executor.md) injects into a `GraphExecutor`).

Some of those cases land first as **non-example** test-suite cases, where the engine
binding already exists but the *sim-Python* verb does not. **TC-10** is the first:
the [partition rebalance](partition-rebalance.md) is a real engine path (#517), so it
ships now as the `cluster.partition_rebalance` distributed case
([test-suite.md](test-suite.md)) + the `CatalogConvergesOnEveryNodeAfterJoin` gtest,
not an example script — a sim-Python rendering follows whenever a partitioning verb
joins the [frontend](python-frontend.md).

## Run it

```bash
# the harness + the catalog, headless:
ctest --test-dir build -R "ExampleScripts|Testkit" --output-on-failure

# interactive: launch the app and open Operations → Test suite → Example scripts.
./build/bin/zetta
```
