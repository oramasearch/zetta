---
name: zetta-walkthrough
description: Run the full product walkthrough of Orama Studio and manage both avenues onto it — Claude Code driving the node over MCP, and the Playwright suite driving the browser — merging both into one dated walkthrough report of what worked, what was awkward, and what lied. Use when asked to "walk through zetta", "run the zetta walkthrough", "test all the features from Claude", "walk the demo script", "does this actually work end to end", or before claiming a release, a demo, or an epic is done. Complements mcp-journeys: that one authors and runs journey tables (layers 1-2); this one runs the walkthrough whose findings become those tables.
---

# zetta-walkthrough

**Walk the product. Not the test suite.**

The test suite answers *do the pieces work*. This answers *can a person get from
nothing to an answer* — a different question, and the one that has been wrong
every time it has been checked.

This skill **manages the whole walkthrough**: it drives one avenue itself, runs
the other, and merges both into a single report. Coverage is the union.

## Two avenues onto the same product

| Avenue | Driven by | Sees what the other cannot |
|---|---|---|
| **MCP** (`POST /mcp`) + REST `/v1` | **Claude Code, directly** | the tool surface an agent or IDE gets: schemas, error text, whether a sequence is awkward, whether a mutation has a read-back |
| **Browser** | the **Playwright suite** (`orama-web-e2e`, [#970](https://github.com/oramasearch/zetta/issues/970)) | what a person actually looks at: rendering, empty states, light/dark, narrow viewports, console errors, and whether a page **claims something the engine never said** |

Neither is sufficient. A capability can be perfect over MCP and unreachable in the
UI; a page can render beautifully and show a number the engine never computed.

They also share one capability registry, so the same fact should read the same in
both. Where they disagree, one of them is wrong — record it.

## The rule that makes this worth doing

> **Not done when the pieces pass in the test harness.**
> — [#960](https://github.com/oramasearch/zetta/issues/960)

Every green suite in this repo has coexisted with a broken product at least once: a
checkpoint emitting perfect tool calls that measured 0% parse rate, a smoke test
reporting all beats passed while the agent answered fluent nonsense, an epic whose
status table said "works" for a leg nobody could walk. **Assume the suite is lying
to you about the experience and go find out.**

## Before you start

Read [`reference.md`](reference.md) — the operational handles and the coverage
matrix. Two handles have already cost real time:

- **`--listen` sets the PEER port, not the API port.** API and MCP stay on **4800**.
  Moving the cluster port and then curling it returns nothing, which reads exactly
  like a dead node.
- **The owner token prints once and the log write races your grep.** Extract it
  *after* the API answers, and check the length is 64.

## Instructions

### 1 · Start a node, record the build

```sh
git rev-parse --short HEAD            # the report cites this
./build/bin/zetta --data-dir <scratch>/data > <scratch>/node.log 2>&1 &
```

Headed by default — **run headed unless told otherwise**, so the GUI is there to
check that externally-initiated actions render live
([#620](https://github.com/oramasearch/zetta/issues/620)).

### 2 · Avenue A — walk it yourself over MCP

Pick a real outcome — *"find out which of my accounts are about to leave, and
why"* — and pursue it. **Do not walk a checklist of capability calls.** The matrix
in `reference.md` is for measuring coverage afterwards, not for driving.

Record a finding every time you flinch (see §4).

### 3 · Avenue B — run the browser suite

```sh
cd modules/orama-web-e2e && npx playwright test --reporter=list
```

Point it at the node you started. Keep the **trace** — it is an artifact you can
read, which is why the project is Playwright and not Cypress
([#970](https://github.com/oramasearch/zetta/issues/970)).

**Until that repo exists**, drive the browser directly with the
`mcp__claude-in-chrome__*` tools and walk the same routes by hand — then say in the
report that avenue B was manual, so nobody reads it as suite-backed.

The browser avenue owns: every `?view=` route renders · nav marks the active item ·
no console errors · **no external asset requests** (the self-contained page rule) ·
light + dark · 375px · and the design-parity diff.

### 4 · Record a finding every time you flinch

The findings are the deliverable. Four kinds, all product bugs, not test failures:

| Finding | Looks like |
|---|---|
| **Unstated prerequisite** | the script says "ask in plain language" and a fresh node has 0 agents |
| **Awkward sequence** | three calls and a uuid you had to save, to do one obvious thing |
| **Missing read-back** | a mutation succeeded and nothing confirms what changed |
| **Unactionable error** | a reason that does not say what to do, or names an internal cause |

And the fifth, which outranks them all: **anything on screen that is false.** A
badge claiming provenance it does not have, a metric labeled "quality" that was
measured on training rows, a number the engine never computed. A wrong answer gets
checked; a wrongly-labeled one does not.

Tag each finding with the avenue that caught it. A finding only one avenue can see
is the argument for keeping both.

### 5 · Merge into one report

`docs/reports/results/walkthrough-<YYYY-MM-DD>.md`, per the validation-report rule:

- the **commit** walked, and which avenues actually ran;
- **every step: asked, happened, verdict** — verbatim output for anything that
  failed or read oddly, because paraphrase hides the defect;
- the **coverage matrix** from `reference.md`, filled in per avenue;
- **substitutions named as substitutions** — no warehouse credentials means the
  connection step was *not walked*, never implied. Cloud providers are
  permission-gated: **never go looking for credentials to complete a step**;
- **findings**, worst first, each with the issue filed;
- **what was not walked**, and why.

A walkthrough that reports only successes was not a walkthrough.

### 6 · Promote every finding — non-negotiable

Anything found becomes a **layer-1 journey in the same change** (the mcp-journeys
promotion rule), and anything the browser found becomes a Playwright spec. A
finding filed as an issue with no test behind it gets rediscovered by the next
walk.

## The numbers rule

**Never compute a figure while writing the report.** Every number comes from the
engine — `get_run_report`, `system.*`, a capability's own output. If something
would help and the engine does not produce it, that is a gap to record, not a
number to work out. Same discipline as the **run-report** skill.

## What "it worked" has to mean

Not *the call returned 200*. For each step:

- the **read-back** confirms the change,
- **both avenues agree** about the same fact,
- and **nothing on screen claims more than the engine knows**.

## Requirements

A built `./build/bin/zetta`, `curl`, Python 3 (stdlib). For avenue B: Node and the
`orama-web-e2e` submodule, or the Chrome browser tools as the manual fallback. No
cloud credentials, ever.

## Version history

- v1.1.0 (2026-08-06): the skill manages **both** avenues — MCP driven here, the
  browser driven by the Playwright suite — and merges them into one report with a
  per-avenue coverage matrix.
- v1.0.0 (2026-08-06): initial — layer 3 of mcp-journeys as its own workflow, with
  the operational handles that cost time on the first walk and the finding taxonomy.
