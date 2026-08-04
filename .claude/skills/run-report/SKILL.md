---
name: run-report
description: Generate or regenerate the report a pipeline run produces — model quality, what drives the outcome, and the ranked at-risk list — from the run's facts artifact. Use when asked to "write the run report", "generate the churn report", "regenerate the report", "what did this run find", or when a run has finished and someone needs the artifact rather than the logs. Enforces the rule that makes the report trustworthy: the engine computes every number and the narrator only arranges them, verified mechanically.
---

# Run report

A run produces a **report** — how good the model is, what drives the outcome, and which
entities are most at risk. An artifact someone opens, not run logs they read
([#948](https://github.com/oramasearch/zetta/issues/948) step 4).

## The one rule

**The engine computes every number. The narrator only arranges them.**

Never compute, average, estimate, or infer a figure while writing. If something would be
useful and is not in the facts artifact, say it is not available — never supply it.

This is not caution about the model writing the prose. **The report's entire value is that
its numbers can be checked**, and every figure the narrator is free to derive is a figure a
reader has to verify twice. One invented number costs more than the report is worth.

The rule is enforced mechanically — see step 4. Do not skip it.

## Instructions

### 1 · Get the facts artifact

If a run has produced one, use it. To generate one for the churn demo:

```sh
python3 scripts/gen_motive_demo_dataset.py --out data/demo/motive --accounts 15000
ZETTA_MOTIVE_DEMO_DIR=data/demo/motive ./build/tests/zetta-tests \
    --gtest_filter='MotiveDemoDataset.*'
```

Writes `data/demo/motive/run-report.json`. Full detail:
[`docs/runbooks/regenerating-the-run-report.md`](../../../docs/runbooks/regenerating-the-run-report.md).

The artifact carries `dataset`, `model` (both AUCs), `drivers`, and `at_risk` with per-entity
`evidence`. **If a field you want is missing, that is a gap in the artifact to fix in the
engine — not a number to work out yourself.** `majority_class_accuracy` exists for exactly
that reason: the prose needed it, computing it was forbidden, so the artifact grew.

### 2 · Read the prompt

[`docs/pipeline/run-report-prompt.md`](../../../docs/pipeline/run-report-prompt.md) holds the
prompt and the reasoning behind each restriction. Follow it exactly; every clause in it is a
failure this class of system actually produces.

### 3 · Write the report

Five sections: what the run produced · how good the model is · what drives the outcome · who
is most at risk · what to check before acting. Under 800 words.

The four that are easiest to get wrong:

- **Held-out figure first, and say what it was measured on.** Never present the in-sample
  number alone — it is the standard way a model flatters itself. State both and say what the
  gap between them means.
- **A driver is a weight, not a cause.** "Carries the most weight in the model", never
  "causes churn".
- **Do not editorialise quality.** "0.73 held out" is a fact; "excellent" is a comparison you
  were not given. Explaining a consequence is not editorialising; grading is.
- **Every figure gets a sentence saying what it means.** A table of numbers is a lab result.
  Translate the metric into something a reader can picture, give the in-sample/held-out gap a
  counterexample, and put any rate-like metric beside its trivial baseline — accuracy without
  `majority_class_accuracy` is misleading by omission.
- **Do not recommend actions.** The report says what was found; a playbook is a separate
  artifact.

Write to `docs/reports/results/<subject>-run-report-<YYYY-MM-DD>.md`.

### 4 · Verify every number traces — required

```sh
python3 .claude/skills/run-report/scripts/verify_report_numbers.py \
    docs/reports/results/<report>.md data/demo/motive/run-report.json
```

Exit 0 means every figure in the prose maps to the artifact. Any output is either a number
that was computed while writing, or a field the artifact should carry and does not. **Both
need fixing before the report ships** — do not lower the check to make it pass.

Checking by eye does not work here: a plausible invented figure looks exactly like a real
one, which is the whole reason the script exists.

### 5 · If the numbers moved, refresh everything together

The run report, the summary report, both diagrams, and the folder index quote the same
figures. **A mixed set is worse than a stale one** — a reader cannot tell which numbers are
current. This has already gone wrong once: a driver table survived a rerun at a different
account count and reversed a claim about account size.

State the account count wherever a figure appears, since the figures move with it.

## Example

A generated report:
[`docs/reports/results/motive-churn-run-report-2026-08-04.md`](../../../docs/reports/results/motive-churn-run-report-2026-08-04.md).
It passes step 4 against `data/demo/motive/run-report.json`.

## Requirements

Python 3 (stdlib only), a built `zetta-tests`, and `graphviz` if diagrams need re-rendering.

## Version history

- v1.0.0 (2026-08-04): initial — the facts artifact, the prompt, and the mechanical
  number-tracing check.
