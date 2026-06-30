# UC-1 at scale (add nodes, load, train, explain, rank) · `examples.uc1_at_scale`

**Category:** Example scripts · **Source:** `src/core/testkit/cases_examples.cpp` · **Engine doc:** [example-scripts.md](../../example-scripts.md)

The capstone UC-1-at-scale program as a lightweight example: add permanent nodes,
load the saas-sales-conversations parquet, train on `outcome`, explain, and rank. It
shares its source with the real [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)
case (one canonical program, `uc1_at_scale_script()`); here the standard example
harness asserts the **lowered shape + a green single-node run**.

## What it does

The example harness runs the same canonical program the real cluster case runs,
through parse → IR → gate → run (see [uc1-at-scale-e2e.md](../../uc1-at-scale-e2e.md)):

1. **Compile.** The [Python frontend](../../python-frontend.md) lowers the source to
   the [ExecutionGraph IR](../../execution-graph.md):

   ```python
   nodes = cluster.add_nodes(10, provider='azure')
   conversations = load_dataset('saas-sales-conversations', columns=['messages',
       'avg_response_seconds', 'sentiment', 'questions_asked', 'outcome'])
   model = train(conversations, target='outcome')
   drivers = explain(model)
   at_risk = rank(model)
   ```

   It lowers across the `connect → … → act` taxonomy: `cluster.add_nodes` → an `Act`
   control step, `load_dataset` → `Connect`, `train` → `Model`, `explain` → `Serve`,
   `rank` → `Index`; the variable refs become the data-flow edges
   (`load_dataset → train`, `train → explain`, `train → rank`).
2. **Gate.** [`default_gate()`](../../gate.md) must pass (RULE-1).
3. **Run.** `Node::run_graph` executes the graph **single-node** (the
   [`DefaultStepExecutor`](../../execution.md)) to a `Succeeded` Run.

This example proves the program is real, well-formed sim-Python; the at-scale, real
multi-node, near-data, governed run over a cluster is the
[`cluster.uc1_at_scale`](cluster.uc1_at_scale.md) case.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Example scripts → UC-1 at scale (add nodes, load, train, explain, rank)**, then
click **Run** (this case takes no options). The result pane shows pass/fail, the
metrics (steps, edges, gate checks, run status, outputs), and the log.

**Headless:** the example cases are exercised by
`tests/test_testkit.cpp::ExampleScriptCasesRunGreen` and by
`tests/test_example_scripts.cpp`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ExampleScriptCasesRunGreen|ExampleScripts' --output-on-failure
```

### Options

This case takes no options. (The real, heavy run is sized by the `nodes`/`rows`
options on [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md).)

## What it asserts (pass criteria)

- **Lowered shape** — 5 steps and 3 edges; `add_nodes` → `Act`, `load_dataset` →
  `Connect`, `train` → `Model`, `explain` → `Serve`, `rank` → `Index`, with the
  data-flow edges `load_dataset → train`, `train → explain`, and `train → rank`
  present.
- **Gated** — `default_gate()` passed.
- **Ran green** — the single-node run reached `Succeeded` and produced `dataset`,
  `model`, and `index` outputs.

## Related

- Engine: [example-scripts.md](../../example-scripts.md) ·
  [uc1-at-scale-e2e.md](../../uc1-at-scale-e2e.md) ·
  [python-frontend.md](../../python-frontend.md) · [gate.md](../../gate.md)
- Sibling cases: [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md) ·
  [`examples.churn_pipeline`](examples.churn_pipeline.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md)
