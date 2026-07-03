# Rejected: no pipeline steps · `examples.no_pipeline_steps`

**Category:** Example scripts · **Source:** `src/core/testkit/cases_examples.cpp` · **Engine doc:** [example-scripts.md](../../example-scripts.md)

A **negative** example: a script with no calls has nothing to lower, so it is
rejected at compile. The case **passes by failing exactly as declared** — the harness
proves the failure surfaces with the reason.

## What it does

The example harness runs the script through the compile phase (see
[example-scripts.md](../../example-scripts.md)):

1. **Compile.** The [Python frontend](../../../pipeline/python-frontend.md) lowers each call to a
   typed step, but this source has only a literal assignment:

   ```python
   threshold = 0.5
   ```

   There is no call to lower into a [step](../../../pipeline/execution-graph.md), so the frontend
   rejects it (non-throwing — an error string, not an exception). The script declares
   `compiles = false` with `failure_contains = "no pipeline steps"`.
2. **Match.** The harness checks that the failure happened at compile and that the
   error reason contains the declared substring.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Example scripts → Rejected: no pipeline steps**, then click **Run** (this case
takes no options). The result pane shows pass (it failed as expected), the metrics
(phase, reason), and the log.

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

- **Failed at compile** — the script did not compile (it was declared not to).
- **Right reason** — the compile error contains `"no pipeline steps"` (a mismatched
  reason would itself fail the case).

The case is **green** precisely because a script with nothing to run was rejected the
way it should be (the harness inverts the verdict for a declared-negative example).

## Related

- Engine: [example-scripts.md](../../example-scripts.md) ·
  [python-frontend.md](../../../pipeline/python-frontend.md)
- Sibling cases: [`examples.unterminated_string`](examples.unterminated_string.md) ·
  [`examples.minimal_train`](examples.minimal_train.md)
