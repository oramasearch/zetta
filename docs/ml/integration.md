# Methodology integration (#1082)

The deploy branch combines the registry with M-3, M-7, M-4, M-2, M-5,
M-6, M-8 and M-9, followed by M-12, M-13, M-11 and M-10 in round two.
Round three adds M-14 speech, M-15 bandits and M-16 optimization. Round four begins
with M-17 Bayesian uncertainty, M-18 ensembling and M-19 label efficiency.
All eighteen source histories remain represented by merge commits.
M-1 through M-19 are runnable; later methodologies retain
their unsupported status and issue links.

## Conflict decisions

- S-11 resolves to M-8 recommendation / next-best-action, the catalogue's
  primary method using portfolio numbering. M-6 uplift supports the solution
  but does not own its lookup; select it by `M-6`, `causal` or `uplift`.
  The registry has no separate supporting-component field. A registry test
  checks that each primary solution has only one owner.
- S-10 and S-12 resolve to M-1 (tabular supervised learning); graph, ranking,
  and vision remain selectable by ID and model kind. S-8 resolves to M-7
  (anomaly detection), not M-5 survival or M-13 vision. These choices use the
  catalogue's first method, mapped to portfolio numbering by
  [buildable-models §8](https://github.com/oramasearch/zetta-bridge/blob/main/docs/solutions/buildable-models.md):
  the catalogue calls anomaly detection M-5 and survival M-6. Its
  [S-8 framing](https://github.com/oramasearch/solutions-designs/blob/main/docs/plan/04-solutions-catalog.md#111-framing-methodology-correctness)
  explicitly lists anomaly detection first.
- M-15 does not take over S-11 from M-8: it remains reachable by its ID and
  bandit model kinds. Both lookup implementations prefer runnable entries
  before planned ones; the existing equivalent lookup is retained.
- S-4 remains owned by M-1, the first method in the catalogue's collections
  row. M-16 drops that primary lookup and stays reachable by ID, `optimization`
  and `operations_research`. Its scenario, verification and solver-status
  checks and its plan/constraint reports are retained alongside every method.
- M-18 also drops its S-10 primary claim: M-1 owns the catalogue row's first
  method. Ensembling keeps its ID/model-kind lookups, out-of-fold gate, and
  independent blended-score and meta-weight outputs.
- All twenty-six default gate checks are retained in methodology order after
  the shared checks. Discovery requires all nineteen implemented methodologies
  runnable. Unsupported examples use M-21 (`online_learning`, tracked by #1060),
  including the graph-check refusal test; forecasting is now runnable and is
  no longer a valid unsupported-method fixture.
- The round-two test audit checks all test files for stale unsupported or
  non-runnable assertions about M-2 through M-13. Both implemented-set tests
  now require graph ML and use M-21's `online_learning` as their unsupported
  example. Refusals for invalid graphs, unsafe parameters, unauthorized input,
  and unmatched intent remain unchanged.
- The round-three audit extends discovery and support assertions through M-16;
  M-21 remains the unsupported fixture. The naming regression checks every
  runnable method and the M-3 capability response: default names come from
  `CompiledIntent::name`, not the legacy `IntentShape::Generic` label.
  Explicit names and M-1's `churn skeleton` remain unchanged.
- GBDT retains binary-logistic and squared-error behavior alongside the
  LambdaMART gradient callback. Forecasting and uplift retain regression.
- Reports retain every method's metrics, tables and dataset facts. Survival
  keeps feature names, drivers and exclusion notes from the shared report
  path; its journey checks those features and drivers. Output references
  accept arrays for forecasting/recommendation and objects for survival.
- Vision reports select the run's `image predictions` output through
  `run_named_table`, matching the shared helper introduced by the earlier
  integration. The audit of removed helper names found no other stale calls;
  churn/ranking retain `run_result_table` and uplift retains its named outputs.
- Speech reports likewise select `redacted transcripts` through
  `run_named_table`; consent, redaction and owner-local audio checks remain
  independent requirements. M-14 claims no primary solution lookup.
- The forecast branch's equivalent registry commit does not create duplicate
  entries. Documentation rows match the combined registry. Generated schemas
  are regenerated from code, not combined by hand.
- The combined run schema declares the shared document/bandit `seed` only
  once, with its existing non-negative integer contract. Optimization retains
  its separate nested solver seed and all LP/MIP controls.

M-10 and M-11 add independent LayerNorm and patch-embedding autograd operations;
both implementations and their gradient checks are retained. Report schemas keep
all method-specific fields without duplicate property names. No runner behavior
is changed beyond resolving the integration conflicts.

## Reviewable diff

The integration was rebuilt from the registry base without running any
formatter. Conflict edits preserve the surrounding layout. Before publication,
each file's added/deleted lines are compared with the sum of its changes across
the eighteen source branches; only small integration documentation and regression
coverage may exceed that sum. The issue comment records the complete audit,
the requested shared-file statistics, and normal versus whitespace-ignored totals.

## Verification

Use a fresh native Release build after combining headers and CMake lists:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 6 --target zetta-tests zetta
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='*Schema*'
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='OpenApi.*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*:*Gate*:IntentCompiler*:*Segment*:*Forecast*:*Anomal*:*Entity*:*Surviv*:*Uplift*:*Causal*:*NextBest*:*Rank*:*Lambda*:*Language*:*Document*:*Graph*:*Vision*:*Speech*:*Audio*:*Bandit*:*Thompson*:*LinUCB*:*Solver*:*Optim*'
./build/tests/zetta-tests
```

`OpenApi.*` additionally selects the committed OpenAPI artifact test, which is
not selected by `*Schema*`. Regeneration combines LambdaMART's objective enum, all method-specific graph
annotations, and every method's report properties; the issue comment records
the current proof output. Round-two regeneration passed all 46 schema tests
and all 3 OpenAPI tests, including graph, vision, document, and language report
fields plus vision and language correctness annotations. Run one suite at a time. Postgres-backed tests use
a fresh local database. Full-suite-only unrelated failures must pass alone;
record both outputs and host load in the issue comment. Linux proof is deferred
when the off-host helper is unavailable. Image builds are deferred on this host;
no container-based proof is used.

Round-three schema regeneration passed all 46 schema and 3 OpenAPI tests.
The artifacts now include speech privacy annotations/transcripts, bandit policy
and decision-table references, and optimization controls/plan/constraint facts.
The registry documentation and compiler/API naming regression checks also pass.
