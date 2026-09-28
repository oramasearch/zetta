# Compose a catalogue solution · `solution-registry`

**Issue:** https://github.com/oramasearch/zetta/issues/1102
**Persona:** An operations analyst evaluating a retention solution on landed customer data.
**Goal:** Discover the catalogue, create a retention preview, and verify what ran and what is still missing.
**Status:** not yet run

## Before starting

A running node with a customer table containing identity, outcome, time, and numeric feature columns.

## Steps

1. Ask the node to list its solutions.
   → It shows twelve catalogue modules and eight entries marked as sketches.
2. Open Churn & Retention.
   → It explains the problem, data roles, policy, measurement, guardrails, levels, and seven missing production components.
3. Create a project and choose level 1 of Churn & Retention against the customer table.
   → A preview Pipeline is created and names the three selected methodologies.
4. Read the project back.
   → The new Pipeline is present on the project and is clearly marked as a preview.
5. Run the preview and wait for it to finish.
   → The Run succeeds, lists three ordered methodology stages in lineage, and warns that production policy, action, experiment, monitoring, ledger, snapshot, and readout work was not executed.
6. Try to create Dynamic Pricing & Discount Governance.
   → The node refuses because B-1 is a sketch and states its promotion condition.

## Passes if

The tester can discover all 20 entries, complete the level-1 preview, verify three ordered stages, and distinguish preview execution from production readiness without asking a question.

## Notes for the tester

Record any field whose meaning is unclear and capture the full refusal if the sketch does not explain why it cannot be created.
