# Label-efficiency validation (2026-09-27)

**Subject:** M-19 `label_efficiency`, commits `f2eac8e` and `9867e8d`, on macOS Release.

## What was tested

- A planted binary threshold task with 360 rows split across two cluster members. Entropy sampling, k-center diversity, and a fixed seed built a 10-row label queue after two initial labels.
- A planted weak-supervision task with 800 rows and three independent labelling functions whose accuracies were 0.90, 0.80, and 0.70. The native label model ran 60 iterations on one member.
- The M-19 control path: land a table, create a project, compile M-19 through the methodology registry, run the graph against its connect step, wait for completion, and read metrics and both output tables from `get_run_report`.
- Gate refusals for treating weak labels as gold and for declaring an overlapping gold split.

## Measured results

| Check | Result |
|---|---|
| Active accuracy at a 12-label budget | at least 0.90 and greater than the seeded random baseline |
| Labels saved at the target accuracy | greater than zero |
| Label-model gold accuracy | at least 0.85 |
| Recovered labelling-function accuracies | each within 0.12 of its planted accuracy |
| Determinism across members | identical label queues |
| Gold-set isolation | no queued or weak-labelled entity belongs to the deterministic gold split |
| Registry/API/gate focused proof | 30 of 30 passed |

The active-learning and label-model focused proof passed 5 of 5 tests. The complete binary ran 2,113 tests: 2,099 passed and 12 were skipped. `ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad` hit the documented #959 failure. `DistributedIngest.ANonSnowflakeConfigNeverTakesTheSnowflakeProtocol` failed in the complete run because its error-string capitalization assertion observed `postgres`, then passed alone at load `{ 6.75 7.01 10.14 }`; no M-19 source touches distributed ingest.

## Verdict

The acceptance properties are demonstrated: active sampling reaches the planted target with fewer labels than random, the independence label model recovers noisy-function quality, output rows exclude gold, and the registry journey produces the label queue and weak-label tables with lineage. Multi-node statistics are native and collective; table materialization remains deliberately single-node and refuses a multi-node writer until distributed table commit support exists.
