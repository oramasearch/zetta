# Cluster docs

The equal-peer cluster: leaderless TCP membership that self-heals, Automerge-CRDT replication
("EVERYTHING is replicated state" — the document is the source of truth, disk is a projection), and
the ephemeral signal plane the distributed-compute pass rides on (RPC/events → collectives →
telemetry). Read here for anything touching membership, gossip, replication semantics, or cross-node
messaging; the compute layers built on these seams live in [../compute/](../compute/job-coordinator.md).

- [cluster.md](cluster.md) — clustering equal peers: running multiple nodes, the connect command, self-healing links, the protocol.
- [replication.md](replication.md) — Automerge-CRDT replicated state, policies (replicate-to-all), actor identity, and the consistency model.
- [signal-plane.md](signal-plane.md) — the ephemeral signal plane (`zetta::cluster::Signal`): RPC / events / tasks addressed by node id, schema-validated at the boundary (ADR-28).
- [collectives.md](collectives.md) — allreduce / broadcast / barrier / gather over the signal plane, deterministic across node order (`zetta::cluster::Collectives`, #365).
- [node-metrics.md](node-metrics.md) — live per-node resource telemetry: `system.node_metrics`, ephemeral + observed, no CRDT backing (`zetta::cluster::NodeMetricsSampler`, #581).
