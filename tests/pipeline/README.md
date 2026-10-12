# Pipeline tests

These native tests cover project graphs, execution, deployment, and durable
Runs. `test_run.cpp` exercises metric persistence, replication, curves, privacy
suppression, and live topics. Automatic sample regressions cover duplicate
natural steps, initial/trainer contention through the owner queue, independent
steps per Run and metric across restart, strict caller validation, overflow,
and agreement between published and stored steps (#1205).

Focused proof: `./build/tests/zetta-tests --gtest_filter='RunMetrics.*'`.
See [Run metrics](../../docs/ml/run-metrics.md) for the storage and event contract.
