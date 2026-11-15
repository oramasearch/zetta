# Pipeline interfaces

Project graph mutations emit audit records through their owning node. Peer
reconciliation does not emit them again; audit travels through the independent
[segment log](../../../docs/governance/audit-trail.md) (#1164).

Project and Pipeline renames notify the owning Node so Run, Model, and Study
display names remain current (#1184). See [Runs](../../../docs/pipeline/runs.md).

Runs produced from text-keyed datasets persist the collision-checked derived-key
map so durable model and solution records can restore the original business id.

Graph execution may continue a caller-supplied Run. Study trials use that path
so admission, graph logs, metrics, lineage, and the trained model share one
durable Run id (#1205).

Run metric writes commit caller values and automatic system samples atomically
on the owner thread. Only automatic samples advance past a conflicting step;
caller values retain strict validation. See
[Run metrics](../../../docs/ml/run-metrics.md) (#1205).

`deployment_credentials.hpp` resolves a target's `credentials_ref` through the
saved-connection store for the deploying principal, failing closed (#1315).

`step_authority.hpp` decides, per step and when the step is reached, whether the
run's principal may run it: deny-by-default, from the principal's roles or
AccessKey capabilities, each decision a `data_access` audit event.
`GraphExecutor::run_as` applies it to a walk. See
[step authority](../../../docs/governance/step-authority.md) (#843). The Run
records the principal (`RunValue::principal`), and a continued Run executed
without `run_as` runs as it (#1322).
