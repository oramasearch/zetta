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
