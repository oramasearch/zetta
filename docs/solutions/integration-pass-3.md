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
3. Current `dev` after #1159 (`4c54eec1`): merge its history without content
   conflicts. The per-file line-count audit shows no incoming content changes.
4. Final synchronization found #1161 on `dev` (`a920271b`): preserve its
   active-topic dispatch pool, operational configuration, metrics, tests, and
   documentation. The merge is conflict-free; per-file counts match the incoming
   change. Its header changes require another empty-directory static build and
   full proof before delivery.

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

## Initial full static proof

After merging `dev`, recursive submodule synchronization completed and the
generated build directory was removed. The fresh native static build passed:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build -j 6 --target zetta-tests
```

The suite used a fresh local PostgreSQL database created for this run:

```sh
createdb -h 127.0.0.1 -U stevesperandeo zetta_issue1122_pass3_20260928_1f90d1c4
ZETTA_PG_TEST_HOST=127.0.0.1 \
ZETTA_PG_TEST_USER=stevesperandeo \
ZETTA_PG_TEST_DB=zetta_issue1122_pass3_20260928_1f90d1c4 \
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_brief=1 \
  --gtest_output=xml:build/pass3-static.xml
```

```text
[==========] 2590 tests from 522 test suites ran. (501836 ms total)
[  PASSED  ] 2574 tests.
[  SKIPPED ] 16 tests.
```

| Proof area | Passed | Failed | Skipped |
|---|---:|---:|---:|
| Full static suite | 2574 | 0 | 16 |
| Solution journeys | 13 | 0 | 0 |
| Solution registry | 39 | 0 | 0 |
| Correctness gate and ordered inventories | 14 | 0 | 0 |
| Schemas and OpenAPI | 7 | 0 | 0 |
| Local PostgreSQL connector | 5 | 0 | 0 |

Neither #959 nor #1124 required a waiver. The 16 skips are explicit optional
test conditions: ten need local model checkpoints, three need an opt-in corpus
or corpus-output path, one needs the generated Motive demo dataset, one needs
CUDA, and the no-engine fallback case does not apply when the full-text engine
is compiled in. Schema regeneration, including `schemas/openapi.json`, is
unchanged. The temporary PostgreSQL database is removed after the proof.

The native log and machine-readable report are `build/pass3-static.log` and
`build/pass3-static.xml`. All pass-three module integration work is complete.
Live lake listing/start validation remains a post-deploy part of the wider
#1122 epic; it is not claimed by this native integration proof. Band-B entries
remain sketches. No formatter or container command was run.
