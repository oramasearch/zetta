# Pipeline interfaces

Project graph mutations emit audit records through their owning node. Peer
reconciliation does not emit them again; audit travels through the independent
[segment log](../../../docs/governance/audit-trail.md) (#1164).
