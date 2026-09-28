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

## Status vocabulary

Four values, and the first two are **not** the same thing:

| Status | Means |
|---|---|
| **not yet runnable** | The capability does not exist. The script states what "done" will look like. (`env-5-managed-cloud` — creating a managed cluster is unbuilt.) |
| **not yet run** | The capability ships. Nobody has walked it. (`rel-7-gui-journey` — every panel exists; no one has tried it cold.) |
| **failed \<date\>** | Someone ran it and it did not pass. Say which step and link the issue that now carries the failure. |
| **passed \<date\> by \<who\>** | The only status that lets an issue close as implemented. |

The distinction matters when triaging: *not yet runnable* is engineering work, *not yet run*
is an afternoon and a willing person. They should not sit in the same queue.

## Designing the measurement

A script that only asks "did it work?" wastes the run. Decide in advance what you are
counting, and make it something a tester can report without interpretation:

- **Count the questions the tester had to ask.** Zero is a pass; each one is a specific
  finding. This is the single most useful measurement for any first-use journey.
- **Record stalls even when solved.** "Worked it out after two minutes" is a finding, not a
  pass.
- **Record anything they could not verify.** An answer the tester had no way to check is a
  failure of the step that produced it, even if the answer was right.
- **Record the number, not the impression** — time to first answer, actual cost, actual
  latency. These become the evidence for the claims we make publicly.

## Craft rules learned the hard way

- **A fresh tester each time.** For any first-use journey, a second run by the same person
  tests nothing — they now know the app. Value comes entirely from ignorance of it.
- **If nothing breaks naturally, break it on purpose.** Every script should exercise at least
  one failure path — a malformed input, an interrupted run — and read what the product said.
  Error copy is where polish passes quietly fail.
- **Check the world, not just the product.** Where the product's claim is about cost or
  cleanup, the step is "open the cloud console and look", not "ask the app what it did". No
  automated test can check a bill.
- **Compose; don't duplicate.** A release-gate script says "run these scripts, in these
  conditions, and here is the extra bar" — it does not restate them. Keep each journey in one
  place so it is maintained in one place.
- **A partial script is legitimate.** When scope is genuinely undefined, write the script for
  what *is* defined and say so. Testing the known escalations beats writing nothing while
  waiting for a design.

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
| [`feat-7-tiered-storage`](feat-7-tiered-storage.md) | [#138](https://github.com/oramasearch/zetta/issues/138) — `[FEAT-7]` Tiered storage | not yet runnable — no tiering exists; step 1 is today's hard wall |
| [`rel-7-gui-journey`](rel-7-gui-journey.md) | [#192](https://github.com/oramasearch/zetta/issues/192) — `[REL-7]` GUI as product surface | not yet run — all panels ship; nobody has walked it cold |
| [`rel-8-agent-learning`](rel-8-agent-learning.md) | [#853](https://github.com/oramasearch/zetta/issues/853) — per-customer fine-tune loop | not yet runnable — no training loop in the engine |
| [`feat-12-base-agent`](feat-12-base-agent.md) | [#555](https://github.com/oramasearch/zetta/issues/555) — `[FEAT-12]` Build the Orama base agent | not yet runnable — no training loop, games, or grader exist |
| [`run-1-node-bootstrap`](run-1-node-bootstrap.md) | [#215](https://github.com/oramasearch/zetta/issues/215) — `[RUN-1]` Low-friction node bootstrapping | not yet runnable — no inventory, no self-provisioning; join blocked by [#784](https://github.com/oramasearch/zetta/issues/784)/[#786](https://github.com/oramasearch/zetta/issues/786) |
| [`tc-1-gate-eval-rate`](tc-1-gate-eval-rate.md) | [#248](https://github.com/oramasearch/zetta/issues/248) — `[TC-1]` Correctness-gate eval rate | not yet runnable — no eval set, no measured rate |
| [`tc-2-no-egress-capture`](tc-2-no-egress-capture.md) | [#249](https://github.com/oramasearch/zetta/issues/249) — `[TC-2]` No egress under network capture | **Part 1 runnable now**; Part 2 needs [#75](https://github.com/oramasearch/zetta/issues/75) |
| [`tc-5-jailbreak-attempt`](tc-5-jailbreak-attempt.md) | [#252](https://github.com/oramasearch/zetta/issues/252) — `[TC-5]` Sandbox escape / over-scope denied | **fails today** — known escapes open ([#793](https://github.com/oramasearch/zetta/issues/793), [#819](https://github.com/oramasearch/zetta/issues/819), [#759](https://github.com/oramasearch/zetta/issues/759)) |
| [`tc-8-intent-to-deployed`](tc-8-intent-to-deployed.md) | [#255](https://github.com/oramasearch/zetta/issues/255) — `[TC-8]` Intent→deployed latency | not yet run — path is built, never timed |
| [`solution-registry`](solution-registry.md) | [#1102](https://github.com/oramasearch/zetta/issues/1102), [#1128](https://github.com/oramasearch/zetta/issues/1128) — executable solution catalogue and governed retention journey | automated native journey passes; human walkthrough not yet run |
