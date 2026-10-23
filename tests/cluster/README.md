# Cluster tests

Tests cover catalog convergence, durable removals and solution reconciliation.
Audit records no longer pad catalog fixtures: those use neutral fixture keys,
while audit delivery and legacy-history compaction are tested in governance
(#1164). See [the proof runbook](../../docs/testing/audit-replication.md).
The incremental-reconcile load case migrates 100,000 legacy audit records, then
checks ordinary catalog ticks while new audit records append outside the catalog.
`ReKeyingRetiresTheEarlierToken` covers the bounded retired-token list a
`--join-token` re-key keeps for the connector-credential key (#1230).
`PeerGateway.OverlappingPrivateNodesReplicateThroughOpaqueGateways` models the
two zetta-tunnel gateway hops with byte-forwarding fixtures and proves that
dial-out-only nodes with overlapping private addresses retain end-to-end mutual
TLS and bidirectional CRDT convergence (#1282).
`test_cluster_hello_bounds.cpp` covers the HELLO bounds (#771): the node-id and
`HOST:PORT` checks, the 16-endpoint cap, refusal of HELLOs carrying escape
sequences or too many endpoints over a live plaintext connection (with a
well-formed HELLO as the positive control), the 16 KiB pre-HELLO frame bound, and
the capped advertised set.
`ReplicationDelta.*` (`test_replication.cpp`) prove a gossip delta carries only the
changes a peer lacks, falls back to the whole document, reaches an audit-retired
peer, and converges a four-node mesh with stale clocks and partial views without
leaving a gap (#794). `test_gossip_bandwidth.cpp` checks that steady-state rounds
over mutual TLS ship deltas, and holds the `ZETTA_GOSSIP_BENCH=1` measurement of
bytes per round and convergence time.
