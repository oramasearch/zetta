# Compute interfaces

Job lifecycle changes emit audit records only at their local source. Peer
reconciliation does not emit them again; audit travels through the independent
[segment log](../../../docs/governance/audit-trail.md) (#1164).
