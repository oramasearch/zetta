# Cluster docs

The equal-peer cluster: leaderless TCP membership that self-heals, Automerge-CRDT replication
("EVERYTHING is replicated state" — the document is the source of truth, disk is a projection), and
the ephemeral signal plane the distributed-compute pass rides on (RPC/events → collectives →
telemetry). Read here for anything touching membership, gossip, replication semantics, or cross-node
messaging; the compute layers built on these seams live in [../compute/](../compute/job-coordinator.md).

- [cluster.md](cluster.md) — equal peers: running nodes, clean refusal of an occupied port, connect commands, self-healing links, protocol.
- [security.md](security.md) — inter-node mTLS + per-node identity + the token-gated join (CTL-2/TB-3): the join token derives the cluster CA, every link is mutual TLS 1.3 (`zetta::cluster::ClusterSecurity`, #466); includes how provisioned / burst nodes get the token (`ZETTA_CLUSTER_TOKEN`) so they join a secure cluster (#670).
- [replication.md](replication.md) — Automerge-CRDT replicated state, policies (replicate-to-all), actor identity, the consistency model, and how a read uses the shared view and key ranges instead of copying the catalogue (#1227).
- [../governance/audit-trail.md](../governance/audit-trail.md) — audit segment-log replication, protocol refusal, legacy compaction, per-origin ordinary/summary watermarks, and replicated explicit purge (#1164, #1169).
- [solution-reconciliation.md](solution-reconciliation.md) — incremental solution projections, direct-read records, parent ordering, and one-tick tests (#1128).
- [signal-plane.md](signal-plane.md) — the ephemeral signal plane (`zetta::cluster::Signal`): RPC / events / tasks addressed by node id, schema-validated at the boundary (ADR-28).
- [collectives.md](collectives.md) — allreduce / broadcast / barrier / gather over the signal plane, deterministic across node order (`zetta::cluster::Collectives`, #365).
- [node-metrics.md](node-metrics.md) — live per-node host and process resource
  telemetry plus pub/sub dispatch pool sizing: `system.node_metrics`, ephemeral
  + observed, no CRDT backing (`zetta::cluster::NodeMetricsSampler`, #581,
  #1157, #1163).
