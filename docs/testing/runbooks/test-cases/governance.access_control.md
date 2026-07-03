# Key-scoped access · `governance.access_control`

**Category:** Governance · **Source:** `src/core/testkit/cases_governance.cpp` · **Engine doc:** [access-control.md](../../../governance/access-control.md)

Issues a single scoped, capability-bearing access key and confirms zetta's V1
access model is **deny-by-default** along both axes: a request inherits exactly
its key's scope (which resources) and capabilities (which rights), and everything
else is denied.

## What it does

1. **Issues a scoped key.** Starts a node, creates an `analyst`
   [principal](../../../governance/access-control.md), and issues one key scoped to the
   `churn` project (the resource axis) with a single granted right,
   `orchestration:start_run` (the capability axis).
2. **Authorizes five decisions.** Runs `Node::authorize` for an **in-scope**
   resource (project `churn`), an **out-of-scope** resource (project `fraud`),
   the **granted** right (`orchestration:start_run`), an **ungranted** right
   (`api:deploy`), and an **unknown key**.
3. **Checks deny-by-default.** Confirms only the in-scope resource and the
   granted right were allowed, and the three others (out-of-scope, ungranted,
   unknown key) were denied.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Governance → Key-scoped access**. Click **Run**; the result pane shows
pass/fail, the per-decision metrics (allow/deny), and the log.

**Headless:** the case is exercised by
`tests/testkit/test_testkit.cpp::GovernanceCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.GovernanceCasesPass' --output-on-failure
```

### Options

This case takes no options.

## What it asserts (pass criteria)

- The **in-scope** resource (project `churn`) is **allowed**.
- The **out-of-scope** resource (project `fraud`) is **denied**.
- The **granted** right (`orchestration:start_run`) is **allowed**.
- The **ungranted** right (`api:deploy`) is **denied** (there is no wildcard
  capability).
- An **unknown / empty key** is **denied**.

## Related

- Engine: [access-control.md](../../../governance/access-control.md)
- Sibling cases: [`governance.audit`](governance.audit.md) ·
  [`governance.entity_resolution_gate`](governance.entity_resolution_gate.md) ·
  [`governance.burst_identity`](governance.burst_identity.md)
