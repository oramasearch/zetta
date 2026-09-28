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
2. S-7 KPI Anomaly & Driver Analysis (#1149): retain all three anomaly domain
   checks, defaults, exact alert selection, flagged-only forum tasks, driver
   caveats, decision-log evidence, phased readout, and registry/journey tests.
   Report dispatch now keeps S-1 driver, S-3 allocation, S-5 planner, and S-7
   forum reports. S-7's M-4 source handling is scoped to S-7 so S-5 retains
   its snapshot annotations. Both ordered-name regressions list all 58 checks,
   including the four S-5 checks.
3. S-8 Predictive Maintenance (#1151): retain censoring, sensor-drift, and
   post-failure leakage checks, complete maintenance defaults, M-5 horizon
   selection, CMMS payloads, six monitor observations, and the planted-effect
   journey. The optional positive decision-source horizon extends the existing
   foundation schema; OpenAPI is regenerated from code after merging `dev`.
   Gate count: 61.
4. S-9 Automated Outbound (#1148): retain consent/suppression and contact-cap
   checks, positive-uplift targeting from level 2, outbound defaults and score
   artifacts, connector evidence, guardrails, and all registry/journey tests.
   M-1 combines the outbound shape with lead conversion, continuous value,
   collections, and claims. Result naming uses explicit graph attributes,
   then regression defaults or coordinated-run options, retaining every
   module's database and columns. Outbound and value consent counters stay
   separate. Both gate regressions enumerate all 64 checks.
5. `dev`: retain the deployed pub/sub and connection-admission behavior plus
   the newer durable Run metrics, private-training reporting, Qwen step
   callbacks, and AutoML studies. The code-map conflict keeps both the
   outbound foundation entry and GBDT metric-callback description. No runtime
   source conflict required discarding either side.

The test inventory audit retains every test from all five incoming branches
and the starting S-1 branch: 53 action-journey tests and 30 solution-registry
tests, with no duplicate names. The gate registers the new S-5, S-7, S-8, and
S-9 checks after the existing S-2, S-3, S-4, and S-12 checks and before the
methodology checks.

Each incoming branch is joined with a merge commit. No formatter is run.
After each merge, the per-file line counts are audited against both parents.

## Proof contract

The completed integration requires a freshly configured native static build
and the full `zetta-tests` suite. Every solution registry and journey test must
pass. Only the known #959 and #1124 families may be waived; another failure
blocks delivery. Shared-library proof is not required (#1096).

The static tree was configured from an empty build directory after recursive
submodule synchronization. Native build with `cmake --build build -j 6`
completed successfully. Schema regeneration and registry/gate proof:

```text
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='Schemas.*:OpenApi.*:SolutionRegistry.*:CorrectnessGate.*:GraphChecksThroughGate.*'
[==========] 51 tests from 5 test suites ran. (71 ms total)
[  PASSED  ] 51 tests.
```

This includes all 30 solution-registry tests and both complete ordered gate
lists. The full static-suite counts and any accepted waiver evidence are
recorded separately below when that run finishes.
