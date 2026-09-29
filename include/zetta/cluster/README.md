# Cluster interfaces

`Cluster` exposes peer audit protocol versions and bounded owner-thread audit
message exchange. `ReplicationManager` restores local actor clocks and retires
legacy audit history without retaining per-event tombstones (#1164).
See [replication](../../../docs/cluster/replication.md).
