# Compose a catalogue solution · `solution-registry`

**Issues:** https://github.com/oramasearch/zetta/issues/1102, https://github.com/oramasearch/zetta/issues/1111, https://github.com/oramasearch/zetta/issues/1114, https://github.com/oramasearch/zetta/issues/1115, https://github.com/oramasearch/zetta/issues/1116, https://github.com/oramasearch/zetta/issues/1117, https://github.com/oramasearch/zetta/issues/1118, https://github.com/oramasearch/zetta/issues/1119, https://github.com/oramasearch/zetta/issues/1120, https://github.com/oramasearch/zetta/issues/1128, https://github.com/oramasearch/zetta/issues/1175, https://github.com/oramasearch/zetta/issues/1176, and https://github.com/oramasearch/zetta/issues/1177
**Persona:** An operations analyst evaluating a retention solution on landed customer data.
**Goal:** Discover executable defaults, run a governed retention Pipeline, and read its decision and outcome evidence.
**Status:** automated native journey passes; human walkthrough not yet run

## Before starting

A running node with a customer table containing identity, outcome, time, numeric
features, M-2 identity roles, and explicit `in_collections`, `open_complaint`, and
`vulnerable` flags. Use an approved test action destination and owner, an explicit
snapshot cutoff, and supplied ROI economics. The complete input contract is in
[executable solution pipelines](../../solutions/executable-pipelines.md).

## Steps

1. Ask the node to list its solutions.
   → It shows 15 catalogue modules and five entries marked as sketches.
2. Open Churn & Retention.
   → It returns typed policy, experiment, measurement and guardrail defaults,
   `runnable: true`, and an empty `missing_components` list. S-2 through S-12
   and B-1 through B-3 also return complete policies and measurement designs.
3. Create a project and choose level 1 of Churn & Retention against the customer table, supplying the foundation inputs above.
   → A Pipeline is created with three selected methodologies and real policy, experiment, monitor, and readout IDs; `preview` is false.
4. Read the project back.
   → The new Pipeline is present. `get_policy`, `get_experiment`, `get_monitor`, and `get_roi_readout` resolve its foundation records.
5. Run the solution and wait for it to finish.
   → The Run succeeds with snapshot, three methodology, and five foundation lineage entries. Its decisions output is SQL-readable. Held-out entities have a recorded cell, no actionable decision, and no delivered task; all decisions have ledger evidence.
6. Record observed `retained` outcomes against ledger action IDs and read the ROI readout again.
   → The readout uses the protected cells and named outcome/window. Missing or inadequate evidence is explicitly non-claimable; the synthetic automated journey recovers a planted 0.2 effect inside its 95% confidence interval.
7. Open Dynamic Pricing & Discount Governance.
   → B-1 reports `status: catalogue`, `runnable: true`, and retains its promotion
   condition as a go-to-market condition. Its policy includes the margin floor,
   hard authority band, and approval threshold above the automatic band.
8. Run B-1 level 4 on synthetic pricing history with randomized treatment.
   → M-4, M-6, and M-16 run; margin and hard-band bad cases are blocked,
   above-automatic-band price changes await approval, and protected holdouts do
   not reach the connector. Recorded net-margin outcomes recover the planted
   effect inside the readout confidence interval.

## Passes if

The tester can discover all 20 entries, complete the level-1 execution, read the
versioned policy and experiment, verify holdout protection and ledger evidence,
and distinguish measured results from missing evidence without asking a question.

## Notes for the tester

Record any field whose meaning is unclear and capture any refusal that does not
identify the violated pricing or data rule.
