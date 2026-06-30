# Minimal load + train · `examples.minimal_train`

**Category:** Example scripts · **Source:** `src/core/testkit/cases_examples.cpp` · **Engine doc:** [example-scripts.md](../../example-scripts.md)

The smallest green example (the #520 frontend acceptance script): load a dataset and
train on it. Two steps, one data-flow edge — the canonical "runs green in the suite"
that proves the simulated-Python frontend, gate, and run path compose end to end.

## What it does

The example harness runs the script through parse → IR → gate → run (see
[example-scripts.md](../../example-scripts.md)):

1. **Compile.** The [Python frontend](../../python-frontend.md) lowers the source to
   the [ExecutionGraph IR](../../execution-graph.md):

   ```python
   ds = load_dataset('saas-churn')
   model = train(ds, target='outcome')
   ```

   Two calls → two typed steps (`load_dataset` → `Connect`, `train` → `Model`); the
   `ds` reference becomes the single data-flow edge `load_dataset → train`.
2. **Gate.** [`default_gate()`](../../gate.md) must pass (RULE-1).
3. **Run.** `Node::run_graph` executes the graph on an ephemeral scratch node to a
   `Succeeded` Run.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Example scripts → Minimal load + train**, then click **Run** (this case takes no
options). The result pane shows pass/fail, the metrics (steps, edges, gate checks,
run status, outputs), and the log.

**Headless:** the example cases are exercised by
`tests/test_testkit.cpp::ExampleScriptCasesRunGreen` and by
`tests/test_example_scripts.cpp`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ExampleScriptCasesRunGreen|ExampleScripts' --output-on-failure
```

### Options

This case takes no options.

## What it asserts (pass criteria)

- **Lowered shape** — 2 steps and 1 edge; `load_dataset` → `Connect`, `train` →
  `Model`, and the data-flow edge `load_dataset → train` present.
- **Gated** — `default_gate()` passed.
- **Ran green** — the run reached `Succeeded` and produced `dataset` and `model`
  outputs.

## Related

- Engine: [example-scripts.md](../../example-scripts.md) ·
  [python-frontend.md](../../python-frontend.md) · [gate.md](../../gate.md) ·
  [execution.md](../../execution.md)
- Sibling cases: [`examples.churn_pipeline`](examples.churn_pipeline.md) ·
  [`examples.method_chain`](examples.method_chain.md) ·
  [`examples.uc1_at_scale`](examples.uc1_at_scale.md)
