# Cluster interfaces

`Cluster` exposes peer audit protocol versions and bounded owner-thread audit
message exchange. `ReplicationManager` restores local actor clocks and retires
legacy audit history without retaining per-event tombstones (#1164).
See [replication](../../../docs/cluster/replication.md).
`ClusterSecurity::retired_join_tokens` lists the tokens a re-keyed node ran
under before (at most four, newest first), so the connector-credential sealing
key derived from an old token still opens and re-seals (#1230). See
[transport security](../../../docs/cluster/security.md).
