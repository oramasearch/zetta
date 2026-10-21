# Pipeline tests

These native tests cover project graphs, execution, deployment, and durable
Runs. `test_run.cpp` exercises metric persistence, replication, curves, privacy
suppression, and live topics. Automatic sample regressions cover duplicate
natural steps, initial/trainer contention through the owner queue, independent
steps per Run and metric across restart, strict caller validation, overflow,
and agreement between published and stored steps (#1205).

Focused proof: `./build/tests/zetta-tests --gtest_filter='RunMetrics.*'`.
See [Run metrics](../../docs/ml/run-metrics.md) for the storage and event contract.

`test_step_authority.cpp` proves per-step authority (#843): a step the run's
principal may not run is refused and never executes (through `GraphExecutor`,
`Node::run_graph`, the governed console and the API's `run`), connect steps are
checked against the table they name, and each decision is audited. Focused proof:
`./build/tests/zetta-tests --gtest_filter='StepAuthority*'`.
