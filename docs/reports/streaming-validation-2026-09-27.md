# M-21 streaming validation — 2026-09-27

**Version:** `issue-1060-methodology-m-21`, based on
`origin/issue-1061-methodology-registry`

**Host:** macOS, native Release build

**Result:** PASS for the M-21 implementation and integration

## What was validated

The native online logistic regression converged deterministically on a seeded
600-event stream. Its most recent 100 prequential labels reached accuracy above
0.95 and log loss below 0.20. Replaying the same input and seed produced the
same serialized model.

The two-member partition test distributed 200 events between real loopback
members. Gradient statistics were combined with `JobRound::allreduce`; both
members produced identical models and metrics, and the last 50 prequential
labels reached accuracy above 0.90.

Correctness tests verified that:

- each probability is recorded before its label updates the model;
- a label timestamp before its event timestamp is excluded;
- events behind the configured watermark are excluded;
- periodic and final model persistence callbacks run;
- accepted inputs produce score events and a lineage-bearing output table; and
- the M-21 registry journey compiles, gates, runs, reports, and reads scores.

## Commands and results

```text
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
Result: PASS

./build/tests/zetta-tests --gtest_filter='*Stream*:*Online*:*Hoeffding*:*Prequential*'
Result: 14 tests passed

./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
Result: 33 tests passed
```

Schema generation ran with `ZETTA_WRITE_SCHEMAS=1`; the schema parity test
passed. The final full suite ran 2,120 tests: 2,106 passed, 12 skipped, and two
unrelated tests failed under shared-host contention. The distributed-ingest
test observed a refused connection to its fixed test port; the known #959 mTLS
stress test did not converge before its deadline. Both tests passed together on
an immediate isolated rerun at one-minute load 4.10:

```text
./build/tests/zetta-tests --gtest_filter='DistributedIngest.ANonSnowflakeConfigNeverTakesTheSnowflakeProtocol:ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad'
Result: 2 tests passed
```

## Scope

This validates the bounded table replay runner shipped by M-21. A continuous
consumer with durable restart offsets and the optional Hoeffding tree remain
outside this implementation.
