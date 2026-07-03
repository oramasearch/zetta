# Audit trail · `governance.audit`

**Category:** Governance · **Source:** `src/core/testkit/cases_governance.cpp` · **Engine doc:** [audit-trail.md](../../../governance/audit-trail.md)

Performs two governed actions, a deploy and a pair of access decisions, then
confirms each one landed in the immutable, append-only audit trail.

## What it does

1. **Deploys a graph.** Compiles a churn skeleton with
   [`compile_intent`](../../../pipeline/intent-compiler.md) and deploys it onto a local
   [target](../../../pipeline/deployments.md) via `Node::deploy_graph`, which records a
   `graph_deployed` event.
2. **Authorizes access.** Creates a `svc` service
   [principal](../../../governance/access-control.md) with a key scoped to the `churn`
   database, then runs `Node::authorize` for the in-scope database `churn`
   (allowed) and the out-of-scope database `fraud` (denied) — each decision is a
   `data_access` event.
3. **Reads the trail.** Walks `Node::audit_events()` and counts the
   `graph_deployed` and `data_access` events.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Governance → Audit trail**. Click **Run**; the result pane shows
pass/fail, the event counts (total, `graph_deployed`, `data_access`), and the
log.

**Headless:** the case is exercised by
`tests/test_testkit.cpp::GovernanceCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.GovernanceCasesPass' --output-on-failure
```

### Options

This case takes no options.

## What it asserts (pass criteria)

- Exactly **one `graph_deployed`** event for the deploy.
- **At least two `data_access`** events for the allow + deny decisions.
- Every governed action is captured in the [audit trail](../../../governance/audit-trail.md)
  as its own immutable, append-only event.

## Related

- Engine: [audit-trail.md](../../../governance/audit-trail.md) ·
  [access-control.md](../../../governance/access-control.md)
- Sibling cases: [`governance.access_control`](governance.access_control.md) ·
  [`governance.entity_resolution_gate`](governance.entity_resolution_gate.md) ·
  [`governance.burst_identity`](governance.burst_identity.md)
