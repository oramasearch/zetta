# Pipeline implementation

Projects, graphs, deployments, and Runs form the durable pipeline execution
model. Run metrics commit caller values and automatic system samples together
on the owner thread. Automatic samples choose increasing steps per Run and
metric from persisted series; caller-supplied steps remain strict (#1205).
A trainer logging every round coalesces the Run's replicated capture to at
most one per second; the Node's tick and stop write the last one (#1217).

`executor.cpp` authorizes each step for the run's principal before it executes
when one is set, through `step_authority.cpp` (deny-by-default, audited as
`data_access`; [step authority](../../../docs/governance/step-authority.md), #843).
It records that principal on the Run, and a Run executed again without one runs
as the principal it records (#1322).

`deployment_controller.cpp` records each run it starts or self-heals as
`data_changed` on `run:<id>`; from `Node::tick` it is the node's own work,
`node:<node id>` through `internal` (#1327).

See [Run metrics](../../../docs/ml/run-metrics.md) and
[pipeline documentation](../../../docs/pipeline/README.md).
