# Solution integration: pass three

Refs #1122 and #1128. This pass starts from the integration branch after
passes one and two (#1152 and #1159), preserving S-1 through S-9 and S-12.

## Merge ledger

1. S-10 Fraud & Financial-Crime Decisioning (#1155): preserve four domain
   checks, request-time feature parity, fraud-specific score output, complete
   policy defaults, authorization and case payloads, protected experimental
   cells, monitor observations, and all registry and journey tests. The
   correctness gate contains 72 checks; both ordered inventories retain every
   check from earlier passes before adding S-10 after S-9.
2. S-11 Next-Best-Action (#1156): preserve its three domain checks, complete
   templates, M-8 candidate grouping, M-15 decision-key binding, experiment
   arbitration, action-channel evidence, propensity logging, monitor metrics,
   and registry and journey tests. The gate now contains 75 checks, with the
   S-11 checks following S-10 in both ordered inventories. Earlier modules keep
   their specialized forecast, survival, uplift, and fraud handling. All twelve
   catalogue modules have complete templates; the eight sketches are unchanged.

Module branches are joined with merge commits. Per-file line counts are
audited after each merge. No formatter is run. The integration adds no new
JSON or wire format; generated schemas remain derived from the native code.

## Proof contract

The delivery proof is a fresh native static build and the full `zetta-tests`
suite. Every solution registry and journey test must pass. Only the known
#959 and #1124 multi-member families may be waived.

The per-merge focused command is:

```sh
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_brief=1 --gtest_filter='SolutionRegistry.*:SolutionJourney.*:CorrectnessGate.*:GraphChecksThroughGate.*:Schemas.*:OpenApi.*'
```

| Merge | Tests | Result |
|---|---:|---|
| S-10 | 69 from 6 suites | 69 passed |
| S-11 | 73 from 6 suites | 73 passed |

Schema regeneration produced no changes. The test inventory retains every
registry and action-journey test from both module branches, without duplicates.
