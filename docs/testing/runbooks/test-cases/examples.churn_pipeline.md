# Churn pipeline (full taxonomy) · `examples.churn_pipeline`

**Category:** Example scripts · **Source:** `src/core/testkit/cases_examples.cpp` · **Engine doc:** [example-scripts.md](../../example-scripts.md)

The canonical end-to-end example: a churn-shaped sim-Python program spanning every
stage of the `connect → clean → model → index → serve → act` taxonomy. It runs
through the example harness (parse → IR → gate → run) and asserts the lowered graph
shape + that it executes green.

## What it does

The example harness runs the script through four phases (see
[example-scripts.md](../../example-scripts.md)):

1. **Compile.** The [Python frontend](../../../pipeline/python-frontend.md) lowers the source to
   the [ExecutionGraph IR](../../../pipeline/execution-graph.md):

   ```python
   accounts = load_dataset('saas-churn-accounts')
   events = load_dataset('saas-churn-events')
   features = featurize(accounts, events)
   model = train(features, target='churned')
   risk_index = build_index(model)
   api = serve(risk_index)
   notify(api)
   ```

   Each call lowers to a typed step and each variable reference becomes a data-flow
   edge — a 7-step, 6-edge graph: `load_dataset`/`load_dataset_2` → `Connect`,
   `featurize` → `Clean`, `train` → `Model`, `build_index` → `Index`, `serve` →
   `Serve`, `notify` → `Act`, with the two sources fanning into `featurize` and a
   linear chain from there.
2. **Gate.** [`default_gate()`](../../../pipeline/gate.md) must pass (RULE-1).
3. **Run.** `Node::run_graph` executes the graph on an ephemeral scratch node to a
   `Succeeded` Run.
4. **Done.** Every declared expectation held.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Example scripts → Churn pipeline (full taxonomy)**, then click **Run** (this case
takes no options). The result pane shows pass/fail, the metrics (steps, edges, gate
checks, run status, outputs), and the log.

**Headless:** the example cases are exercised by
`tests/testkit/test_testkit.cpp::ExampleScriptCasesRunGreen` and by
`tests/testkit/test_example_scripts.cpp`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ExampleScriptCasesRunGreen|ExampleScripts' --output-on-failure
```

### Options

This case takes no options.

## What it asserts (pass criteria)

- **Lowered shape** — 7 steps and 6 edges; each verb lowered to the expected
  `StepKind` (`load_dataset`/`load_dataset_2` → `Connect`, `featurize` → `Clean`,
  `train` → `Model`, `build_index` → `Index`, `serve` → `Serve`, `notify` → `Act`),
  and every expected data-flow edge present (the two sources → `featurize`, then
  `featurize → train → build_index → serve → notify`).
- **Gated** — `default_gate()` passed.
- **Ran green** — the run reached `Succeeded` and produced `dataset`, `model`, and
  `index` outputs.

## Related

- Engine: [example-scripts.md](../../example-scripts.md) ·
  [python-frontend.md](../../../pipeline/python-frontend.md) · [gate.md](../../../pipeline/gate.md) ·
  [execution.md](../../../pipeline/execution.md)
- Sibling cases: [`examples.minimal_train`](examples.minimal_train.md) ·
  [`examples.method_chain`](examples.method_chain.md) ·
  [`examples.uc1_at_scale`](examples.uc1_at_scale.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md)
