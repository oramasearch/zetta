# Solution integration: pass two

Refs #1122 and #1128. This pass starts from the S-1 integration branch after
the first landing (#1152), preserving S-1, S-2, S-3, S-4, and S-12.

## Merge ledger and resolutions

1. S-5 Demand Forecasting (#1150): retain all four forecast domain checks,
   executable defaults, hierarchy binding, real M-4 output selection,
   per-series uncertainty decisions, protected planner reports, guardrails,
   registry tests, and the full journey. Existing module score bindings,
   uplift handling, claim safeguards, tests, and documentation remain intact.
   The gate now has 55 checks; both count assertions and the ordered-name
   regression reflect the registration list.

Each incoming branch is joined with a merge commit. No formatter is run.
After each merge, the per-file line counts are audited against both parents.

## Proof contract

The completed integration requires a freshly configured native static build
and the full `zetta-tests` suite. Every solution registry and journey test must
pass. Only the known #959 and #1124 families may be waived; another failure
blocks delivery. Shared-library proof is not required (#1096).

The final counts, commands, and any accepted waiver evidence will be recorded
here after all requested branches and `dev` have been merged.
