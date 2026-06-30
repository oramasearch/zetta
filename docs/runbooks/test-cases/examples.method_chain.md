# Method chaining · `examples.method_chain`

**Category:** Example scripts · **Source:** `src/core/testkit/cases_examples.cpp` · **Engine doc:** [example-scripts.md](../../example-scripts.md)

Proves a chained transform lowers to a **linear data-flow path**, each method its own
step. `read_csv(...).dropna().fillna(0)` → `fit` becomes a four-step chain that
compiles, gates, and runs green.

## What it does

The example harness runs the script through parse → IR → gate → run (see
[example-scripts.md](../../example-scripts.md)):

1. **Compile.** The [Python frontend](../../python-frontend.md) lowers method chaining
   to the [ExecutionGraph IR](../../execution-graph.md), each chained method a step
   whose receiver is its upstream producer:

   ```python
   df = read_csv('accounts.csv').dropna().fillna(0)
   model = fit(df)
   ```

   Four steps (`read_csv` → `Connect`, `dropna` → `Clean`, `fillna` → `Clean`, `fit`
   → `Model`) along three data-flow edges `read_csv → dropna → fillna → fit`.
2. **Gate.** [`default_gate()`](../../gate.md) must pass (RULE-1).
3. **Run.** `Node::run_graph` executes the graph on an ephemeral scratch node to a
   `Succeeded` Run.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Example scripts → Method chaining**, then click **Run** (this case takes no
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

- **Lowered shape** — 4 steps and 3 edges; `read_csv` → `Connect`, the chained
  `dropna`/`fillna` → `Clean`, `fit` → `Model`, and the linear data-flow path
  `read_csv → dropna → fillna → fit` present (the method receiver becomes the edge).
- **Gated** — `default_gate()` passed.
- **Ran green** — the run reached `Succeeded` and produced `dataset` and `model`
  outputs.

## Related

- Engine: [example-scripts.md](../../example-scripts.md) ·
  [python-frontend.md](../../python-frontend.md) · [gate.md](../../gate.md) ·
  [execution.md](../../execution.md)
- Sibling cases: [`examples.minimal_train`](examples.minimal_train.md) ·
  [`examples.churn_pipeline`](examples.churn_pipeline.md)
