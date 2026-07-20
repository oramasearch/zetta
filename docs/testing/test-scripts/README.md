# Test scripts — manual acceptance, for humans

A **test script** is something you hand to a human tester. They follow it and tell you
whether it passed. It is a mostly high-level, human-understandable walkthrough of a **real
user pursuing a real use case**.

It is **not** a unit test, integration test, e2e test, or any programmatic check.

| | Proves | Lives in |
|---|---|---|
| Unit / integration tests | the code does what the developer intended | `tests/` |
| Testkit cases | a subsystem behaves on a real node/cluster | `src/core/testkit/`, documented in [`runbooks/test-cases/`](../runbooks/test-cases/) |
| MCP journeys (layers 1–2) | a capability sequence works over the control surface | `tests/api/`, documented in [`mcp-journeys.md`](../mcp-journeys.md) |
| **Test scripts** | **a real person trying to accomplish a real goal succeeds** | **here** |

Automated green is *evidence*. It is not acceptance. A build can be entirely green while a
customer with three bare servers still cannot get a cluster up.

## When a script is written

Every GitHub issue on [`oramasearch/zetta`](https://github.com/oramasearch/zetta) carries a
`## Test script` section. An issue may only be **closed as implemented** once a human has run
its script and it passed. See the `github-ops` and `prune-backlog` conventions.

Write the script even when the feature does not exist yet — it states, in the user's terms,
what "done" means, and it is usually the most useful thing a grooming pass produces.

## Shape

```markdown
# <Title> · `<slug>`

**Issue:** https://github.com/oramasearch/zetta/issues/<n>
**Persona:** who this person is, in one line.
**Goal:** what they are trying to achieve, in their words.
**Status:** passed <date> by <who> | failed <date> — <why> | not yet runnable — <what's missing>

## Before starting
What they need in hand. No build steps unless a tester would really do them.

## Steps
1. What they do.
   → *What they should see.*
2. …

## Passes if
One unambiguous statement. The tester answers yes or no.

## Notes for the tester
Known rough edges, what to capture if it fails, who to ask.
```

## Rules

- **User language only.** No class names, file paths, function names, API endpoints, or
  `ctest` invocations in the steps. If the tester would not type it or click it, it does not
  belong in a step.
- **Executable by someone who has never read the code.** If only the author could follow it,
  it is not a test script.
- **Every step has an observable outcome** the tester can confirm or deny.
- **One pass criterion.** Not a checklist of maybes — yes or no.
- **State the status honestly**, including "not yet runnable". A known gap is useful; silence
  is not.
- **Keep it in sync with the issue.** The script may be duplicated into the GitHub comment;
  this file is the maintained copy.

## Scripts

| Script | Issue | Status |
|---|---|---|
| [`env-3-on-prem-cluster`](env-3-on-prem-cluster.md) | [#112](https://github.com/oramasearch/zetta/issues/112) — `[ENV-3]` Customer VPC / on-prem | not yet runnable |
| [`env-4-ephemeral-burst`](env-4-ephemeral-burst.md) | [#113](https://github.com/oramasearch/zetta/issues/113) — `[ENV-4]` Ephemeral burst | not yet runnable — step 9 known flaky ([#824](https://github.com/oramasearch/zetta/issues/824)) |
| [`env-5-managed-cloud`](env-5-managed-cloud.md) | [#114](https://github.com/oramasearch/zetta/issues/114) — `[ENV-5]` Managed cloud | not yet runnable — creating a managed cluster does not exist |
