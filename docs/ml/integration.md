# Methodology integration (#1082)

The deploy branch combines the registry with M-3, M-7, M-4, M-2, M-5,
M-6, M-8 and M-9, in that merge order. All eight source histories remain
represented by merge commits. M-1 through M-9 are runnable; M-10 remains
partial and later methodologies retain their unsupported status and issue links.

## Conflict decisions

- S-11 resolves to M-8 recommendation / next-best-action, the catalogue's
  primary method using portfolio numbering. M-6 uplift supports the solution
  but does not own its lookup; select it by `M-6`, `causal` or `uplift`.
  The registry has no separate supporting-component field. A registry test
  checks that each primary solution has only one owner.
- All thirteen default gate checks are retained in methodology order after
  the shared checks. Discovery requires all nine implemented methodologies
  runnable. Unsupported examples use M-21.
- GBDT retains binary-logistic and squared-error behavior alongside the
  LambdaMART gradient callback. Forecasting and uplift retain regression.
- Reports retain every method's metrics, tables and dataset facts. Survival
  keeps feature names, drivers and exclusion notes from the shared report
  path; its journey checks those features and drivers. Output references
  accept arrays for forecasting/recommendation and objects for survival.
- The forecast branch's equivalent registry commit does not create duplicate
  entries. Documentation rows match the combined registry. Generated schemas
  are regenerated from code, not combined by hand.

## Reviewable diff

The integration was rebuilt from the registry base without running any
formatter. Conflict edits preserve the surrounding layout. Before publication,
each file's added/deleted lines are compared with the sum of its changes across
the eight source branches; only small integration documentation and regression
coverage may exceed that sum. The issue comment records the complete audit,
the requested shared-file statistics, and normal versus whitespace-ignored totals.

## Verification

Use a fresh native Release build after combining headers and CMake lists:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 6 --target zetta-tests zetta
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='*Schema*'
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='OpenApi.*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*:*Gate*:IntentCompiler*:*Segment*:*Forecast*:*Anomal*:*Entity*:*Surviv*:*Uplift*:*Causal*:*NextBest*:*Rank*:*Lambda*'
./build/tests/zetta-tests
```

`OpenApi.*` additionally selects the committed OpenAPI artifact test, which is
not selected by `*Schema*`. Regeneration passed all 46 schema tests and all
3 OpenAPI tests, combining LambdaMART's objective enum, survival/uplift graph
annotations, and every method's report properties. Run one suite at a time. Postgres-backed tests use
a fresh local database. Full-suite-only unrelated failures must pass alone;
record both outputs and host load in the issue comment. Linux proof is deferred
when the off-host helper is unavailable. Image builds are deferred on this host;
no container-based proof is used.
