# Syntax error: unterminated string · `examples.unterminated_string`

**Category:** Example scripts · **Source:** `src/core/testkit/cases_examples.cpp` · **Engine doc:** [example-scripts.md](../../example-scripts.md)

A **negative** example: an unterminated string literal must fail at the compile
phase. The case **passes by failing exactly as declared** — the harness proves the
failure surfaces with the parser's offending-statement locator.

## What it does

The example harness runs the script through the compile phase (see
[example-scripts.md](../../example-scripts.md)):

1. **Compile.** The [Python frontend](../../../pipeline/python-frontend.md) is non-throwing, so a
   malformed source returns an error string rather than an exception:

   ```python
   ds = load('unterminated
   ```

   The string literal is never closed, so the parser fails at the `load(...)`
   statement. The script declares `compiles = false` with `failure_contains =
   "unterminated string"`.
2. **Match.** The harness checks that the failure happened at compile and that the
   error reason contains the declared substring, then records the offending
   statement (the parser's own locator).

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Example scripts → Syntax error: unterminated string**, then click **Run** (this
case takes no options). The result pane shows pass (it failed as expected), the
metrics (phase, offending statement), and the log.

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
- **Right reason** — the compile error contains `"unterminated string"` (a mismatched
  reason would itself fail the case).
- **Offending statement surfaced** — the parser's locator names the statement at
  fault, so the failure reads back to the line a script author wrote.

The case is **green** precisely because the bad input was rejected the way it should
be (the harness inverts the verdict for a declared-negative example).

## Related

- Engine: [example-scripts.md](../../example-scripts.md) ·
  [python-frontend.md](../../../pipeline/python-frontend.md)
- Sibling cases: [`examples.no_pipeline_steps`](examples.no_pipeline_steps.md) ·
  [`examples.minimal_train`](examples.minimal_train.md)
