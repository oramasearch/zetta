# Methodology integration (#1082)

The deploy branch combines the registry with M-3, M-7, M-4, M-2, M-5,
M-6, M-8 and M-9, followed by M-12, M-13, M-11 and M-10 in round two.
All twelve source histories remain represented by merge commits. M-1 through
M-13 are runnable; later methodologies retain their unsupported status and issue links.

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
- All eighteen default gate checks are retained in methodology order after
  the shared checks. Discovery requires all thirteen implemented methodologies
  runnable. Unsupported examples use M-21 (`online_learning`, tracked by #1060),
  including the graph-check refusal test; forecasting is now runnable and is
  no longer a valid unsupported-method fixture.
- The round-two test audit checks all test files for stale unsupported or
  non-runnable assertions about M-2 through M-13. Both implemented-set tests
  now require graph ML and use M-21's `online_learning` as their unsupported
  example. Refusals for invalid graphs, unsafe parameters, unauthorized input,
  and unmatched intent remain unchanged.
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
- The forecast branch's equivalent registry commit does not create duplicate
  entries. Documentation rows match the combined registry. Generated schemas
  are regenerated from code, not combined by hand.

M-10 and M-11 add independent LayerNorm and patch-embedding autograd operations;
both implementations and their gradient checks are retained. Report schemas keep
all method-specific fields without duplicate property names. No runner behavior
is changed beyond resolving the integration conflicts.

## Reviewable diff

The integration was rebuilt from the registry base without running any
formatter. Conflict edits preserve the surrounding layout. Before publication,
each file's added/deleted lines are compared with the sum of its changes across
the twelve source branches; only small integration documentation and regression
coverage may exceed that sum. The issue comment records the complete audit,
the requested shared-file statistics, and normal versus whitespace-ignored totals.

## Verification

Use a fresh native Release build after combining headers and CMake lists:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 6 --target zetta-tests zetta
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='*Schema*'
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='OpenApi.*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*:*Gate*:IntentCompiler*:*Segment*:*Forecast*:*Anomal*:*Entity*:*Surviv*:*Uplift*:*Causal*:*NextBest*:*Rank*:*Lambda*:*Language*:*Document*:*Graph*:*Vision*'
./build/tests/zetta-tests
```

`OpenApi.*` additionally selects the committed OpenAPI artifact test, which is
not selected by `*Schema*`. Regeneration combines LambdaMART's objective enum, all method-specific graph
annotations, and every method's report properties; the issue comment records
the current proof output. Run one suite at a time. Postgres-backed tests use
a fresh local database. Full-suite-only unrelated failures must pass alone;
record both outputs and host load in the issue comment. Linux proof is deferred
when the off-host helper is unavailable. Image builds are deferred on this host;
no container-based proof is used.
