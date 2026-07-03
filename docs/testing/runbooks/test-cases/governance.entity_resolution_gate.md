# Entity-resolution gate (TC-4) · `governance.entity_resolution_gate`

**Category:** Governance · **Source:** `src/core/testkit/cases_governance.cpp` · **Engine doc:** [entity-resolution-check.md](../../../use-cases/entity-resolution-check.md)

The make-or-break correctness eval (TC-4). Runs the
[correctness gate](../../../pipeline/gate.md) over a churn graph whose clean step carries an
`entity_resolution` annotation, and confirms the gate's verdict matches the
resolver that produced it: a **division-correct** resolver passes, a **conflating**
one is blocked (RULE-1).

## What it does

1. **Reads the resolver choice.** The `resolver` option picks the
   [`EntityResolver`](../../../use-cases/entity-resolution.md) strategy that produces the
   annotation: `full_name` (the default, division-correct) or `brand` (the coarse
   trap).
2. **Builds an annotated graph.** Compiles a churn skeleton with
   [`compile_intent`](../../../pipeline/intent-compiler.md), then attaches the
   `entity_resolution` annotation to its `build_features` clean step, computed
   from the [synthetic churn fixture](../../../use-cases/churn-dataset.md) under the chosen
   strategy (so "division-correct" is *earned* from the FedEx mentions, not
   asserted by construction).
3. **Runs the gate.** Runs [`default_gate()`](../../../pipeline/gate.md) over the graph and
   reads the `entity_resolution` check's status.
4. **Checks the verdict matches the resolver.**
   - **`full_name` (choice 0):** the FedEx siblings stay separate accounts, the
     check **passes**, and the gate does not block. The case passes by asserting
     the gate passed.
   - **`brand` (choice 1):** the resolver **conflates** the 5–6 FedEx
     subsidiaries onto one account, the check **fails**, and the gate **blocks
     deploy**. The case passes by asserting the gate blocked it (the failure is
     the expected, correct outcome — TC-4).

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Governance → Entity-resolution gate (TC-4)**. Pick the resolver in the
modal and click **Run**; the result pane shows pass/fail, the gate verdict
(passed/blocked) + the `entity_resolution` status (pass/fail), and the log.

**Headless:** the case is exercised **at both resolver values** by
`tests/test_testkit.cpp::GovernanceCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.GovernanceCasesPass' --output-on-failure
```

### Options

| option | default | choices | meaning |
|---|---|---|---|
| `resolver` | `full_name (correct)` | `full_name (correct)` (0) · `brand (conflates)` (1) | which resolution strategy produced the annotation |

The case is green either way: choice 0 asserts the gate **passes**, choice 1
asserts the gate **blocks** the conflating resolver.

## What it asserts (pass criteria)

- **`full_name` (choice 0):** the gate **passed**, `block_deploy` is false, and
  the `entity_resolution` check status is **pass**.
- **`brand` (choice 1):** the gate did **not** pass, `block_deploy` is true, and
  the `entity_resolution` check status is **fail** — the conflation was caught
  and deploy blocked (RULE-1).

## Related

- Engine: [entity-resolution-check.md](../../../use-cases/entity-resolution-check.md) ·
  [entity-resolution.md](../../../use-cases/entity-resolution.md) · [gate.md](../../../pipeline/gate.md)
- Sibling cases: [`governance.access_control`](governance.access_control.md) ·
  [`governance.audit`](governance.audit.md) ·
  [`governance.burst_identity`](governance.burst_identity.md)
