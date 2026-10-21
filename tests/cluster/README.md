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
