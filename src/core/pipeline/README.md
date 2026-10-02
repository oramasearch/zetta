# Pipeline implementation

Projects, graphs, deployments, and Runs form the durable pipeline execution
model. Run metrics commit caller values and automatic system samples together
on the owner thread. Automatic samples choose increasing steps per Run and
metric from persisted series; caller-supplied steps remain strict (#1205).
A trainer logging every round coalesces the Run's replicated capture to at
most one per second; the Node's tick and stop write the last one (#1217).

See [Run metrics](../../../docs/ml/run-metrics.md) and
[pipeline documentation](../../../docs/pipeline/README.md).
